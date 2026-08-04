import * as fs from "node:fs";
import * as path from "node:path";
import { spawn } from "child_process";
import * as net from "net";
import WebSocket from "ws";
import type { ProjectContext } from "./project.js";
import { findEngineInstall } from "./deployer.js";
import { invalidatePluginFreshness } from "./plugin-freshness.js";
import { findInteractiveEditors, readEngineState, type EngineState } from "./engine-observer.js";

// Process control is cross-platform: the editor binary path and the running-
// process probe differ per OS, and stopping goes through the bridge (#790).
const IS_WINDOWS = process.platform === "win32";

const NO_EDITOR_BINARY_MSG =
  "Unreal Editor executable not found. Set UE_EDITOR_PATH to the editor binary (on macOS that is inside UnrealEditor.app/Contents/MacOS/), or install the engine to a default location.";

/** Read EngineAssociation from a .uproject, or null if unreadable. */
function readEngineAssociation(projectPath: string): string | null {
  try {
    const parsed = JSON.parse(fs.readFileSync(projectPath, "utf-8"));
    return typeof parsed?.EngineAssociation === "string" ? parsed.EngineAssociation : null;
  } catch {
    return null;
  }
}

function findUEBuildTool(engineAssociation?: string | null): string | null {
  const envPath = process.env.UE_BUILD_TOOL_PATH;
  if (envPath) return envPath;

  const scriptName = IS_WINDOWS ? "Build.bat" : "Build.sh";

  // Prefer the engine the project's EngineAssociation actually points at, so a
  // 5.7 project builds with 5.7's Build tool - not whatever version happens to
  // sort first in the fallback search below. The editor launch already respects
  // the association (findEditorExecutable); without this the CLI build could
  // silently compile against a different engine than the editor runs, masking
  // API incompatibilities until the editor's own rebuild fails.
  const associatedRoot = findEngineInstall(engineAssociation ?? null);
  if (associatedRoot) {
    const associatedTool = path.join(associatedRoot, "Engine", "Build", "BatchFiles", scriptName);
    if (fs.existsSync(associatedTool)) return associatedTool;
  }

  const versions = ["5.8", "5.7", "5.6", "5.5", "5.4", "5.3"];

  const searchRoots: string[] = IS_WINDOWS
    ? [
        "C:/Program Files/Epic Games",
        "D:/Program Files/Epic Games",
        "E:/Program Files/Epic Games",
        "C:/Epic Games",
        "D:/Epic Games",
        "E:/Epic Games",
      ]
    : process.platform === "darwin"
      ? ["/Users/Shared/Epic Games"]
      : [
          path.join(process.env.HOME ?? "/home", "UnrealEngine"),
          "/opt/UnrealEngine",
        ];

  for (const basePath of searchRoots) {
    for (const version of versions) {
      const buildToolPath = path.join(basePath, `UE_${version}`, "Engine", "Build", "BatchFiles", scriptName);
      if (fs.existsSync(buildToolPath)) {
        return buildToolPath;
      }
    }
  }

  // Linux source builds: ~/UnrealEngine/Engine/Build/BatchFiles/Build.sh (no version subdir)
  if (!IS_WINDOWS && process.platform !== "darwin") {
    const home = process.env.HOME ?? "/home";
    const sourceBuild = path.join(home, "UnrealEngine", "Engine", "Build", "BatchFiles", "Build.sh");
    if (fs.existsSync(sourceBuild)) return sourceBuild;
  }

  return null;
}

/**
 * #766/#790: the editor binary lives at a different path per platform. Only the
 * Win64 path was ever checked, which is the whole reason start_editor was
 * Windows-only - engine discovery itself (findUEBuildTool) has always worked
 * cross-platform. On macOS the launchable binary is inside the .app bundle.
 */
