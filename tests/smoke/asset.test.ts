import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { getBridge, disconnectBridge, callBridge, resultArray, TEST_PREFIX } from "../setup.js";
import type { EditorBridge } from "../../src/bridge.js";

let bridge: EditorBridge;

beforeAll(async () => { bridge = await getBridge(); });
afterAll(() => disconnectBridge());

describe("asset — read", () => {
  it("search_assets (wildcard)", async () => {
    const r = await callBridge(bridge, "search_assets", { query: "*", maxResults: 10 });
    expect(r.ok, r.error).toBe(true);
  });

  it("search_assets (typed)", async () => {
    const r = await callBridge(bridge, "search_assets", { query: "StaticMesh", maxResults: 5 });
    expect(r.ok, r.error).toBe(true);
  });

  it("list_textures", async () => {
    const r = await callBridge(bridge, "list_textures", { recursive: true });
    expect(r.ok, r.error).toBe(true);
  });
});

describe("asset — read specific (dynamic)", () => {
  let assetPath: string | undefined;

  beforeAll(async () => {
    const r = await callBridge(bridge, "search_assets", { query: "*", maxResults: 1 });
    if (r.ok) {
      const assets = resultArray(r.result, "assets");
      if (assets && assets.length > 0) {
        const first = assets[0] as Record<string, unknown>;
        assetPath = (first.path ?? first.asset_path ?? first.objectPath) as string | undefined;
      }
    }
  });

  it("read_asset", async ({ skip }) => {
    if (!assetPath) skip();
    const r = await callBridge(bridge, "read_asset", { path: assetPath });
    expect(r.ok, r.error).toBe(true);
  });

  it("read_asset_properties", async ({ skip }) => {
    if (!assetPath) skip();
    const r = await callBridge(bridge, "read_asset_properties", { assetPath });
    expect(r.ok, r.error).toBe(true);
  });
});

