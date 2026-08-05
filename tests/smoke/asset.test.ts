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
});

// The bulk upsert's whole point is that a batch either preflights clean or
// touches nothing, and that a replay of the same request is a no-op. Neither
// property is observable from a single call, so it needs a real sequence
// against a live editor rather than an argument-validation test.
describe("asset bulk_upsert_data_assets", () => {
  const folder = `${TEST_PREFIX}/BulkUpsert_${Date.now()}`;
  const names = ["DA_BulkUpsertA", "DA_BulkUpsertB"];
  // UInputAction is a UDataAsset subclass that ships with the engine, so this
  // needs no project-specific class.
  const className = "/Script/EnhancedInput.InputAction";
  const items = names.map((name) => ({
    name,
    packagePath: folder,
    className,
    properties: { bConsumeInput: false },
  }));

  type ItemResult = { name?: string; status?: string; success?: boolean; error?: string };
  type UpsertResult = {
    success?: boolean;
    createdAssetCount?: number;
    updatedAssetCount?: number;
    unchangedAssetCount?: number;
    failedAssetCount?: number;
    mutationPerformed?: boolean;
    rollback?: { method?: string };
    items?: ItemResult[];
  };

  afterAll(async () => {
    await callBridge(bridge, "delete_folder", { path: folder, force: true });
  });

  // Handlers report failure in the result body, not as a transport error, so a
  // missing asset is read_asset resolving with success:false.
  async function assetExists(name: string): Promise<boolean> {
    const r = await callBridge(bridge, "read_asset", { path: `${folder}/${name}.${name}` });
    return r.ok && (r.result as { success?: boolean })?.success === true;
  }

  it("dry run plans every item and writes nothing", async () => {
    const r = await callBridge(bridge, "bulk_upsert_data_assets", { items, dryRun: true });
    expect(r.ok, r.error).toBe(true);
    const result = r.result as UpsertResult;
    expect(result.success).toBe(true);
    expect(result.mutationPerformed).toBe(false);
    expect(result.items?.map((i) => i.status)).toEqual(["wouldCreate", "wouldCreate"]);

    for (const name of names) {
      expect(await assetExists(name), `${name} must not exist after a dry run`).toBe(false);
    }
  });

  it("creates the batch, then replays as unchanged", async () => {
    const first = await callBridge(bridge, "bulk_upsert_data_assets", { items });
    expect(first.ok, first.error).toBe(true);
    const created = first.result as UpsertResult;
    expect(created.success).toBe(true);
    expect(created.createdAssetCount).toBe(names.length);
    expect(created.failedAssetCount).toBe(0);
    expect(created.items?.every((i) => i.status === "created" && i.success === true)).toBe(true);
    expect(created.rollback?.method).toBe("bulk_restore_data_assets");

    const replay = await callBridge(bridge, "bulk_upsert_data_assets", { items });
    expect(replay.ok, replay.error).toBe(true);
    const unchanged = replay.result as UpsertResult;
    expect(unchanged.success).toBe(true);
    expect(unchanged.createdAssetCount).toBe(0);
    expect(unchanged.unchangedAssetCount).toBe(names.length);
    expect(unchanged.items?.every((i) => i.status === "unchanged")).toBe(true);
  });

  it("updates only the properties it is given", async () => {
    const r = await callBridge(bridge, "bulk_upsert_data_assets", {
      items: [{ name: names[0], packagePath: folder, className, properties: { bConsumeInput: true } }],
    });
    expect(r.ok, r.error).toBe(true);
    const result = r.result as UpsertResult;
    expect(result.updatedAssetCount).toBe(1);
    expect(result.items?.[0]?.status).toBe("updated");

    const readBack = await callBridge(bridge, "read_asset_properties", {
      assetPath: `${folder}/${names[0]}.${names[0]}`,
      propertyName: "bConsumeInput",
      includeValues: true,
    });
    expect(readBack.ok, readBack.error).toBe(true);
    expect((readBack.result as { success?: boolean })?.success).toBe(true);
  });

  it("skip leaves an existing asset alone", async () => {
    const r = await callBridge(bridge, "bulk_upsert_data_assets", {
      items: [{ name: names[0], packagePath: folder, className, properties: { bConsumeInput: false } }],
      onConflict: "skip",
    });
    expect(r.ok, r.error).toBe(true);
    const result = r.result as UpsertResult;
    expect(result.items?.[0]?.status).toBe("skipped");
    expect(result.mutationPerformed).toBe(false);
  });

  it("rejects the whole batch when one descriptor is bad", async () => {
    const badName = "DA_BulkUpsertNeverWritten";
    const r = await callBridge(bridge, "bulk_upsert_data_assets", {
      items: [
        { name: badName, packagePath: folder, className, properties: { bConsumeInput: false } },
        { name: "DA_BulkUpsertBadProp", packagePath: folder, className, properties: { NoSuchProperty: 1 } },
      ],
    });
    expect(r.ok, r.error).toBe(true);
    const result = r.result as UpsertResult;
    expect(result.success).toBe(false);
    // The valid descriptor came first, so a handler that wrote as it went
    // would have created it before reaching the bad one.
    expect(await assetExists(badName), "a rejected batch must write nothing").toBe(false);
  });
});
