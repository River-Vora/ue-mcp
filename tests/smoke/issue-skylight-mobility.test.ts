import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { callBridge, disconnectBridge, getBridge } from "../setup.js";
import type { EditorBridge } from "../../src/bridge.js";

let bridge: EditorBridge;
const label = `MCPTest_SkyLightMobility_${Date.now()}`;

beforeAll(async () => { bridge = await getBridge(); });
afterAll(async () => {
  await callBridge(bridge, "delete_actor", { actorLabel: label });
  disconnectBridge();
});

describe("SkyLight mobility regression", () => {
  it("sets mobility through the SkyLight SceneComponent", async () => {
    const spawned = await callBridge(bridge, "spawn_light", {
      lightType: "sky",
      label,
    });
    expect(spawned.ok, spawned.error).toBe(true);
    expect((spawned.result as Record<string, unknown>).success).not.toBe(false);

    const updated = await callBridge(bridge, "set_light_properties", {
      actorLabel: label,
      mobility: "movable",
    });
    expect(updated.ok, updated.error).toBe(true);
    const updatedResult = updated.result as Record<string, unknown>;
    expect(updatedResult.success).not.toBe(false);
    expect(updatedResult.isSkyLight).toBe(true);

    const tree = await callBridge(bridge, "get_component_tree", { actorLabel: label });
    expect(tree.ok, tree.error).toBe(true);
    const components = ((tree.result as Record<string, unknown>).components ?? []) as Array<Record<string, unknown>>;
    const skyComponent = components.find((component) =>
      String(component.class).includes("SkyLightComponent"),
    );
    expect(skyComponent?.mobility).toBe("Movable");
  });
});
