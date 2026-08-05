import { describe, it, expect } from "vitest";
import {
  ComposeError,
  bulletKey,
  comparePrereleaseIds,
  compareVersions,
  composeReleaseNotes,
  fetchHeadline,
  fetchPrereleases,
  issueRefs,
  mergeBodies,
  mergeHeadlines,
  parseBullets,
  parseSections,
  prereleaseTagsFor,
  retitle,
} from "../../scripts/compose-release-notes.mjs";
import { processBody } from "../../scripts/release-headline.mjs";

/** A published prerelease body, frontmatter already stripped by CI. */
function release(tag: string, body: string, headline: string[] = []) {
  return { tag, body, headline };
}

describe("prerelease ordering", () => {
  it("orders numeric identifiers numerically, not lexically", () => {
    expect(comparePrereleaseIds("beta.2", "beta.10")).toBe(-1);
    expect(comparePrereleaseIds("beta.10", "beta.2")).toBe(1);
  });

  it("ranks a bare identifier below the same identifier with a number", () => {
    expect(comparePrereleaseIds("beta", "beta.1")).toBe(-1);
  });

  it("ranks numeric identifiers below alphanumeric ones", () => {
    expect(comparePrereleaseIds("alpha.1", "alpha.beta")).toBe(-1);
  });

  it("ranks a prerelease below its own stable release", () => {
    expect(comparePrereleaseIds("rc.1", null)).toBe(-1);
    expect(compareVersions("1.2.0-rc.1", "1.2.0")).toBe(-1);
    expect(compareVersions("1.2.0", "1.2.1-beta")).toBe(-1);
  });

  it("sorts the prereleases of one version and ignores everything else", () => {
    const tags = [
      "v1.2.0-beta.10",
      "v1.1.44",
      "v1.2.0",
      "v1.2.0-beta",
      "v1.3.0-rc.1",
      "not-a-tag",
      "v1.2.0-beta.2",
      "v1.2.0-rc.1",
    ];
    expect(prereleaseTagsFor("1.2.0", tags)).toEqual([
      "v1.2.0-beta",
      "v1.2.0-beta.2",
      "v1.2.0-beta.10",
      "v1.2.0-rc.1",
    ]);
  });

  it("returns nothing when the version never had a prerelease", () => {
    expect(prereleaseTagsFor("1.1.44", ["v1.1.43", "v1.1.44", "v1.2.0-beta"])).toEqual([]);
  });
});

describe("body parsing", () => {
  const BODY = [
    "## v1.2.0-beta",
    "",
    "One line summary.",
    "",
    "### Server",
    "",
    "- First thing.",
    "  Continued on a second line.",
    "- Second thing.",
    "",
    "### Bug fixes",
    "",
    "Prose above the bullets.",
    "",
    "- A fix (#123).",
  ].join("\n");

  it("keeps the version heading and summary in the preamble", () => {
    const parsed = parseSections(BODY);
    expect(parsed.preamble).toBe("## v1.2.0-beta\n\nOne line summary.");
    expect(parsed.sections.map((s) => s.heading)).toEqual(["Server", "Bug fixes"]);
  });

  it("handles CRLF bodies, which is what the GitHub API returns", () => {
    const parsed = parseSections(BODY.replace(/\n/g, "\r\n"));
    expect(parsed.sections.map((s) => s.heading)).toEqual(["Server", "Bug fixes"]);
    expect(parsed.sections[0].body).not.toContain("\r");
  });

  it("keeps a bullet's continuation lines with the bullet", () => {
    const { bullets } = parseBullets(parseSections(BODY).sections[0].body);
    expect(bullets).toEqual([
      "- First thing.\n  Continued on a second line.",
      "- Second thing.",
    ]);
  });

  it("separates prose above the bullets from the bullets", () => {
    const { lead, bullets } = parseBullets(parseSections(BODY).sections[1].body);
    expect(lead).toBe("Prose above the bullets.");
    expect(bullets).toEqual(["- A fix (#123)."]);
  });

  it("reads issue citations but not version numbers or headings", () => {
    expect([...issueRefs("- Fixes #123 and #45.")]).toEqual(["#123", "#45"]);
    expect([...issueRefs("- Parenthesised (#7).")]).toEqual(["#7"]);
    expect([...issueRefs("- Works on UE 5.7 with #5.7 style anchors")]).toEqual([]);
  });

  it("ignores case and trailing punctuation in the exact key", () => {
    expect(bulletKey("- The Same  Thing.")).toBe(bulletKey("*   the same thing"));
  });
});

