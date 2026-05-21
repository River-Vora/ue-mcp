# Configuration

## MCP Client Configuration

The easiest way to configure UE-MCP is to run `npx ue-mcp init` — it detects your MCP clients and writes the config automatically.

### Manual Configuration

```json
{
  "mcpServers": {
    "ue-mcp": {
      "command": "npx",
      "args": ["ue-mcp", "C:/path/to/MyGame.uproject"]
    }
  }
}
```

### Where to Put This

| Client | Config File |
|--------|-------------|
| Claude Code | `.mcp.json` in project root, or `~/.claude/` global config |
| Claude Desktop | `claude_desktop_config.json` |
| Cursor | `mcp.json` in `.cursor/` or project root |

### Without a Project Path

You can start the server without a `.uproject` argument. It will run in a limited mode — you can then use `project(action="set_project", projectPath="...")` at runtime to attach to a project.

## Project Configuration (`ue-mcp.yml`)

Project-level config lives under the `ue-mcp:` block at the top of `ue-mcp.yml` (next to your `.uproject`). User-machine-specific state lives in `ue-mcp.local.yml`. Both files share the same schema; the loader deep-merges local on top of the tracked one. `npx ue-mcp init` creates and maintains them for you.

```yaml
ue-mcp:
  version: 1
  contentRoots:
    - /Game/
    - /MyPlugin/
  disable:
    - gas
    - networking
  http:
    enabled: false
  feedback:
    mode: interactive

tasks: {}
flows: {}
plugins: []
```

!!! info "Tracked vs untracked"
    `ue-mcp.yml` is meant to live in git so every collaborator sees the same project surface. `ue-mcp.local.yml` is gitignored by `npx ue-mcp init` (when run inside a git repo) and holds user-machine-specific state — currently just `installedHooks`, which lists absolute paths to your Claude Code settings files. Don't put project policy in the local file or shared things will silently drop on other machines.

!!! tip "Migrating from `.ue-mcp.json`"
    Earlier versions used `.ue-mcp.json` for the project config. On first run after upgrade, the server detects that file and migrates its contents (tracked fields → `ue-mcp.yml`, `installedHooks` → `ue-mcp.local.yml`) automatically, then deletes the JSON. You don't need to do anything.

