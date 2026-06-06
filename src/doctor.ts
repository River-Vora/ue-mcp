#!/usr/bin/env node
/**
 * `ue-mcp doctor` — diagnose every ue-mcp version source and flag the failure
 * mode nothing else surfaces: a project-local `node_modules/ue-mcp` that
 * silently shadows the global install (npx prefers the local copy, so updating
 * the global one changes nothing the server actually runs). (#550)
 *
 * Read-only: never mutates anything.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import { execSync } from "node:child_process";
import { createRequire } from "node:module";

const RESET = "\x1b[0m";
const BOLD = "\x1b[1m";
const GREEN = "\x1b[32m";
const RED = "\x1b[31m";
const DIM = "\x1b[2m";
const CYAN = "\x1b[36m";
const YELLOW = "\x1b[33m";

export interface DoctorReport {
  selfVersion: string;            // the ue-mcp currently executing this command
  registryLatest: string | null;
  npmGlobal: { version: string | null; dir: string | null };
  localShadow: { version: string; dir: string } | null; // nearest node_modules/ue-mcp from cwd up
  effectiveNpx: string | null;    // what `npx ue-mcp` would run: shadow ?? global
  runningServers: Array<{ pid: number; version: string | null; script: string }>;
  bridgePlugin: { version: string | null; project: string } | null;
  bareNpxConfigs: string[];       // .mcp.json paths using bare `npx ue-mcp`
}

function safeExec(cmd: string): string | null {
  try {
    return execSync(cmd, { encoding: "utf-8", stdio: ["pipe", "pipe", "pipe"] }).trim();
  } catch {
    return null;
  }
}

function readJsonVersion(pkgJsonPath: string): string | null {
  try {
    const raw = fs.readFileSync(pkgJsonPath, "utf-8");
    const v = JSON.parse(raw).version;
    return typeof v === "string" ? v : null;
  } catch {
    return null;
  }
}

function selfVersion(): string {
  try {
    const require = createRequire(import.meta.url);
    return require("../package.json").version;
  } catch {
    return "unknown";
  }
}

function registryLatest(): string | null {
  return safeExec("npm view ue-mcp version");
}

function npmGlobal(): { version: string | null; dir: string | null } {
  const root = safeExec("npm root -g");
  if (!root) return { version: null, dir: null };
  const dir = path.join(root, "ue-mcp");
  return { version: readJsonVersion(path.join(dir, "package.json")), dir };
}

/** Walk up from cwd; the first node_modules/ue-mcp is what npx resolves locally. */
export function findLocalShadow(startDir: string): { version: string; dir: string } | null {
  let dir = path.resolve(startDir);
  for (;;) {
    const candidate = path.join(dir, "node_modules", "ue-mcp");
    const pkg = path.join(candidate, "package.json");
    if (fs.existsSync(pkg)) {
      const version = readJsonVersion(pkg);
      if (version) return { version, dir: candidate };
    }
    const parent = path.dirname(dir);
    if (parent === dir) return null;
    dir = parent;
  }
}