describe("section merging", () => {
  it("merges matching sections and keeps first-appearance order", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Server\n\n- Alpha\n\n### Bug fixes\n\n- Bravo" },
      { label: "beta.2", body: "### Bug fixes\n\n- Charlie\n\n### Server\n\n- Delta" },
    ]);
    expect(merged.sections.map((s) => s.heading)).toEqual(["Server", "Bug fixes"]);
    expect(merged.sections[0].bullets).toEqual(["- Alpha", "- Delta"]);
    expect(merged.sections[1].bullets).toEqual(["- Bravo", "- Charlie"]);
  });

  it("appends a section that only the newest input introduced", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Server\n\n- Alpha" },
      { label: "local", body: "### Security\n\n- Bravo" },
    ]);
    expect(merged.sections.map((s) => s.heading)).toEqual(["Server", "Security"]);
  });

  it("matches a heading regardless of case", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Bug fixes\n\n- Alpha" },
      { label: "beta.2", body: "### Bug Fixes\n\n- Bravo" },
    ]);
    expect(merged.sections).toHaveLength(1);
    expect(merged.sections[0].heading).toBe("Bug fixes");
    expect(merged.sections[0].bullets).toEqual(["- Alpha", "- Bravo"]);
  });

  it("carries prose above the bullets from the first input that had it", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Server\n\nThe original lead.\n\n- Alpha" },
      { label: "beta.2", body: "### Server\n\nA later lead.\n\n- Bravo" },
    ]);
    expect(merged.sections[0].lead).toBe("The original lead.");
  });
});

describe("bullet dedupe", () => {
  it("folds an exact repeat, ignoring case and spacing", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Server\n\n- The same thing shipped." },
      { label: "beta.2", body: "### Server\n\n- The  Same thing shipped" },
    ]);
    expect(merged.sections[0].bullets).toEqual(["- The same thing shipped."]);
    expect(merged.duplicates).toHaveLength(1);
    expect(merged.duplicates[0].reason).toBe("same text");
  });

  it("folds two wordings of the same issue and keeps the longer one", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Bug fixes\n\n- Map writes no longer drop entries (#820)." },
      {
        label: "beta.2",
        body:
          "### Bug fixes\n\n- Map writes no longer drop entries, and the count is verified " +
          "against what was asked for (#820).",
      },
    ]);
    expect(merged.sections[0].bullets).toEqual([
      "- Map writes no longer drop entries, and the count is verified against what was asked for (#820).",
    ]);
    expect(merged.duplicates[0].reason).toBe("same issue #820");
  });

  it("keeps the earlier position when a later wording wins", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Server\n\n- Short (#5).\n- Untouched." },
      { label: "beta.2", body: "### Server\n\n- A much longer retelling of it (#5)." },
    ]);
    expect(merged.sections[0].bullets).toEqual([
      "- A much longer retelling of it (#5).",
      "- Untouched.",
    ]);
  });

  it("folds a bullet that moved between sections", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Bug fixes\n\n- Reconnect loop (#404)." },
      { label: "beta.2", body: "### Server\n\n- Reconnect loop reworked (#404)." },
    ]);
    expect(merged.sections.map((s) => s.heading)).toEqual(["Bug fixes", "Server"]);
    expect(merged.sections[0].bullets).toEqual(["- Reconnect loop reworked (#404)."]);
    expect(merged.sections[1].bullets).toEqual([]);
  });

  it("keeps two bullets that merely read alike", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Server\n\n- Faster asset search." },
      { label: "beta.2", body: "### Server\n\n- Faster asset search on large projects." },
    ]);
    expect(merged.sections[0].bullets).toHaveLength(2);
    expect(merged.duplicates).toHaveLength(0);
  });

  it("keeps two fixes that cite different issues", () => {
    const merged = mergeBodies([
      { label: "beta", body: "### Bug fixes\n\n- A fix (#1)." },
      { label: "beta.2", body: "### Bug fixes\n\n- A fix (#2)." },
    ]);
    expect(merged.sections[0].bullets).toHaveLength(2);
  });
});

