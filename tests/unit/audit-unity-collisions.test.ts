import { describe, it, expect } from "vitest";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { findUnityCollisions } from "../../scripts/audit-unity-collisions.mjs";

const REPO_ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const PLUGIN_ROOT = path.join(REPO_ROOT, "plugin");

/**
 * Lays out .cpp files the way UBT expects, so the audit resolves the module
 * name from the path exactly as it does in the real tree.
 */
function moduleTree(files: Record<string, string>): string {
  const root = mkdtempSync(path.join(tmpdir(), "unity-collisions-"));
  const dir = path.join(root, "Source", "TestModule", "Private");
  mkdirSync(dir, { recursive: true });
  for (const [name, body] of Object.entries(files)) {
    writeFileSync(path.join(dir, name), body);
  }
  return root;
}

describe("the shipped plugin sources", () => {
  it("define no file-local helper twice within one module", () => {
    // A duplicate here compiles on the machine that wrote it and fails on a
    // user's build the moment unity groups the two files together. v1.2.0
    // shipped exactly that: IsProtectedAssetPath in both AssetHandlers.cpp and
    // AssetHandlers_BulkUpsert.cpp.
    const collisions = findUnityCollisions(PLUGIN_ROOT);
    const rendered = collisions
      .map((c) => `[${c.module}] ${c.signature}\n    ${c.files.join("\n    ")}`)
      .join("\n");
    expect(rendered).toBe("");
  });
});

describe("findUnityCollisions", () => {
  it("reports a helper defined identically in two files of one module", () => {
    const root = moduleTree({
      "A.cpp": "namespace\n{\n\tbool IsProtected(const FString& Path)\n\t{\n\t\treturn false;\n\t}\n}\n",
      "B.cpp": "namespace\n{\n\tbool IsProtected(const FString& Path)\n\t{\n\t\treturn true;\n\t}\n}\n",
    });
    const collisions = findUnityCollisions(root);
    expect(collisions).toHaveLength(1);
    expect(collisions[0].signature).toBe("IsProtected(const FString &)");
    expect(collisions[0].module).toBe("TestModule");
    expect(collisions[0].files).toHaveLength(2);
  });

  it("ignores the parameter names, which do not participate in overloading", () => {
    const root = moduleTree({
      "A.cpp": "namespace\n{\n\tbool Check(const FString& Path)\n\t{\n\t\treturn false;\n\t}\n}\n",
      "B.cpp": "namespace\n{\n\tbool Check(const FString& Other)\n\t{\n\t\treturn true;\n\t}\n}\n",
    });
    expect(findUnityCollisions(root)).toHaveLength(1);
  });

  it("accepts genuine overloads, which share a unity blob legally", () => {
    // FindNodeByName and PinToJson really are spelled this way in two handler
    // files each, and really do compile: the parameter types differ.
    const root = moduleTree({
      "A.cpp": "namespace\n{\n\tUSoundNode* FindNodeByName(USoundCue* Cue, const FString& Name)\n\t{\n\t\treturn nullptr;\n\t}\n}\n",
      "B.cpp": "namespace\n{\n\tUPCGNode* FindNodeByName(UPCGGraph* Graph, const FString& Name)\n\t{\n\t\treturn nullptr;\n\t}\n}\n",
    });
    expect(findUnityCollisions(root)).toEqual([]);
  });

  it("accepts the same helper in two different modules", () => {
    const root = mkdtempSync(path.join(tmpdir(), "unity-collisions-"));
    for (const mod of ["ModuleOne", "ModuleTwo"]) {
      const dir = path.join(root, "Source", mod, "Private");
      mkdirSync(dir, { recursive: true });
      writeFileSync(
        path.join(dir, "Handler.cpp"),
        "namespace\n{\n\tbool Check(const FString& Path)\n\t{\n\t\treturn false;\n\t}\n}\n"
      );
    }
    expect(findUnityCollisions(root)).toEqual([]);
  });

  it("does not mistake a call inside a function body for a definition", () => {
    const root = moduleTree({
      "A.cpp": "namespace\n{\n\tvoid Run()\n\t{\n\t\tif (Check(Path))\n\t\t{\n\t\t\tDoThing();\n\t\t}\n\t}\n}\n",
      "B.cpp": "namespace\n{\n\tvoid Other()\n\t{\n\t\tif (Check(Path))\n\t\t{\n\t\t\tDoThing();\n\t\t}\n\t}\n}\n",
    });
    expect(findUnityCollisions(root)).toEqual([]);
  });

  it("does not read braces inside comments or string literals as scope", () => {
    const root = moduleTree({
      "A.cpp":
        'namespace\n{\n\t// bool Check(const FString& Path) {\n\tconst TCHAR* Brace = TEXT("{");\n\tbool Real(int32 Value)\n\t{\n\t\treturn Value > 0;\n\t}\n}\n',
      "B.cpp": "namespace\n{\n\tbool Real(int32 Value)\n\t{\n\t\treturn Value < 0;\n\t}\n}\n",
    });
    const collisions = findUnityCollisions(root);
    expect(collisions).toHaveLength(1);
    expect(collisions[0].signature).toBe("Real(int32)");
  });
});
