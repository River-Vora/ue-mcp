import { z } from "zod";
import { EngineConfigSchema } from "@db-lyon/flowkit";

/**
 * The `ue-mcp:` block at the top of ue-mcp.yml. Historically just had
 * `version: 1`. As of 1.0.29, also hosts the project-level config that
 * used to live in the separate `.ue-mcp.json` file (killed for one-config-
 * format consistency).
 *
 *   ue-mcp:
 *     version: 1
 *     contentRoots: ["/Game/"]
 *     disable: ["gas"]
 *     http: { enabled: false, port: 7723 }
 *     feedback: { mode: "interactive" }
 *     installedHooks: [...]            # user-local; lives in ue-mcp.local.yml
 *
 * Tracked vs untracked split is by convention:
 *   ue-mcp.yml         - tracked. version, contentRoots, disable, http, feedback.
 *   ue-mcp.local.yml   - gitignored. installedHooks and anything else user-local.
 * flowkit's loader deep-merges local on top of yml so both surface as one
 * resolved config to the server.
 */
export const FlowVersionSchema = z.object({
  version: z.literal(1),
  contentRoots: z.array(z.string()).optional(),
  disable: z.array(z.string()).optional(),
  http: z
    .object({
      enabled: z.boolean().optional(),
      port: z.number().int().min(1).max(65535).optional(),
      host: z.string().optional(),
    })
    .optional(),
  feedback: z
    .object({
      mode: z.enum(["interactive", "auto-approve", "defer"]).optional(),
    })
    .optional(),
  installedHooks: z.array(z.string()).optional(),
}).passthrough();

export const FlowProjectSchema = z.object({
  name: z.string().optional(),
  engine: z.string().optional(),
}).optional();

export const GitSnapshotSchema = z.object({
  enabled: z.boolean().default(false),
  paths: z.array(z.string()).default(["Content", "Config"]),
  snapshot_dir: z.string().default(".ue-mcp/snapshot.git"),
  max_age_hours: z.number().default(24),
}).optional();

/**
 * A plugin entry in the user's ue-mcp.yml.
 * `name` is the npm package; `version` is optional and honored at resolve time.
 */
export const PluginEntrySchema = z.object({
  name: z.string().min(1),
  version: z.string().optional(),
});

export type PluginEntry = z.infer<typeof PluginEntrySchema>;

export const FlowConfigSchema = EngineConfigSchema.extend({
  "ue-mcp": FlowVersionSchema.optional(),
  project: FlowProjectSchema,
  git_snapshot: GitSnapshotSchema,
  plugins: z.array(PluginEntrySchema).default([]),
});

export type FlowConfig = z.infer<typeof FlowConfigSchema>;
