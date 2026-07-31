import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { afterEach, describe, expect, it, vi } from "vitest";
import { editorTool } from "../../src/tools/editor.js";
import type { ElicitFn, ToolContext } from "../../src/types.js";

const tempDirs: string[] = [];

function makeContext(projectDir: string, elicit?: ElicitFn) {
  const call = vi.fn().mockResolvedValue({ success: true, action: "start" });
  const ctx = {
    bridge: { call },
    project: { projectDir },
    elicit,
  } as unknown as ToolContext;
  return { ctx, call };
}

async function invoke(ctx: ToolContext) {
  return editorTool.handler(ctx, {
    action: "play_in_editor_ignore_blueprint_errors",
  }) as Promise<Record<string, unknown>>;
}

function makeProjectDir(): string {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "ue-mcp-ignore-bp-errors-"));
  tempDirs.push(root);
  const projectDir = path.join(root, "Project");
  fs.mkdirSync(projectDir);
  return projectDir;
}

afterEach(() => {
  vi.restoreAllMocks();
  for (const dir of tempDirs.splice(0)) {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

describe("editor(play_in_editor_ignore_blueprint_errors)", () => {
  it("refuses when neither an instruction marker nor elicitation is available", async () => {
    const { ctx, call } = makeContext(makeProjectDir());

    const result = await invoke(ctx);

    expect(result.blocked).toBe(true);
    expect(result.code).toBe("approval_required");
    expect(call).not.toHaveBeenCalled();
  });

  it("does not start PIE when the user declines", async () => {
    const elicit = vi.fn<ElicitFn>().mockResolvedValue({ action: "decline" });
    const { ctx, call } = makeContext(makeProjectDir(), elicit);

    const result = await invoke(ctx);

    expect(result.blocked).toBe(true);
    expect(result.code).toBe("user_declined");
    expect(call).not.toHaveBeenCalled();
  });

  it("starts the native guarded path after explicit user approval", async () => {
    const elicit = vi.fn<ElicitFn>().mockResolvedValue({ action: "accept" });
    const { ctx, call } = makeContext(makeProjectDir(), elicit);

    await invoke(ctx);

    expect(call).toHaveBeenCalledWith("pie_control", expect.objectContaining({
      action: "start",
      ignoreBlueprintErrors: true,
      authorizationSource: "user_approval",
    }));
  });

  it("accepts the exact marker in an ancestor AGENTS.md without prompting", async () => {
    const projectDir = makeProjectDir();
    const root = path.dirname(projectDir);
    fs.writeFileSync(
      path.join(root, "AGENTS.md"),
      "UE_MCP_ALLOW_IGNORE_BLUEPRINT_ERRORS=true\n",
      "utf8",
    );
    const elicit = vi.fn<ElicitFn>();
    const { ctx, call } = makeContext(projectDir, elicit);

    await invoke(ctx);

    expect(elicit).not.toHaveBeenCalled();
    expect(call).toHaveBeenCalledWith("pie_control", expect.objectContaining({
      ignoreBlueprintErrors: true,
      authorizationSource: "instructions_file",
      authorizationFile: path.join(root, "AGENTS.md"),
    }));
  });
});
