# Runtime widget instance inspection

Use `widget(action="inspect_runtime_instances")` when several live widgets share
one class and you need to compare their identity or payload state. Unlike
`get_runtime`, this action returns every matching instance in deterministic
path order and never silently selects the first object iterator match.

The native handler can serialize an explicit `propertyNames` list from the root
widgets and, with `includeSubtree`, matching descendant widgets. Each result
includes object/outer/hierarchy paths and owning-player metadata. Multi-client
PIE sessions can select a world with `pieInstance`.

```json
{
  "action": "inspect_runtime_instances",
  "classFilter": "Hero",
  "propertyNames": ["MemberID", "BuffDynamic", "EffectDynamic"],
  "includeSubtree": true,
  "childClassFilter": "BuffSlot",
  "world": "pie",
  "pieInstance": 1
}
```

Missing requested properties are reported per node rather than failing the
entire query, which makes one call useful across related Blueprint widget
classes. `maxInstances` and `maxNodesPerInstance` keep broad diagnostics bounded.