describe("headline union", () => {
  it("unions in first-appearance order and drops case-insensitive repeats", () => {
    const merged = mergeHeadlines([
      ["Multi-editor sessions", "Bridge protocol"],
      ["bridge protocol", "Widget interaction"],
    ]);
    expect(merged.items).toEqual([
      "Multi-editor sessions",
      "Bridge protocol",
      "Widget interaction",
    ]);
    expect(merged.dropped).toEqual([]);
  });

  it("truncates to six items and reports the ones it dropped", () => {
    const merged = mergeHeadlines([["One A", "Two B", "Three C", "Four D", "Five E", "Six F"], ["Seven G"]]);
    expect(merged.items).toHaveLength(6);
    expect(merged.dropped).toEqual([{ item: "Seven G", reason: "over the 6 item cap" }]);
  });

  it("drops items the publish gate would reject on length or characters", () => {
    const merged = mergeHeadlines([["OK", "Fine one", "Nope: colon", "x".repeat(31)]]);
    expect(merged.items).toEqual(["Fine one"]);
    expect(merged.dropped.map((d) => d.reason)).toEqual([
      "2 chars, allowed 3-30",
      "forbidden characters",
      "31 chars, allowed 3-30",
    ]);
  });

  it("stops before the joined string passes the status-description cap", () => {
    const merged = mergeHeadlines([
      ["A".repeat(30), "B".repeat(30), "C".repeat(30), "D".repeat(30), "E".repeat(30)],
    ]);
    expect(merged.items.join(" · ").length).toBeLessThanOrEqual(140);
    expect(merged.dropped).toHaveLength(1);
  });
});