function editorBinaryCandidates(engineRoot: string): string[] {
  const binaries = path.join(engineRoot, "Engine", "Binaries");
  if (IS_WINDOWS) {
    return [path.join(binaries, "Win64", "UnrealEditor.exe")];
  }
  if (process.platform === "darwin") {
    return [
      path.join(binaries, "Mac", "UnrealEditor.app", "Contents", "MacOS", "UnrealEditor"),
      path.join(binaries, "Mac", "UnrealEditor"),
    ];
  }
  return [path.join(binaries, "Linux", "UnrealEditor")];
}

function findEditorExecutable(project?: ProjectContext): string | null {
  const envPath = process.env.UE_EDITOR_PATH;
  if (envPath) return envPath;

  const associatedEngineRoot = findEngineInstall(project?.engineAssociation ?? null);
  if (associatedEngineRoot) {
    for (const candidate of editorBinaryCandidates(associatedEngineRoot)) {
      if (fs.existsSync(candidate)) return candidate;
    }
  }

  const buildTool = findUEBuildTool(project?.engineAssociation ?? null);
  if (!buildTool) return null;

  const engineRoot = path.resolve(buildTool, "..", "..", "..", "..");
  for (const candidate of editorBinaryCandidates(engineRoot)) {
    if (fs.existsSync(candidate)) return candidate;
  }

  return null;
}

/**
 * #804: this used to be a `tasklist /FI "IMAGENAME eq UnrealEditor.exe"` string
 * match, which cannot tell an interactive editor apart from a headless shard
 * (`-server -game -nullrhi`) or from an editor holding a completely different
 * project. Two shards on the box made start_editor refuse with "Editor is
 * already running" while get_status reported nothing connected. The probe now
 * matches on PID + command line and scopes to the project being asked about.
 */
async function isEditorRunning(projectPath?: string | null): Promise<boolean> {
  return (await findInteractiveEditors(projectPath ?? null)).length > 0;
}

async function isBridgeAvailable(host = process.env.UE_MCP_HOST ?? "127.0.0.1", port = 9877, timeoutMs = 1000): Promise<boolean> {
  return new Promise((resolve) => {
    const socket = new net.Socket();
    let resolved = false;

    const timer = setTimeout(() => {
      if (!resolved) {
        resolved = true;
        socket.destroy();
        resolve(false);
      }
    }, timeoutMs);

    socket.once("connect", () => {
      if (!resolved) {
        resolved = true;
        clearTimeout(timer);
        socket.destroy();
        resolve(true);
      }
    });

    socket.once("error", () => {
      if (!resolved) {
        resolved = true;
        clearTimeout(timer);
        resolve(false);
      }
    });

    socket.connect(port, host);
  });
}

// #758: the readiness probe used to poll a hardcoded 9877 while the bridge
// binds a per-project port and publishes it to Saved/UE_MCP_Bridge/port.json.
// On any project not on the default port this waited out the whole timeout and
// reported failure even though the editor and bridge were up - which the very
// next tool call would then prove by succeeding immediately. The lockfile does
// not exist until the bridge starts, so the port must be re-read every poll
// rather than resolved once up front.
async function waitForBridge(
  projectDir: string | undefined,
  maxWaitSeconds = 120,
  checkIntervalMs = 2000,
  projectPath?: string | null,
): Promise<{ available: boolean; state: EngineState | null }> {
  const startTime = Date.now();
  const maxWaitMs = maxWaitSeconds * 1000;
  let lastState: EngineState | null = null;

  while (Date.now() - startTime < maxWaitMs) {
    if (await isBridgeAvailable(undefined, resolveBridgePort(projectDir))) {
      return { available: true, state: lastState };
    }

    // The wait used to be blind: 120 seconds of sleeping, then a message that
    // said nothing about why the bridge never appeared. The engine publishes
    // its real state in its own log and in native windows the whole time, so
    // read it. A prompt waiting on a human, or a crash, will never resolve by
    // waiting - stop immediately and say what is on screen.
    lastState = await readEngineState(projectPath ?? null);
    if (lastState.log.phase === "crashed") {
      return { available: false, state: lastState };
    }
    if (lastState.log.blocking) {
      const withWindows = await readEngineState(projectPath ?? null, { probeWindows: true });
      return { available: false, state: withWindows };
    }
    // Nothing in the process table and nothing moving in the log means the
    // launch died rather than being slow.
    if (!lastState.running && (lastState.log.secondsSinceWrite ?? 0) > 10 && Date.now() - startTime > 15000) {
      return { available: false, state: lastState };
    }

    await new Promise((resolve) => setTimeout(resolve, checkIntervalMs));
  }

  // One last look before declaring failure: the bridge may have come up during
  // the final sleep, and reporting "not available" for a bridge that is in fact
  // serving sends the caller off debugging a problem that does not exist.
  if (await isBridgeAvailable(undefined, resolveBridgePort(projectDir))) {
    return { available: true, state: lastState };
  }
  return { available: false, state: await readEngineState(projectPath ?? null, { probeWindows: true }) };
}

