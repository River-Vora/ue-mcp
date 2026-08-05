import { z } from "zod";
import { categoryTool, bp, type ToolDef } from "../types.js";

export const demoTool: ToolDef = categoryTool(
  "demo",
  "Neon Shrine demo scene builder and cleanup.",
  {
    step:    bp("Execute demo step. Params: stepIndex?", "demo_step", (p) => p.stepIndex !== undefined ? { step: p.stepIndex } : {}),
    get_steps: bp("List every demo step up front: index, id and description, plus a count. Use this to see what the 19 steps build before running any of them. Params: none", "demo_get_steps"),
    cleanup: bp("Remove demo assets and actors. Switches editor to /Game/MCP_Home before deleting so the editor is never left on an Untitled map.", "demo_cleanup"),
    go_home: bp("Switch the editor to /Game/MCP_Home (creating it on first use). Use this before any operation that would leave the editor on an Untitled map.", "demo_go_home"),
  },
  undefined,
  {
    stepIndex: z.number().optional().describe("Step index to execute. Omit for step list."),
  },
);
