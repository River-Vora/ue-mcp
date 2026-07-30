import { z } from "zod";
import { categoryTool, bp, type ToolDef } from "../types.js";

export const networkingTool: ToolDef = categoryTool(
  "networking",
  "Networking and replication: actor replication, property replication, net relevancy, dormancy.",
  {
    set_replicates:        bp("Enable actor replication. Params: blueprintPath, replicates?", "set_replicates"),
    set_property_replicated: bp("Mark a Blueprint variable as replicated. replicationType is 'None' | 'Replicated' | 'RepNotify'; repNotify=true is shorthand for RepNotify and replicated=true for Replicated. Params: blueprintPath, variableName (alias: propertyName), replicationType? | replicated? | repNotify? (#768)", "set_property_replicated", (p) => ({
      blueprintPath: p.blueprintPath,
      variableName: p.variableName ?? p.propertyName,
      replicationType: p.replicationType
        ?? (p.repNotify ? "RepNotify" : p.replicated === true ? "Replicated" : p.replicated === false ? "None" : undefined),
    })),
    configure_net_frequency: bp("Set update frequency. Params: blueprintPath, netUpdateFrequency?, minNetUpdateFrequency?", "configure_net_update_frequency"),
    set_dormancy:          bp("Set net dormancy. Params: blueprintPath, dormancy", "set_net_dormancy"),
    set_net_load_on_client: bp("Control client loading. Params: blueprintPath, loadOnClient?", "set_net_load_on_client"),
    set_always_relevant:   bp("Always network relevant. Params: blueprintPath, alwaysRelevant?", "set_always_relevant"),
    set_only_relevant_to_owner: bp("Only relevant to owner. Params: blueprintPath, onlyRelevantToOwner?", "set_only_relevant_to_owner"),
    configure_cull_distance: bp("Net cull distance. Params: blueprintPath, netCullDistanceSquared?", "configure_net_cull_distance"),
    set_priority:          bp("Net priority. Params: blueprintPath, netPriority?", "set_net_priority"),
    set_replicate_movement: bp("Replicate movement. Params: blueprintPath, replicateMovement?", "set_replicate_movement"),
    get_info:              bp("Get networking info. Params: blueprintPath", "get_networking_info"),
  },
  undefined,
  {
    blueprintPath: z.string().optional(),
    propertyName: z.string().optional(),
    variableName: z.string().optional().describe("set_property_replicated: Blueprint variable name (propertyName is accepted too) (#768)"),
    replicationType: z.string().optional().describe("set_property_replicated: None | Replicated | RepNotify (#768)"),
    replicates: z.boolean().optional(),
    replicated: z.boolean().optional(),
    replicationCondition: z.string().optional(),
    repNotify: z.boolean().optional(),
    netUpdateFrequency: z.number().optional(),
    minNetUpdateFrequency: z.number().optional(),
    dormancy: z.string().optional(),
    loadOnClient: z.boolean().optional(),
    alwaysRelevant: z.boolean().optional(),
    onlyRelevantToOwner: z.boolean().optional(),
    netCullDistanceSquared: z.number().optional(),
    netPriority: z.number().optional(),
    replicateMovement: z.boolean().optional(),
  },
);