export async function startEditor(
  project: ProjectContext,
  timeoutSeconds = 120,
): Promise<{ success: boolean; message: string; state?: EngineState }> {
  // Fast signal first: a bridge answering on this project's port is proof its
  // editor is up, costs a millisecond, and needs no process table at all. The
  // process probe (seconds, on Windows) only runs when that fails, which is
  // also the only case where its extra detail is worth anything.
  const projectDirForPort = project.projectPath ? path.dirname(project.projectPath) : undefined;
  if (await isBridgeAvailable(undefined, resolveBridgePort(projectDirForPort))) {
    return { success: false, message: "Editor is already running for this project (its bridge is answering)." };
  }

  const alreadyRunning = await findInteractiveEditors(project.projectPath ?? null);
  if (alreadyRunning.length > 0) {
    const state = await readEngineState(project.projectPath ?? null, { probeWindows: true });
    return {
      success: false,
      message: `Editor is already running for this project (pid ${alreadyRunning.map((p) => p.pid).join(", ")}) but its bridge is not answering yet. ${state.summary}`,
      state,
    };
  }

  const editorExe = findEditorExecutable(project);
  if (!editorExe) {
    return {
      success: false,
      message: NO_EDITOR_BINARY_MSG,
    };
  }

  if (!project.projectPath) {
    return { success: false, message: "No project loaded. Use project(action='set_project') first." };
  }

  try {
    const editorProcess = spawn(editorExe, [project.projectPath], {
      stdio: "ignore",
      detached: true,
    });

    editorProcess.unref();

    // Wait for bridge to become available (editor fully started)
    const projectDir = path.dirname(project.projectPath);
    const { available, state } = await waitForBridge(projectDir, timeoutSeconds, 2000, project.projectPath);
    if (!available) {
      const detail = state
        ? ` ${state.summary}`
        : "";
      return {
        success: false,
        message: `Editor launched but the bridge did not answer on port ${resolveBridgePort(projectDir)}.${detail}`,
        ...(state ? { state } : {}),
      };
    }

    return { success: true, message: `Editor launched and bridge available: ${editorExe}` };
  } catch (error) {
    return {
      success: false,
      message: `Failed to launch editor: ${error instanceof Error ? error.message : String(error)}`,
    };
  }
}

// Ask the editor to quit ITSELF, on the game thread, via a deferred slate tick
// so the bridge can reply before the process exits. This is a clean in-process
// exit, not an OS kill.
const EDITOR_SELF_QUIT_PY = [
  "import unreal",
  "def _ue_mcp_quit(dt):",
  "    try:",
  "        unreal.SystemLibrary.quit_editor()",
  "    except Exception as e:",
  "        unreal.log_error('ue-mcp quit_editor failed: ' + str(e))",
  "unreal.register_slate_post_tick_callback(_ue_mcp_quit)",
].join("\n");

/**
 * The .uproject inside a project directory. The stop/restart paths are handed a
 * directory, but the process probe matches editors by the project file they
 * have open, so resolve one from the other.
 */
