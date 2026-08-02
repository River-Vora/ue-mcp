# Widget subtree extraction

`extract_widget_subtree` is a native editor bridge handler for turning an authored UMG designer subtree into a standalone Widget Blueprint. The equivalent dotted bridge alias is `widget.extract_subtree`.

The handler uses Unreal's widget clipboard serializer instead of rebuilding controls property-by-property. This preserves the selected hierarchy, child order, editable presentation properties, widget bindings, named-slot clipboard extension data, and compatible internal panel slot data. The selected widget becomes the destination root, so its slot in the source widget's external parent is intentionally not copied.

## Parameters

| Field | Required | Description |
| --- | --- | --- |
| `sourceAssetPath` | yes | Source Widget Blueprint asset path. |
| `sourceWidgetName` | yes | Root widget of the subtree to extract. |
| `destinationAssetPath` | yes | Destination package path, including the new asset name. |
| `destinationParentClass` | no | Native or Blueprint `UUserWidget` subclass; defaults to `UserWidget`. |
| `destinationRootName` | no | Name override for the extracted root; descendants keep their names. |
| `dryRun` | no | Defaults to `true`. Returns the deterministic mapping without creating or saving an asset. |

Example request:

```json
{
  "sourceAssetPath": "/Game/UI/WBP_ComplexWindow",
  "sourceWidgetName": "ResultsRowPreview",
  "destinationAssetPath": "/Game/UI/Rows/WBP_ResultsRow",
  "destinationParentClass": "/Script/UMG.UserWidget",
  "destinationRootName": "ResultsRow",
  "dryRun": true
}
```

## Safety and idempotency

- Preflight validates source and destination paths, parent class, deterministic names, and destination collisions without mutation.
- A destination must be absent or empty. A non-empty destination is accepted only when its widget names, classes, hierarchy, and child order already match the requested extraction; that replay returns `existed: true`.
- The source asset is never compiled or saved. Mutation compiles and saves only the destination.
- Import or compile failure removes a destination created by the request instead of leaving a partially authored asset on disk.
- Successful creation includes a `delete_asset` rollback descriptor.