describe("composeReleaseNotes", () => {
  const BETA = release(
    "v1.2.0-beta",
    [
      "## v1.2.0-beta",
      "",
      "The beta summary.",
      "",
      "### Multi-editor",
      "",
      "- One server, many editors (#800).",
      "",
      "### Bug fixes",
      "",
      "- Map writes keep their entries (#820).",
    ].join("\n"),
    ["Multi-editor sessions", "Struct-keyed TMap safety"]
  );

  const BETA2 = release(
    "v1.2.0-beta.2",
    [
      "## v1.2.0-beta.2",
      "",
      "The second beta summary.",
      "",
      "### Bug fixes",
      "",
      "- Map writes keep their entries, and the stored count is verified (#820).",
      "",
      "### Internals",
      "",
      "- A new test tier.",
    ].join("\n"),
    ["Multi-editor sessions", "Two-bridge test tier"]
  );

  const LOCAL = [
    "---",
    "headline:",
    "  - Landed after the betas",
    "---",
    "",
    "## v1.2.0",
    "",
    "The stable summary.",
    "",
    "### Server",
    "",
    "- One last thing.",
  ].join("\n");

  it("accumulates every prerelease plus the local notes", () => {
    const result = composeReleaseNotes({
      version: "1.2.0",
      prereleases: [BETA, BETA2],
      localNotes: LOCAL,
    });
    expect(result.passthrough).toBe(false);
    expect(result.merged).toEqual(["v1.2.0-beta", "v1.2.0-beta.2"]);

    const { headline, strippedBody } = processBody(result.body);
    expect(headline).toBe(
      "Multi-editor sessions · Struct-keyed TMap safety · Two-bridge test tier · Landed after the betas"
    );
    expect(strippedBody).toContain("## v1.2.0\n");
    expect(strippedBody).not.toContain("v1.2.0-beta");
    expect(strippedBody).toContain("The stable summary.");

    const headings = [...strippedBody.matchAll(/^### (.+)$/gm)].map((m) => m[1]);
    expect(headings).toEqual(["Multi-editor", "Bug fixes", "Internals", "Server"]);

    // The reworded #820 bullet collapsed to the longer variant.
    expect(strippedBody).toContain("the stored count is verified (#820)");
    expect(strippedBody.match(/#820/g)).toHaveLength(1);
    expect(result.duplicates).toHaveLength(1);
  });

  it("falls back to the newest prerelease summary and says so", () => {
    const result = composeReleaseNotes({ version: "1.2.0", prereleases: [BETA, BETA2] });
    expect(result.preambleFrom).toBe("v1.2.0-beta.2");
    expect(processBody(result.body).strippedBody).toContain("The second beta summary.");
  });

  it("passes a stable release with no prereleases straight through", () => {
    const result = composeReleaseNotes({ version: "1.1.44", prereleases: [], localNotes: LOCAL });
    expect(result.passthrough).toBe(true);
    expect(result.body).toBe(LOCAL);
    expect(processBody(result.body).headline).toBe("Landed after the betas");
  });

  it("refuses a stable release with neither prereleases nor notes", () => {
    expect(() => composeReleaseNotes({ version: "1.1.44" })).toThrow(ComposeError);
  });

  it("refuses to compose for a prerelease, whose notes stay incremental", () => {
    expect(() => composeReleaseNotes({ version: "1.2.0-beta.3", prereleases: [BETA] })).toThrow(
      /prerelease notes stay incremental/i
    );
  });

  it("fails loudly when nothing supplies a headline", () => {
    const bare = release("v1.2.0-beta", "## v1.2.0-beta\n\nSummary.\n\n### Server\n\n- Alpha");
    expect(() => composeReleaseNotes({ version: "1.2.0", prereleases: [bare] })).toThrow(
      /no usable headline/i
    );
  });

  it("emits a body the publish gate accepts", () => {
    const result = composeReleaseNotes({
      version: "1.2.0",
      prereleases: [BETA, BETA2],
      localNotes: LOCAL,
    });
    expect(() => processBody(result.body)).not.toThrow();
    expect(result.body.endsWith("\n")).toBe(true);
    expect(result.body).not.toMatch(/\n{3,}/);
  });
});

describe("retitle", () => {
  it("points the version heading at the release being cut", () => {
    expect(retitle("## v1.2.0-beta.2\n\nSummary.", "1.2.0")).toBe("## v1.2.0\n\nSummary.");
  });

  it("adds a heading to a preamble that has none", () => {
    expect(retitle("Summary only.", "1.2.0")).toBe("## v1.2.0\n\nSummary only.");
  });
});

describe("GitHub reads", () => {
  /** Stands in for `gh`, keyed on the subcommand shape each call uses. */
  function fakeGh(statuses: Record<string, string>) {
    return (args: string[]) => {
      if (args[0] === "release" && args[1] === "list") {
        return JSON.stringify([
          { tagName: "v1.2.0-beta.10", isDraft: false },
          { tagName: "v1.2.0-beta.2", isDraft: false },
          { tagName: "v1.2.0-rc.1", isDraft: true },
          { tagName: "v1.1.44", isDraft: false },
        ]);
      }
      if (args[0] === "release" && args[1] === "view") {
        return JSON.stringify({ body: `body of ${args[2]}` });
      }
      if (args[0] === "api") {
        const tag = args[1].split("/")[4];
        if (!(tag in statuses)) throw new Error("no status");
        return statuses[tag];
      }
      throw new Error(`unexpected gh call: ${args.join(" ")}`);
    };
  }

  it("reads the headline back off the landing/headline status", () => {
    const run = fakeGh({ "v1.2.0-beta.2": "First item · Second item\n" });
    expect(fetchHeadline("db-lyon/ue-mcp", "v1.2.0-beta.2", run)).toEqual([
      "First item",
      "Second item",
    ]);
  });

  it("treats a tag with no status as contributing no headline", () => {
    expect(fetchHeadline("db-lyon/ue-mcp", "v1.2.0-beta", fakeGh({}))).toEqual([]);
  });

  it("returns published prereleases in semver order, skipping drafts", () => {
    const found = fetchPrereleases("db-lyon/ue-mcp", "1.2.0", fakeGh({}));
    expect(found.map((r) => r.tag)).toEqual(["v1.2.0-beta.2", "v1.2.0-beta.10"]);
    expect(found[0].body).toBe("body of v1.2.0-beta.2");
  });
});