/** From a running script path, walk up to its package.json and read the version. */
function versionForScript(scriptPath: string): string | null {
  let dir = path.dirname(scriptPath);
  for (let i = 0; i < 6; i++) {
    const pkg = path.join(dir, "package.json");
    if (fs.existsSync(pkg)) return readJsonVersion(pkg);
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return null;
}

/** Best-effort scan of running node processes for a ue-mcp server. */
function findRunningServers(): Array<{ pid: number; version: string | null; script: string }> {
  const out: Array<{ pid: number; version: string | null; script: string }> = [];
  const selfPid = process.pid;
  let lines: string[] = [];

  if (process.platform === "win32") {
    // PowerShell CIM gives the full command line, which tasklist does not.
    const raw = safeExec(
      'powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \\"Name=\'node.exe\'\\" | ForEach-Object { $_.ProcessId.ToString() + \'|\' + $_.CommandLine }"',
    );
    if (raw) lines = raw.split(/\r?\n/);
  } else {
    const raw = safeExec("ps -eo pid=,args=");
    if (raw) lines = raw.split(/\n/).map((l) => l.trim().replace(/^(\d+)\s+/, "$1|"));
  }

  for (const line of lines) {
    if (!line) continue;
    const sep = line.indexOf("|");
    if (sep < 0) continue;
    const pid = parseInt(line.slice(0, sep), 10);
    const cmd = line.slice(sep + 1);
    if (!Number.isFinite(pid) || pid === selfPid) continue;
    // The server entrypoint is ue-mcp/dist/index.js. Match it, skip doctor/update
    // CLIs (which run other dist/*.js). Reconstruct the path and require it to
    // exist on disk, so a loose regex match against an unrelated node process
    // can't produce a phantom "unknown" entry.
    const m = cmd.match(/([A-Za-z]:[\\/].*?|\/.*?)ue-mcp[\\/]dist[\\/]index\.js/i);
    if (!m) continue;
    const script = path.normalize((m[1] ?? "") + path.join("ue-mcp", "dist", "index.js"));
    if (!fs.existsSync(script)) continue;
    if (out.some((s) => s.pid === pid)) continue;
    out.push({ pid, version: versionForScript(script), script: script.replace(/\\/g, "/") });
  }
  return out;
}

function findUproject(projectArg: string | undefined, cwd: string): string | null {
  if (projectArg && projectArg.endsWith(".uproject") && fs.existsSync(projectArg)) {
    return path.resolve(projectArg);
  }
  let dir = path.resolve(cwd);
  for (let i = 0; i < 4; i++) {
    try {
      const found = fs.readdirSync(dir).find((f) => f.endsWith(".uproject"));
      if (found) return path.join(dir, found);
    } catch { /* ignore */ }
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return null;
}

function bridgePluginVersion(projectArg: string | undefined, cwd: string): { version: string | null; project: string } | null {
  const uproject = findUproject(projectArg, cwd);
  if (!uproject) return null;
  const upluginPath = path.join(path.dirname(uproject), "Plugins", "UE_MCP_Bridge", "UE_MCP_Bridge.uplugin");
  if (!fs.existsSync(upluginPath)) return { version: null, project: uproject };
  try {
    const parsed = JSON.parse(fs.readFileSync(upluginPath, "utf-8"));
    return { version: typeof parsed.VersionName === "string" ? parsed.VersionName : null, project: uproject };
  } catch {
    return { version: null, project: uproject };
  }
}

/**
 * Scan cwd + parents for a .mcp.json that launches the server via bare
 * `npx ue-mcp` (an npx server whose args include "ue-mcp" with no @version pin
 * and no -y) — the configuration that lets a local copy shadow the global one.
 */
export function findBareNpxConfigs(cwd: string): string[] {
  const hits: string[] = [];
  let dir = path.resolve(cwd);
  for (let i = 0; i < 4; i++) {
    const cfg = path.join(dir, ".mcp.json");
    if (fs.existsSync(cfg)) {
      try {
        const parsed = JSON.parse(fs.readFileSync(cfg, "utf-8"));
        const servers = (parsed && typeof parsed === "object" && parsed.mcpServers) || {};
        for (const key of Object.keys(servers)) {
          const s = servers[key] || {};
          const command = String(s.command ?? "");
          const args: string[] = Array.isArray(s.args) ? s.args.map((a: unknown) => String(a)) : [];
          const isNpx = /(^|[\\/])npx(\.cmd)?$/i.test(command) || command.toLowerCase() === "npx";
          const targetsUeMcp = args.some((a) => a === "ue-mcp");
          const pinned = args.some((a) => a.startsWith("ue-mcp@"));
          const selfHealing = args.includes("-y") || args.includes("--yes");
          if (isNpx && targetsUeMcp && !pinned && !selfHealing) { hits.push(cfg); break; }
        }
      } catch { /* ignore malformed config */ }
    }
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return hits;
}

export function collectDoctor(projectArg?: string, cwd: string = process.cwd()): DoctorReport {
  const global = npmGlobal();
  const shadow = findLocalShadow(cwd);
  return {
    selfVersion: selfVersion(),
    registryLatest: registryLatest(),
    npmGlobal: global,
    localShadow: shadow,
    effectiveNpx: shadow ? shadow.version : global.version,
    runningServers: findRunningServers(),
    bridgePlugin: bridgePluginVersion(projectArg, cwd),
    bareNpxConfigs: findBareNpxConfigs(cwd),
  };
}

function row(label: string, value: string): string {
  return `  ${label.padEnd(16)}${value}`;
}

export function formatDoctor(d: DoctorReport): string {
  const latest = d.registryLatest;
  const lines: string[] = [];
  lines.push("");
  lines.push(`  ${BOLD}${CYAN}ue-mcp doctor${RESET}`);
  lines.push("");

  lines.push(row("registry latest:", latest ? `${BOLD}${latest}${RESET}` : `${DIM}(offline)${RESET}`));
  lines.push(row("npm global:", d.npmGlobal.version ?? `${DIM}(not installed)${RESET}`));

  if (d.localShadow) {
    const rel = path.relative(process.cwd(), d.localShadow.dir).replace(/\\/g, "/") || d.localShadow.dir;
    const warn = `${RED}<-- WARN npx runs THIS, not global${RESET}`;
    lines.push(row("local shadow:", `${YELLOW}./${rel} @ ${d.localShadow.version}${RESET}  ${warn}`));
  } else {
    lines.push(row("local shadow:", `${GREEN}none${RESET}`));
  }

  const effLabel = d.effectiveNpx ?? "unknown";
  const effMismatch = latest && d.effectiveNpx && d.effectiveNpx !== latest;
  lines.push(row("effective (npx):", effMismatch ? `${RED}${effLabel}  (behind latest ${latest})${RESET}` : `${GREEN}${effLabel}${RESET}`));

  if (d.runningServers.length === 0) {
    lines.push(row("running server:", `${DIM}none detected${RESET}`));
  } else {
    for (const s of d.runningServers) {
      const v = s.version ?? "unknown";
      const mismatch = latest && s.version && s.version !== latest;
      const tag = mismatch ? `${RED}(MISMATCH with latest ${latest})${RESET}` : `${GREEN}ok${RESET}`;
      lines.push(row("running server:", `${v}  ${DIM}pid ${s.pid}${RESET}  ${tag}`));
    }
  }

  if (d.bridgePlugin) {
    const v = d.bridgePlugin.version ?? `${DIM}(not deployed)${RESET}`;
    lines.push(row("bridge plugin:", `${v}  ${DIM}${path.basename(d.bridgePlugin.project)}${RESET}`));
  }

  lines.push("");

  const problems: string[] = [];
  if (d.localShadow && latest && d.localShadow.version !== latest) {
    problems.push(
      `A project-local node_modules/ue-mcp@${d.localShadow.version} shadows the global install. ` +
      `npx runs it, so global updates do nothing. Fix: remove the dependency from package.json and delete node_modules/ue-mcp, ` +
      `or pin .mcp.json to \`npx -y ue-mcp@latest\`. Then run \`ue-mcp update --build\`.`,
    );
  }
  for (const cfg of d.bareNpxConfigs) {
    const rel = path.relative(process.cwd(), cfg).replace(/\\/g, "/") || cfg;
    problems.push(`${rel} launches with bare \`npx ue-mcp\`. Use \`npx -y ue-mcp@latest\` so the server self-heals to latest on each launch.`);
  }
  for (const s of d.runningServers) {
    if (latest && s.version && s.version !== latest) {
      problems.push(`Running server (pid ${s.pid}) is ${s.version}, not ${latest}. Quit your MCP client and relaunch to swap it.`);
    }
  }

  if (problems.length === 0) {
    lines.push(`  ${GREEN}✓ Everything aligned.${RESET}`);
  } else {
    lines.push(`  ${BOLD}${YELLOW}Findings${RESET}`);
    for (const p of problems) lines.push(`  ${RED}!${RESET} ${p}`);
  }
  lines.push("");
  return lines.join("\n");
}

/** Entry point for `ue-mcp doctor [project.uproject]`. */
export function runDoctorCli(): void {
  const projectArg = process.argv.slice(2).find((a) => !a.startsWith("-"));
  console.log(formatDoctor(collectDoctor(projectArg)));
}
