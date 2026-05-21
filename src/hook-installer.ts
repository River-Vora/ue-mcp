import * as fs from "node:fs";
import * as path from "node:path";
import yaml from "js-yaml";
import { warn as logWarn } from "./log.js";

/**
 * Symmetric install/uninstall for the Claude Code PostToolUse hook that
 * prompts the agent to file feedback when execute_python was used as a
 * workaround.
 *
 * Two invariants:
 *   1. Every install records the settings path in ue-mcp.local.yml's
 *      `ue-mcp.installedHooks[]`, so uninstall can reach every site even
 *      after the user moves their MCP client config. The local file is
 *      gitignored — these paths are user-machine-specific.
 *   2. Uninstall is idempotent — calling it against a settings file that
 *      doesn't have our matcher is a no-op, not an error.
 */

const MATCHER = "mcp__ue-mcp__editor";
const COMMAND = "npx ue-mcp hook post-tool-use";

interface ClaudeHookEntry {
  type: string;
  command: string;
}

interface ClaudeHookMatcher {
  matcher: string;
  hooks: ClaudeHookEntry[];
}

interface ClaudeSettings {
  hooks?: Record<string, ClaudeHookMatcher[]>;
  [key: string]: unknown;
}

function readSettings(settingsPath: string): ClaudeSettings {
  if (!fs.existsSync(settingsPath)) return {};
  try {
    return JSON.parse(fs.readFileSync(settingsPath, "utf-8"));
  } catch (e) {
    logWarn(
      "hook-installer",
      `Claude settings at ${settingsPath} was not valid JSON — treating as empty`,
      e,
    );
    return {};
  }
}

function writeSettings(settingsPath: string, settings: ClaudeSettings): void {
  const dir = path.dirname(settingsPath);
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(settingsPath, JSON.stringify(settings, null, 2));
}

/**
 * Read/write the `ue-mcp:` block of ue-mcp.local.yml. The local file is
 * gitignored and holds user-machine-specific state — installed hook paths
 * are absolute paths in the user's filesystem and have no business in
 * tracked config.
 */

interface LocalYaml {
  "ue-mcp"?: {
    installedHooks?: string[];
    [key: string]: unknown;
  };
  [key: string]: unknown;
}

function localPath(projectDir: string): string {
  return path.join(projectDir, "ue-mcp.local.yml");
}

function readLocal(projectDir: string): LocalYaml {
  const file = localPath(projectDir);
  if (!fs.existsSync(file)) return {};
  try {
    return (yaml.load(fs.readFileSync(file, "utf-8")) as LocalYaml | null) ?? {};
  } catch (e) {
    logWarn(
      "hook-installer",
      `ue-mcp.local.yml at ${file} did not parse — installedHooks registry will be rewritten`,
      e,
    );
    return {};
  }
}

function writeLocal(projectDir: string, doc: LocalYaml): void {
  const file = localPath(projectDir);
  // Empty doc → remove the file rather than leaving a stub behind.
  const block = doc["ue-mcp"];
  if (
    !block ||
    Object.keys(block).filter((k) => block[k] !== undefined).length === 0
  ) {
    if (fs.existsSync(file)) fs.unlinkSync(file);
    return;
  }
  fs.writeFileSync(file, yaml.dump(doc, { indent: 2 }), "utf-8");
}

function getInstalledHooks(projectDir: string): string[] {
  const local = readLocal(projectDir);
  const hooks = local["ue-mcp"]?.installedHooks;
  return Array.isArray(hooks) ? hooks : [];
}

function setInstalledHooks(projectDir: string, hooks: string[]): void {
  const local = readLocal(projectDir);
  const block = (local["ue-mcp"] ??= {});
  if (hooks.length > 0) {
    block.installedHooks = hooks;
  } else {
    delete block.installedHooks;
  }
  writeLocal(projectDir, local);
}

function registerInstalledHook(projectDir: string, settingsPath: string): void {
  const abs = path.resolve(settingsPath);
  const installed = new Set(getInstalledHooks(projectDir));
  installed.add(abs);
  setInstalledHooks(projectDir, [...installed].sort());
}

function unregisterInstalledHook(
  projectDir: string,
  settingsPath: string,
): void {
  const abs = path.resolve(settingsPath);
  const existing = getInstalledHooks(projectDir);
  if (existing.length === 0) return;
  setInstalledHooks(
    projectDir,
    existing.filter((p) => path.resolve(p) !== abs),
  );
}

/**
 * Install the ue-mcp PostToolUse hook into a Claude Code settings.json. If
 * `projectDir` is supplied, the settings path is also recorded in the
 * project's ue-mcp.local.yml installedHooks registry.
 */
export function installClaudeHooks(
  settingsPath: string,
  projectDir?: string,
): void {
  const settings = readSettings(settingsPath);
  if (!settings.hooks) settings.hooks = {};
  if (!settings.hooks.PostToolUse) settings.hooks.PostToolUse = [];

  const already = settings.hooks.PostToolUse.some(
    (h) => h.matcher === MATCHER,
  );
  if (!already) {
    settings.hooks.PostToolUse.push({
      matcher: MATCHER,
      hooks: [{ type: "command", command: COMMAND }],
    });
  }

  writeSettings(settingsPath, settings);
  if (projectDir) registerInstalledHook(projectDir, settingsPath);
}

/**
 * Remove the ue-mcp PostToolUse matcher from a Claude Code settings.json.
 * Idempotent: a missing file, missing hooks block, or missing matcher is
 * treated as already-uninstalled. Returns true if a matcher was actually
 * removed, false otherwise.
 */
export function uninstallClaudeHooks(
  settingsPath: string,
  projectDir?: string,
): boolean {
  let removed = false;
  if (fs.existsSync(settingsPath)) {
    const settings = readSettings(settingsPath);
    if (settings.hooks?.PostToolUse) {
      const before = settings.hooks.PostToolUse.length;
      settings.hooks.PostToolUse = settings.hooks.PostToolUse.filter(
        (h) => h.matcher !== MATCHER,
      );
      if (settings.hooks.PostToolUse.length !== before) {
        removed = true;
        if (settings.hooks.PostToolUse.length === 0) {
          delete settings.hooks.PostToolUse;
        }
        if (Object.keys(settings.hooks).length === 0) {
          delete settings.hooks;
        }
        writeSettings(settingsPath, settings);
      }
    }
  }
  if (projectDir) unregisterInstalledHook(projectDir, settingsPath);
  return removed;
}

/**
 * Uninstall the hook from every path recorded in ue-mcp.local.yml's
 * `ue-mcp.installedHooks[]`. Used by `npx ue-mcp uninstall-hooks` and by
 * init when the user disables feedback or opts out of the prompt checkbox.
 */
export function uninstallAllRegisteredHooks(projectDir: string): {
  removed: string[];
  skipped: string[];
} {
  const paths = getInstalledHooks(projectDir);
  const removed: string[] = [];
  const skipped: string[] = [];
  for (const p of paths) {
    const didRemove = uninstallClaudeHooks(p);
    if (didRemove) removed.push(p);
    else skipped.push(p);
  }
  setInstalledHooks(projectDir, []);
  return { removed, skipped };
}
