import { describe, expect, it, vi } from "vitest";
import { reflectionTool } from "../../src/tools/reflection.js";
import type { ToolContext } from "../../src/types.js";

describe("reflection.inspect_save_game", () => {
  it("maps the validated slot selector to the native handler", async () => {
    const call = vi.fn().mockResolvedValue({ success: true });
    const ctx = { bridge: { call } } as unknown as ToolContext;

    await reflectionTool.handler(ctx, {
      action: "inspect_save_game",
      slotName: "UserSettings",
      userIndex: 2,
      className: "ignored",
    });

    expect(call).toHaveBeenCalledWith(
      "inspect_save_game",
      { slotName: "UserSettings", userIndex: 2 },
      undefined,
    );
  });

  it("rejects negative and fractional user indexes in the public schema", () => {
    expect(reflectionTool.schema.userIndex.safeParse(-1).success).toBe(false);
    expect(reflectionTool.schema.userIndex.safeParse(0.5).success).toBe(false);
    expect(reflectionTool.schema.userIndex.safeParse(0).success).toBe(true);
  });
});
