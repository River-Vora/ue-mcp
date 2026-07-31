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
  it("sets and rolls back mobility without overwriting SkyLight color", async () => {
    const originalColor = { r: 31, g: 95, b: 173 };
    const spawned = await callBridge(bridge, "spawn_light", {
      lightType: "sky",
      label,
      color: originalColor,
    });
    expect(spawned.ok, spawned.error).toBe(true);
    expect((spawned.result as Record<string, unknown>).success).not.toBe(false);

    const beforeTree = await callBridge(bridge, "get_component_tree", { actorLabel: label });
    expect(beforeTree.ok, beforeTree.error).toBe(true);
    const beforeComponents = ((beforeTree.result as Record<string, unknown>).components ?? []) as Array<Record<string, unknown>>;
    const beforeSky = beforeComponents.find((component) =>
      String(component.class).includes("SkyLightComponent"),
    );
    expect(beforeSky?.mobility).toBeTruthy();
    const previousMobility = String(beforeSky!.mobility);
    const requestedMobility = previousMobility === "Movable" ? "static" : "movable";

    const updated = await callBridge(bridge, "set_light_properties", {
      actorLabel: label,
      mobility: requestedMobility,
    });
    expect(updated.ok, updated.error).toBe(true);
    const updatedResult = updated.result as Record<string, unknown>;
    expect(updatedResult.success).not.toBe(false);
    expect(updatedResult.isSkyLight).toBe(true);
    expect(updatedResult.mobility).toBe(requestedMobility);

    const updatedColor = updatedResult.color as Record<string, number>;
    expect(Math.abs(updatedColor.r - originalColor.r)).toBeLessThan(2);
    expect(Math.abs(updatedColor.g - originalColor.g)).toBeLessThan(2);
    expect(Math.abs(updatedColor.b - originalColor.b)).toBeLessThan(2);

    const rollback = updatedResult.rollback as {
      method: string;
      payload: Record<string, unknown>;
    };
    expect(rollback.method).toBe("set_light_properties");
    expect(rollback.payload.actorLabel).toBe(label);
    expect(rollback.payload.mobility).toBe(previousMobility.toLowerCase());
    expect(rollback.payload).not.toHaveProperty("color");
    expect(rollback.payload).not.toHaveProperty("intensity");
    expect(Object.keys(rollback.payload).sort()).toEqual(["actorLabel", "mobility"]);

    const tree = await callBridge(bridge, "get_component_tree", { actorLabel: label });
    expect(tree.ok, tree.error).toBe(true);
    const components = ((tree.result as Record<string, unknown>).components ?? []) as Array<Record<string, unknown>>;
    const skyComponent = components.find((component) =>
      String(component.class).includes("SkyLightComponent"),
    );
    expect(skyComponent?.mobility).toBe(requestedMobility === "movable" ? "Movable" : "Static");

    const rolledBack = await callBridge(bridge, rollback.method, rollback.payload);
    expect(rolledBack.ok, rolledBack.error).toBe(true);
    const rolledBackResult = rolledBack.result as Record<string, unknown>;
    expect(rolledBackResult.success).not.toBe(false);
    const rolledBackColor = rolledBackResult.color as Record<string, number>;
    expect(Math.abs(rolledBackColor.r - originalColor.r)).toBeLessThan(2);
    expect(Math.abs(rolledBackColor.g - originalColor.g)).toBeLessThan(2);
    expect(Math.abs(rolledBackColor.b - originalColor.b)).toBeLessThan(2);

    const restoredTree = await callBridge(bridge, "get_component_tree", { actorLabel: label });
    expect(restoredTree.ok, restoredTree.error).toBe(true);
    const restoredComponents = ((restoredTree.result as Record<string, unknown>).components ?? []) as Array<Record<string, unknown>>;
    const restoredSky = restoredComponents.find((component) =>
      String(component.class).includes("SkyLightComponent"),
    );
    expect(restoredSky?.mobility).toBe(previousMobility);
  });
});
