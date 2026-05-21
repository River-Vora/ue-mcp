import { describe, it, expect, beforeEach } from "vitest";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import yaml from "js-yaml";
import { ProjectContext } from "../../src/project.js";

function makeTempProject(): string {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ue-mcp-project-test-"));
  const uproject = path.join(dir, "Test.uproject");
  fs.writeFileSync(
    uproject,
    JSON.stringify({ FileVersion: 3, EngineAssociation: "5.7", Plugins: [] }, null, 2),
  );
  fs.mkdirSync(path.join(dir, "Content"), { recursive: true });
  return uproject;
}

describe("ProjectContext.resolveContentPath", () => {
  let ctx: ProjectContext;
  let uproject: string;

  beforeEach(() => {
    uproject = makeTempProject();
    ctx = new ProjectContext();
    ctx.setProject(uproject);
  });

  it("appends .uasset to game paths without an extension", () => {
    const out = ctx.resolveContentPath("/Game/MyAsset");
    expect(out.endsWith("MyAsset.uasset")).toBe(true);
  });

  it("preserves .umap extension", () => {
    const out = ctx.resolveContentPath("/Game/MyLevel.umap");
    expect(out.endsWith("MyLevel.umap")).toBe(true);
    expect(out.endsWith(".uasset")).toBe(false);
  });

  it("treats a trailing slash as a directory (no .uasset suffix)", () => {
    const out = ctx.resolveContentPath("/Game/MyFolder/");
    expect(out.endsWith(".uasset")).toBe(false);
    expect(out.endsWith("MyFolder")).toBe(true);
  });

  it("treats a trailing backslash as a directory", () => {
    const out = ctx.resolveContentPath("/Game/MyFolder\\");
    expect(out.endsWith(".uasset")).toBe(false);
  });
});

describe("ProjectContext config loading", () => {
  it("ignores a malformed ue-mcp.yml without throwing", () => {
    const uproject = makeTempProject();
    const projectDir = path.dirname(uproject);
    fs.writeFileSync(path.join(projectDir, "ue-mcp.yml"), "this: is: not: valid yaml: at all:");

    const ctx = new ProjectContext();
    expect(() => ctx.setProject(uproject)).not.toThrow();
    expect(ctx.config).toEqual({});
  });

  it("loads a valid ue-mcp.yml", () => {
    const uproject = makeTempProject();
    const projectDir = path.dirname(uproject);
    fs.writeFileSync(
      path.join(projectDir, "ue-mcp.yml"),
      yaml.dump({
        "ue-mcp": {
          version: 1,
          disable: ["gas"],
          http: { enabled: true, port: 7723 },
        },
      }),
    );

    const ctx = new ProjectContext();
    ctx.setProject(uproject);
    expect(ctx.config.disable).toEqual(["gas"]);
    expect(ctx.config.http?.port).toBe(7723);
  });

  it("merges ue-mcp.local.yml on top of ue-mcp.yml", () => {
    const uproject = makeTempProject();
    const projectDir = path.dirname(uproject);
    fs.writeFileSync(
      path.join(projectDir, "ue-mcp.yml"),
      yaml.dump({ "ue-mcp": { version: 1, disable: ["gas"] } }),
    );
    fs.writeFileSync(
      path.join(projectDir, "ue-mcp.local.yml"),
      yaml.dump({
        "ue-mcp": {
          installedHooks: ["C:/Users/test/.claude/settings.json"],
        },
      }),
    );

    const ctx = new ProjectContext();
    ctx.setProject(uproject);
    expect(ctx.config.disable).toEqual(["gas"]);
    expect(ctx.config.installedHooks).toEqual([
      "C:/Users/test/.claude/settings.json",
    ]);
  });

  it("rejects a ue-mcp.yml with wrong types in the ue-mcp: block", () => {
    const uproject = makeTempProject();
    const projectDir = path.dirname(uproject);
    fs.writeFileSync(
      path.join(projectDir, "ue-mcp.yml"),
      yaml.dump({ "ue-mcp": { version: 1, disable: "gas" } }),
    );

    const ctx = new ProjectContext();
    ctx.setProject(uproject);
    expect(ctx.config).toEqual({});
  });

  it("migrates a legacy .ue-mcp.json into the YAML files and deletes the JSON", () => {
    const uproject = makeTempProject();
    const projectDir = path.dirname(uproject);
    const jsonPath = path.join(projectDir, ".ue-mcp.json");
    fs.writeFileSync(
      jsonPath,
      JSON.stringify({
        contentRoots: ["/Game/", "/MyPlugin/"],
        disable: ["gas"],
        installedHooks: ["C:/some/settings.json"],
        feedback: { mode: "defer" },
      }),
    );

    const ctx = new ProjectContext();
    ctx.setProject(uproject);

    // Legacy file is gone.
    expect(fs.existsSync(jsonPath)).toBe(false);

    // Tracked fields moved to ue-mcp.yml.
    const yml = yaml.load(
      fs.readFileSync(path.join(projectDir, "ue-mcp.yml"), "utf-8"),
    ) as { "ue-mcp": Record<string, unknown> };
    expect(yml["ue-mcp"].disable).toEqual(["gas"]);
    expect(yml["ue-mcp"].contentRoots).toEqual(["/Game/", "/MyPlugin/"]);
    expect(yml["ue-mcp"].feedback).toEqual({ mode: "defer" });
    expect(yml["ue-mcp"].installedHooks).toBeUndefined();

    // installedHooks moved to ue-mcp.local.yml.
    const local = yaml.load(
      fs.readFileSync(path.join(projectDir, "ue-mcp.local.yml"), "utf-8"),
    ) as { "ue-mcp": Record<string, unknown> };
    expect(local["ue-mcp"].installedHooks).toEqual(["C:/some/settings.json"]);

    // The merged config that the context exposes has everything.
    expect(ctx.config.disable).toEqual(["gas"]);
    expect(ctx.config.feedback?.mode).toBe("defer");
    expect(ctx.config.installedHooks).toEqual(["C:/some/settings.json"]);
  });
});