### `ue-mcp:` block options

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `version` | `1` | `1` | Schema version; required. Set automatically by init. |
| `contentRoots` | `string[]` | `["/Game/"]` | Content paths to search when using `asset(action="search")`. Add plugin content roots here if your project uses plugins with their own assets. |
| `disable` | `string[]` | `[]` | Tool categories to disable. Disabled categories are not registered with the MCP server, reducing context noise for the AI. Use `"feedback"` here to opt out of the feedback tool entirely. |
| `http` | `object` | `undefined` (HTTP server off) | Optional REST surface for the flow engine. Object with `enabled` (bool), `port` (default `7723`), `host` (default `127.0.0.1`). When `enabled: true`, the MCP server also serves `GET /flows`, `GET /flows/<name>/plan`, `POST /flows/<name>/run` over HTTP so external tools can drive flows without an MCP client. |
| `feedback.mode` | `"interactive" \| "auto-approve" \| "defer"` | `"interactive"` | How `feedback(submit)` handles the consent gate. `interactive` blocks on the MCP elicitation prompt. `auto-approve` posts to GitHub immediately without prompting (scrubs still apply). `defer` writes the scrubbed payload to `~/.ue-mcp/pending-feedback/` for later review via `npx ue-mcp feedback list/approve/discard`. Override at runtime with `UE_MCP_FEEDBACK_MODE`. See [Feedback → modes](feedback.md#feedback-modes). |
| `installedHooks` | `string[]` | `[]` | **Lives in `ue-mcp.local.yml`, not `ue-mcp.yml`.** Maintained by `npx ue-mcp init` / `npx ue-mcp uninstall-hooks`. Lists the absolute paths of every Claude Code `settings.json` where ue-mcp installed its feedback PostToolUse hook, so uninstall can find and remove them cleanly. Don't hand-edit. |

## Plugins

The `plugins:` array in **`ue-mcp.yml`** declares npm packages that inject new actions into existing built-in categories. The full author contract lives in [Plugins](plugins.md); this is the consumer view.

```yaml
plugins:
  - name: ue-mcp-plugin-voxel-plugin
  - name: ue-mcp-plugin-my-other-thing
    version: "0.2.x"        # optional - npm semver range
```

At server start, ue-mcp resolves each entry against the project's `node_modules/`, validates the plugin manifest, and merges its injected actions into the host category tools. Stay-on-disk facts:

- The package must already be installed under `<project>/node_modules/`. Use `ue-mcp plugin install <name>` to add an entry **and** run `npm install --save` in one step.
- Plugins are loaded only when the server boots — edit the array and restart your MCP client (`/mcp` in Claude Code).
- A plugin that fails validation is skipped with a loud warning. Other plugins keep loading; the host tools are never partially mutated.
- Use the `plugins` tool to introspect the loaded set:
  - `plugins(action="list")` — name, version, prefix, status, injected count, host UE plugin presence.
  - `plugins(action="describe", name="<package>")` — full detail including injected actions, knowledge files, and flows.

Order matters: earlier entries win on inter-plugin action-name collisions. A plugin action can never overwrite a built-in.

### Host UE plugin dependencies

A plugin can declare `uePluginDependency: <PluginName>` in its `ue-mcp.plugin.yml`. The MCP server checks the project's `.uproject` for `Plugins[].Name == "<PluginName>"` and exposes the result as `uePluginPresent` in `plugins(action="list")`. The npm side loads regardless — the flag is a signal that the host UE plugin needs to be enabled before the injected actions will actually run.

For example, `ue-mcp-plugin-voxel-plugin` declares `uePluginDependency: Voxel`. Until `Voxel` is added to `<Project>.uproject`'s `Plugins` array (and the C++ modules are built), `voxel_build_scatter_graph` is loaded but will fail at execute time.

## Bridge Connection

The C++ plugin listens on **`ws://localhost:9877`** (currently hardcoded). The MCP server auto-connects on startup and reconnects every 15 seconds if the connection drops.

### Connection States

| State | Meaning |
|-------|---------|
| **Connected** | Bridge is active, all tools available |
| **Disconnected** | Editor not running or plugin not loaded. Filesystem tools still work (INI parsing, C++ headers, asset listing) |
| **Reconnecting** | Connection lost, auto-retry in progress |

Check the current state with `project(action="get_status")`.

## Plugin Deployment

On first run with a project path, the server automatically:

1. Copies `plugin/ue_mcp_bridge/` → `<Project>/Plugins/UE_MCP_Bridge/`
2. Adds `UE_MCP_Bridge` to the `.uproject` plugins list
3. Enables `PythonScriptPlugin` if not already enabled (needed for `execute_python` escape hatch)

The plugin is editor-only and has no runtime footprint.

### Plugin Dependencies

The C++ bridge plugin enables these UE plugins (adding them to `.uproject` if missing):

- `PythonScriptPlugin` — for `editor(action="execute_python")`
- `EnhancedInput` — for input action/mapping creation
- `GameplayAbilities` — for GAS tools
- `Niagara` — for VFX tools
- `PCG` — for procedural generation tools

## CLI Subcommands

`npx ue-mcp` exposes a few utility subcommands beyond the default MCP server entry:

| Command | Description |
|---------|-------------|
| `npx ue-mcp init` | Interactive setup wizard. Deploys the C++ bridge plugin, writes MCP client configs, scaffolds `ue-mcp.yml` (and adds `ue-mcp.local.yml` to `.gitignore` if the project's in git), optionally installs Claude Code skills + feedback prompt hook, optionally runs the GitHub OAuth device flow. Migrates any legacy `.ue-mcp.json` it finds. |
| `npx ue-mcp update` | Re-deploy the C++ bridge plugin to the project. Use after a ue-mcp version bump. |
| `npx ue-mcp auth` | Run the GitHub device flow standalone so `feedback(submit)` can author issues as your real GitHub user. Same step that lives inside `init`; use this if you skipped it at init time. |
| `npx ue-mcp uninstall-hooks` | Remove the feedback PostToolUse hook from every Claude Code settings file recorded in `ue-mcp.local.yml`'s `ue-mcp.installedHooks[]`. |
| `npx ue-mcp feedback list \| show \| approve \| discard` | Manage submissions queued by `feedback.mode = "defer"`. See [Feedback → Reviewing deferred submissions](feedback.md#reviewing-deferred-submissions). |
| `npx ue-mcp resolve <issue>` | Fetch a feedback issue, branch, hand it to Claude Code to implement, open a PR. See [Feedback](feedback.md#resolving-feedback-issues). |
| `npx ue-mcp plugin install <name>` | Install a ue-mcp plugin from npm and register it in `ue-mcp.yml`. See [Configuration → Plugins](#plugins). |
| `npx ue-mcp plugin uninstall <name>` | Inverse of install. |
| `npx ue-mcp plugin create <name>` | Scaffold a new plugin package. See [Plugins](plugins.md). |

## Editor Lifecycle

The server can manage the editor process:

| Command | Description |
|---------|-------------|
| `editor(action="start_editor")` | Launch UE with the current project |
| `editor(action="stop_editor")` | Gracefully stop the editor |
| `editor(action="restart_editor")` | Stop and relaunch |
