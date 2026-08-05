# Widgets

The `widget` category authors UMG assets and drives live widgets while
Play-In-Editor is running. This section covers the parts that need more than a
one-line entry in the [Tool Reference](tool-reference.md).

## Start here

Every action in the category takes the same parameter names for the same
concepts, so read the [parameter contract](widget-parameters.md) first. It is
short, and it removes the guesswork from every other page here.

## Authoring versus runtime

Two different worlds are in play, and mixing them up is the most common source
of confusion:

- **Authoring** actions operate on a Widget Blueprint asset on disk. They work
  whether or not PIE is running.
- **Runtime** actions operate on live `UUserWidget` instances inside a running
  PIE or Game world. They never fall back to the editor world, so calling one
  before PIE starts returns an error rather than quietly answering with an
  Editor Utility Widget.

## Where the runtime actions stop

The runtime actions here are point-in-time: one read of the current state, or
one write plus the delegate broadcast that a real interaction would fire. That
is the line this project keeps. Core does authoring and point-in-time
inspection; recording, replaying, sampling over a window, and synthesizing
input events belong to the [PIE Studio](pie-record-replay.md) plugin.

So reach for PIE Studio instead when you want to:

- capture a widget's state every frame over a window rather than once, which is
  `pie(observe_arm)` and `pie(observe_read)`;
- drive a scripted sequence of interactions with timing, which is
  `pie(inject_input_tape)`;
- synthesize a real key, button or axis event at the input layer rather than
  calling a widget's own setter, which is `pie(inject_input)`;
- record a session and replay it with drift comparison.

The same split already governs the `editor` category: `stage_game_input` sets
the input mode because that is one call, while the injection itself lives in
`pie(inject_input*)`.

## Runtime pages

| Page | Use it when |
|---|---|
| [Runtime widget inspection](runtime-widget-inspection.md) | You need the state of every matching live instance, with stable identity and selected reflected properties. |
| [Runtime widget interaction](runtime-widget-interaction.md) | You need to drive a widget: toggle a checkbox, move a slider, type into a text box, pick a combo entry, or call a UFUNCTION. |
| [Runtime layout diagnostics](runtime-widget-layout-diagnostics.md) | A widget is in the wrong place or the wrong size and you need geometry, slot data, clipping and opacity to find out why. |
