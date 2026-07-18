// Regression: #724 — capture_screenshot target="pie" captured the editor
// viewport in Play-in-New-Window and never included the debug canvas. It now
// captures the actual PIE game viewport with UI + on-screen debug canvas.
import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { getBridge, disconnectBridge, callBridge } from "../setup.js";
import type { EditorBridge } from "../../src/bridge.js";

let bridge: EditorBridge;
beforeAll(async () => { bridge = await getBridge(); });
afterAll(async () => {
  await callBridge(bridge, "pie_control", { action: "stop" }).catch(() => {});
  disconnectBridge();
});

describe("editor — capture_screenshot target=pie (#724)", () => {
  it("captures the PIE game viewport with the debug canvas", async ({ skip }) => {
    const start = await callBridge(bridge, "pie_control", { action: "start" });
    // PIE can fail to start on a cold AssetRegistry / headless config - skip then.
    if (!start.ok) skip();
    await new Promise((r) => setTimeout(r, 3000));

    const status = await callBridge(bridge, "pie_control", { action: "status" });
    const running = JSON.stringify(status.result).toLowerCase().includes("running")
      || (status.result as Record<string, unknown>)?.isPlaying === true;
    if (!running) skip();

    const r = await callBridge(bridge, "capture_screenshot", { filename: "mcp_pie_724", target: "pie" });
    expect(r.ok, r.error).toBe(true);
    const result = r.result as Record<string, unknown>;
    expect(result.target).toBe("pie");
    // The game-viewport path sets includesDebugCanvas=true; the HighResShot
    // fallback (no game viewport) would be false.
    expect(result.includesDebugCanvas).toBe(true);
  });
});