function uprojectInDir(projectDir?: string): string | null {
  if (!projectDir) return null;
  try {
    const match = fs.readdirSync(projectDir).find((f) => f.toLowerCase().endsWith(".uproject"));
    return match ? path.join(projectDir, match) : null;
  } catch {
    return null;
  }
}

/** Read the project's live bridge port from its lockfile, else env, else 9877. */
function resolveBridgePort(projectDir?: string): number {
  if (projectDir) {
    try {
      const raw = fs.readFileSync(path.join(projectDir, "Saved", "UE_MCP_Bridge", "port.json"), "utf-8");
      const p = JSON.parse(raw) as { port?: unknown };
      if (typeof p.port === "number" && p.port > 0) return p.port;
    } catch { /* fall through to defaults */ }
  }
  const env = Number(process.env.UE_MCP_PORT);
  return Number.isFinite(env) && env > 0 ? env : 9877;
}

/**
 * Ask the editor to quit itself via the bridge (`execute_python` -> quit_editor).
 * Returns true if the request was delivered. Never touches the OS process table.
 */
function requestEditorSelfQuit(port: number): Promise<boolean> {
  return new Promise<boolean>((resolve) => {
    let settled = false;
    const ws = new WebSocket(`ws://127.0.0.1:${port}`);
    const finish = (v: boolean) => {
      if (settled) return;
      settled = true;
      try { ws.close(); } catch { /* ignore */ }
      resolve(v);
    };
    const timer = setTimeout(() => finish(false), 8000);
    ws.on("open", () => ws.send(JSON.stringify({ id: "ue-mcp-stop", method: "execute_python", params: { code: EDITOR_SELF_QUIT_PY } })));
    ws.on("message", () => { clearTimeout(timer); finish(true); });
    ws.on("error", () => { clearTimeout(timer); finish(false); });
  });
}

/**
 * Stop the editor by asking it to quit ITSELF through the bridge. ue-mcp NEVER
 * issues an OS kill: `taskkill /IM UnrealEditor.exe` matches by image name and
 * would also close the user's other editors (e.g. their real project). `force`
 * is accepted for back-compat but there is deliberately no force-kill path.
 * Success is confirmed by the project's own bridge port going quiet, so it is
 * specific to this editor even when others are open.
 */
export async function stopEditor(force = false, projectDir?: string): Promise<{ success: boolean; message: string; state?: EngineState }> {
  void force;

  const projectPath = uprojectInDir(projectDir);
  const port = resolveBridgePort(projectDir);
  const bridgeUp = await isBridgeAvailable("127.0.0.1", port);
  if (!bridgeUp && !(await isEditorRunning(projectPath))) {
    return { success: false, message: "Editor is not running" };
  }
  if (!bridgeUp) {
    // "Unreachable" is where the user is left guessing, so say what the engine
    // is actually doing: a modal dialog waiting on an answer, a slow task at
    // 60%, or a game thread that stopped ticking are all visible from outside.
    const state = await readEngineState(projectPath, { probeWindows: true });
    return {
      success: false,
      message: `Editor is running but its bridge is unreachable, so it cannot be asked to quit cleanly. ${state.summary} Close it manually - ue-mcp never force-kills processes.`,
      state,
    };
  }

  const quitSent = await requestEditorSelfQuit(port);
  if (!quitSent) {
    return {
      success: false,
      message: "Could not deliver a quit request to the editor bridge. Close the editor manually - ue-mcp never force-kills processes.",
    };
  }

  // Confirm via the project's own bridge port closing - specific to this editor.
  for (let i = 0; i < 20; i++) {
    await new Promise((resolve) => setTimeout(resolve, 1000));
    if (!(await isBridgeAvailable("127.0.0.1", port))) {
      return { success: true, message: "Editor quit itself via the bridge" };
    }
  }
  return {
    success: false,
    message: "Asked the editor to quit but its bridge is still up after 20s. Close it manually - ue-mcp never force-kills processes.",
  };
}