describe("asset — write (with cleanup)", () => {
  const created: string[] = [];

  afterAll(async () => {
    for (const p of created) {
      await callBridge(bridge, "delete_asset", { assetPath: p });
    }
  });

  it("duplicate_asset", async ({ skip }) => {
    const search = await callBridge(bridge, "search_assets", { query: "*", maxResults: 1 });
    const assets = resultArray(search.result, "assets");
    if (!search.ok || !assets || assets.length === 0) skip();
    const first = assets[0] as Record<string, unknown>;
    const src = (first.path ?? first.asset_path ?? first.objectPath) as string;
    const dest = `${TEST_PREFIX}/DuplicateTest`;
    const r = await callBridge(bridge, "duplicate_asset", { sourcePath: src, destinationPath: dest });
    expect(r.ok, r.error).toBe(true);
    created.push(dest);
  });

  it("save_asset (all dirty)", async () => {
    const r = await callBridge(bridge, "save_asset", { assetPath: "" });
    // May fail if no dirty assets; we're testing the method exists
    expect(r.method).toBe("save_asset");
  });

  it("create_folder + delete_folder round-trip", async () => {
    const folder = `${TEST_PREFIX}/FolderRoundTrip_${Date.now()}`;
    const created = await callBridge(bridge, "create_folder", { path: folder });
    expect(created.ok, created.error).toBe(true);
    const deleted = await callBridge(bridge, "delete_folder", { path: folder });
    expect(deleted.ok, deleted.error).toBe(true);
    const entries = (deleted.result as { entries?: Array<{ status?: string }> })?.entries ?? [];
    expect(entries[0]?.status).toBe("deleted");
  });

  it("delete_folder refuses non-empty without force", async () => {
    const folder = `${TEST_PREFIX}/FolderNonEmpty_${Date.now()}`;
    await callBridge(bridge, "create_folder", { path: folder });
    // Drop one asset inside so the folder is non-empty.
    const search = await callBridge(bridge, "search_assets", { query: "*", maxResults: 1 });
    const assets = resultArray(search.result, "assets");
    if (!search.ok || !assets || assets.length === 0) {
      await callBridge(bridge, "delete_folder", { path: folder });
      return;
    }
    const src = ((assets[0] as Record<string, unknown>).path ?? (assets[0] as Record<string, unknown>).objectPath) as string;
    const dup = `${folder}/RefuseProbe`;
    await callBridge(bridge, "duplicate_asset", { sourcePath: src, destinationPath: dup });

    const refused = await callBridge(bridge, "delete_folder", { path: folder });
    expect(refused.ok, refused.error).toBe(true);
    const refusedEntries = (refused.result as { entries?: Array<{ status?: string; reason?: string }> })?.entries ?? [];
    expect(refusedEntries[0]?.status).toBe("failed");
    expect(refusedEntries[0]?.reason).toBe("not_empty");

    // Clean up with force.
    const forced = await callBridge(bridge, "delete_folder", { path: folder, force: true });
    expect(forced.ok, forced.error).toBe(true);
  });

  it("set_mesh_materials_batch reports every rejected assignment and mutates nothing", async () => {
    const r = await callBridge(bridge, "set_mesh_materials_batch", {
      assignments: [
        { assetPath: "/Game/DoesNotExist_MeshMaterialSmoke", materialPath: "/Engine/EngineMaterials/WorldGridMaterial" },
        { assetPath: "/Game/AlsoDoesNotExist_MeshMaterialSmoke", materialPath: "/Engine/EngineMaterials/WorldGridMaterial", slotName: "Trim" },
      ],
    });
    expect(r.method).toBe("set_mesh_materials_batch");
    expect(r.ok, r.error).toBe(true);
    const result = r.result as {
      success?: boolean;
      error?: string;
      preflightPassed?: boolean;
      preflightFailedCount?: number;
      updatedCount?: number;
      items?: Array<{ index?: number; assetPath?: string; ok?: boolean; status?: string; error?: string }>;
    };
    expect(result.success).toBe(false);
    expect(result.error).toContain("Preflight failed");
    expect(result.preflightPassed).toBe(false);
    expect(result.preflightFailedCount).toBe(2);
    expect(result.updatedCount).toBe(0);
    // Both bad entries are accounted for individually, in submission order.
    expect(result.items).toHaveLength(2);
    expect(result.items?.map((i) => i.index)).toEqual([0, 1]);
    for (const item of result.items ?? []) {
      expect(item.ok).toBe(false);
      expect(item.status).toBe("not_found");
      expect(item.error).toContain("no StaticMesh or SkeletalMesh");
    }
  });

  it("set_mesh_materials_batch reports a protected mount per item rather than aborting", async () => {
    const r = await callBridge(bridge, "set_mesh_materials_batch", {
      assignments: [{ assetPath: "/Engine/BasicShapes/Cube", materialPath: "/Engine/EngineMaterials/WorldGridMaterial" }],
      continueOnError: true,
      dryRun: true,
    });
    expect(r.method).toBe("set_mesh_materials_batch");
    expect(r.ok, r.error).toBe(true);
    const result = r.result as { updatedCount?: number; items?: Array<{ status?: string }> };
    expect(result.updatedCount).toBe(0);
    expect(result.items).toHaveLength(1);
    expect(result.items?.[0]?.status).toBe("protected");
  });

  it("set_mesh_materials_batch resolves a slot by name on a real mesh", async ({ skip }) => {
    const search = await callBridge(bridge, "search_assets", { query: "StaticMesh", maxResults: 20 });
    const assets = resultArray(search.result, "assets") ?? [];
    let meshPath: string | undefined;
    let slotName: string | undefined;
    let slotIndex: number | undefined;
    let materialPath: string | undefined;
    for (const asset of assets) {
      const path = ((asset as Record<string, unknown>).path ?? (asset as Record<string, unknown>).objectPath) as string | undefined;
      if (!path || path.startsWith("/Engine/")) continue;
      const info = await callBridge(bridge, "get_mesh_info", { assetPath: path });
      if (!info.ok) continue;
      const slots = (info.result as { materialSlots?: Array<{ index?: number; slotName?: string; materialPath?: string }> })?.materialSlots ?? [];
      const usable = slots.find((s) => s.slotName && s.materialPath);
      if (usable) {
        meshPath = path;
        slotName = usable.slotName;
        slotIndex = usable.index;
        materialPath = usable.materialPath;
        break;
      }
    }
    if (!meshPath || !slotName || !materialPath) skip();

    // dryRun: the point is that slotName resolves to the right index without
    // the caller ever passing one, which is what survives a reimport.
    const r = await callBridge(bridge, "set_mesh_materials_batch", {
      assignments: [{ assetPath: meshPath, materialPath, slotName }],
      dryRun: true,
    });
    expect(r.ok, r.error).toBe(true);
    const result = r.result as {
      success?: boolean;
      items?: Array<{ ok?: boolean; status?: string; slotIndex?: number; slotName?: string; wouldChange?: boolean }>;
    };
    expect(result.success).toBe(true);
    expect(result.items?.[0]?.ok).toBe(true);
    expect(result.items?.[0]?.status).toBe("ok");
    expect(result.items?.[0]?.slotIndex).toBe(slotIndex);
    expect(result.items?.[0]?.slotName).toBe(slotName);
    // Re-assigning the material already in the slot is not a change.
    expect(result.items?.[0]?.wouldChange).toBe(false);
  });
});
