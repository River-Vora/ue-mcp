# Runtime Widget Layout Diagnostics

`widget.get_runtime` includes a read-only layout snapshot for every returned
runtime widget node while PIE is active. It complements the native Widget
Reflector with structured output that an automated client can inspect and
compare.

Each node reports:

- desired, local, and absolute size;
- absolute position, layout bounds, and render bounds;
- render transform, pivot, accumulated layout scale, and effective opacity;
- complete reflected slot properties;
- structured Canvas anchors, offsets, alignment, auto-size, and Z-order;
- authored clipping mode and a derived effective clipping rectangle;
- parent bounds and whether the child extends beyond them;
- diagnostics for suspicious layout relationships.

The handler retains the previous capture for the same PIE widget instance and
optional `childName`. Calling `widget.get_runtime` again after moving, resizing,
toggling, or changing the viewport produces `deltaSincePreviousCapture` for
each node. This detects position-dependent dimensions such as a vertically
stretched Canvas slot whose `Bottom` offset was mistakenly treated as height.

## Recommended debugging sequence

1. Start client-mode PIE on the required test map.
2. Open the affected screen.
3. Capture the PIE window for visual evidence.
4. Call `widget.get_runtime` for the affected widget class or instance.
5. Reproduce one layout-affecting change.
6. Call `widget.get_runtime` again and inspect changed nodes and diagnostics.
7. Inspect designer properties and bindings before changing presentation.

The reported clipping rectangle is derived from UMG clipping modes and cached
render bounds. It is suitable for layout diagnosis, but it is not a serialized
copy of Slate's paint-element clip stack. Use a native Widget Reflector
`.widgetsnapshot` when exact paint clipping, hit-test grids, or Slate source
addresses are required.