export async function restartEditor(project: ProjectContext, bridge?: { connect: (timeoutMs?: number) => Promise<void> }): Promise<{ success: boolean; message: string }> {
  const stopResult = await stopEditor(false, project.projectDir ?? undefined);
  if (!stopResult.success && (await isEditorRunning(project.projectPath ?? null))) {
    return { success: false, message: `Failed to stop editor: ${stopResult.message}` };
  }

  // Wait for process to fully terminate and release locks
  await new Promise((resolve) => setTimeout(resolve, 3000));

  const startResult = await startEditor(project);
  if (!startResult.success) {
    return startResult;
  }

  // Reconnect the bridge if provided
  if (bridge) {
    try {
      await bridge.connect(5000);
    } catch {
      // Bridge reconnect timer will handle it
    }
  }

  return startResult;
}

export interface BuildResult {
  success: boolean;
  message: string;
  exitCode: number | null;
}

function getPlatformString(): string {
  if (IS_WINDOWS) return "Win64";
  if (process.platform === "darwin") return "Mac";
  return "Linux";
}

export async function buildProject(
  projectPath: string,
  opts: { onOutput?: (line: string) => void } = {},
): Promise<BuildResult> {
  const resolvedPath = path.resolve(projectPath);
  const buildTool = findUEBuildTool(readEngineAssociation(resolvedPath));
  if (!buildTool) {
    return {
      success: false,
      exitCode: null,
      message:
        "Unreal Engine build tool not found. Set UE_BUILD_TOOL_PATH or install UE5.3+ to a default location.",
    };
  }

  if (!fs.existsSync(resolvedPath)) {
    return { success: false, exitCode: null, message: `Project file not found: ${resolvedPath}` };
  }

  const projectName = path.basename(resolvedPath, ".uproject");
  const target = `${projectName}Editor`;
  const platform = getPlatformString();

  // #740: the quotes around the project path are SHELL syntax, not part of the
  // value. On Windows the args are joined into a single `cmd /c` string, so
  // they are required. Off Windows the args go straight into argv with no shell
  // to strip them, so UnrealBuildTool received a path containing literal quote
  // characters and reported "Unable to find project file" for a file that was
  // plainly there - while the same command pasted into a terminal worked,
  // because the shell removed them first.
  const commonArgs = [target, platform, "Development"];
  const tailArgs = ["-WaitMutex", "-FromMsBuild"];
  const windowsArgs = [...commonArgs, `-Project="${resolvedPath}"`, ...tailArgs];
  const posixArgs = [...commonArgs, `-Project=${resolvedPath}`, ...tailArgs];

  return new Promise((resolve) => {
    let proc;
    if (IS_WINDOWS) {
      const quotedCommand = `"${buildTool}"`;
      const fullCommand = `cmd /c "${quotedCommand} ${windowsArgs.join(" ")}"`;
      proc = spawn(fullCommand, [], { shell: true, stdio: "pipe" });
    } else {
      proc = spawn(buildTool, posixArgs, { stdio: "pipe" });
    }

    const forward = (data: Buffer) => {
      const text = data.toString();
      if (opts.onOutput) opts.onOutput(text);
      else process.stdout.write(text);
    };

    if (proc.stdout) proc.stdout.on("data", forward);
    if (proc.stderr) proc.stderr.on("data", forward);

    proc.on("close", (code) => {
      // A build is the only event that can turn a "stale plugin" verdict fresh
      // ahead of the cache TTL, so drop the cached answer here rather than
      // making the next get_status report a binary that no longer exists.
      invalidatePluginFreshness();
      resolve(
        code === 0
          ? { success: true, exitCode: 0, message: "Build succeeded" }
          : { success: false, exitCode: code, message: `Build failed with exit code ${code}` },
      );
    });

    proc.on("error", (err) => {
      resolve({ success: false, exitCode: null, message: `Build error: ${err.message}` });
    });
  });
}
