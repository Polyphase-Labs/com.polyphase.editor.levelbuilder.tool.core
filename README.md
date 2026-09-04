# `com.polyphase.editor.levelbuilder.tool.core`

**Status:** Functional — all seven shape brushes shipped · **Layer:** sibling (depends on `core`) · **Hot-reload safe:** yes

Generic, kit-agnostic brushes that compose on top of whichever sibling
owns the active tool. Brushes never know what a kit is: every spawn
routes back through the active sibling's registered spawn function via
core's v4 `GetSpawnFnForActiveTool()`.

Sibling to [`modular`](../com.polyphase.editor.levelbuilder.modular/README.md)
and [`grid`](../com.polyphase.editor.levelbuilder.grid/README.md);
sits on top of [`core`](../com.polyphase.editor.levelbuilder.core/README.md).

---

## Why it's separate

Every sibling addon ships its own placement semantics:

- **modular** spawns StaticMesh or Scene with socket-aware snap.
- **grid** spawns onto a cell grid with no sockets.
- **(future) voxel / spline** will have their own rules.

Without `tool.core`, every sibling would re-implement Line / Box / Fill
brushes. With it, you write the brush *once* and the brush asks core
"who owns the active tool?" — that sibling's spawn-fn handles the
StaticMesh-vs-Scene distinction, snap, and bookkeeping for each shape
point. Brushes stay simple; siblings stay independent.

## What's shipped

All seven brushes from the T-series ship and are feature-complete:

| Brush     | Flow            | Distribution                       | Multi-target | Mask gating |
| --------- | --------------- | ---------------------------------- | ------------ | ----------- |
| Line      | 2-click         | Stride-stepped along segment       | ✓            | 1D (along U) |
| Box       | 2-click         | Stride-stepped perimeter (4 edges) | ✓            | 1D (perimeter sweep) |
| BoxFill   | 2-click         | Stride-grid inside rectangle       | ✓            | 2D (cell UV) |
| NoiseFill | 2-click         | 2D value-noise scatter             | ✓            | 2D, AND-gated with noise |
| Paint     | LMB-drag        | Density × disc area × dt           | ✓            | 2D (disc UV) |
| Replace   | Click           | Single-piece swap in radius        | ✓ (multi-target) | n/a |
| MaskFill  | 2-click         | Per-channel from RGBA PNG          | ✓ (per channel) | RGB channels + per-pixel threshold |

**Shared UX across every brush:**
- **Multi-target picker**: thumbnail-grid modal with kit pieces; random pick per spawn. Empty list falls back to the active palette piece.
- **Optional mask brush**: load a PNG mask; cursor area maps to mask UV; gate spawns by luminance threshold (+ invert).
- **Undo grouping**: each commit pushes one engine `EditorAction` group — single Ctrl+Z reverts the whole stroke.
- **Eyedropper-style "click to capture"** (Replace): pick the source piece from the scene without leaving the brush.
- **Viewport preview** (cyan / amber / magenta wire shapes) per brush geometry.

## Shared helpers

To keep the seven brushes from drifting, common functionality is
extracted into shared files all brushes use:

| File                    | Purpose |
| ----------------------- | ------- |
| `LBToolPicker.{h,cpp}`   | Thumbnail-grid piece-picker modal; `CollectActiveKitPieces`, `DrawPiecePickerModal`, `PickFromList` (random-pick helper for spawn loops). |
| `LBToolMaskImage.{h,cpp}` | PNG mask state, decode (stb_image, internal-linkage), sampling (`Sample(state, u, v)`), and the standard Browse/Reload/96×96 thumbnail row (`DrawMaskUI`). |
| `LBToolDistribution.{h,cpp}` | RNG helpers + `JitteredYaw`, `RandomInDisc`, `Noise2D` (value noise for NoiseFill). |
| `LBToolShared.{h,cpp}`   | `BuildStrideWalk` (stride-stepping a segment), shared math used by Line/Box/BoxFill. |
| `ThumbnailCache.{h,cpp}` | PNG → `ImTextureID` cache used by every picker thumbnail. |

## How a brush is wired

Every brush:

1. Has **no data members** — all state lives in file-static variables in
   an anonymous namespace inside `LBTool<Name>.cpp`. Matches the
   "no-data-members on the C-ABI base class" convention from
   `LevelBuilderInterfaces.h`.
2. Registers itself via `api->RegisterBrush(name, &gBrush)` in
   `Initialize`; unregisters in `Shutdown`.
3. Renders its parameter UI by overriding `DrawSettingsUI()` — core
   draws it inside the Brush tab automatically when the brush is active.
4. Commits placements by calling `api->GetSpawnFnForActiveTool(&ud)`,
   then `spawnFn(assetName, &pos, &rot, ud)` for every shape point.
   `assetName = nullptr` means "use the active palette item" (the
   common case); pass a specific asset name when the brush is using
   the multi-target picker.
5. Optional: overrides `NeedsArmedPreview()` to opt out of preview-armed
   gating (Replace does this — its viewport behavior is "click on a
   scene piece" not "armed preview").
6. Optional: registers a viewport-overlay callback that gates on
   "active brush == me" and draws a live preview.

For new brushes, pick the closest reference impl:

| Pattern | Reference brush |
| ------- | ---------------- |
| Two-click commit | `LBToolLine` (simplest), `LBToolBox` (multi-edge), `LBToolBoxFill` (2D grid) |
| LMB-drag continuous | `LBToolPaint` (density × area × dt) |
| Per-cell noise scatter | `LBToolNoiseFill` |
| Per-cell RGBA channel routing | `LBToolMaskFill` |
| Click-on-scene-piece (no armed preview) | `LBToolReplace` |

## Build

```bat
cd Packages\com.polyphase.editor.levelbuilder.tool.core
build.bat
```

Linux: `./build.sh`. Output:
`build/Windows/x64/<config>/com.polyphase.editor.levelbuilder.tool.core.dll`.

Must follow a core rebuild — strict-equality ABI gate. Reload via
**Tools → Addons → Reload Native Addons**.

## Source layout

```
Source/
├── ComPolyphaseEditorLevelbuilderToolCore.cpp  ← plugin entry
├── LevelBuilderCoreAPI.h                       ← synced copy from core
├── LevelBuilderInterfaces.h                    ← synced copy from core
├── LevelBuilderCoreLoader.{h,cpp}              ← late-bound resolver
├── LBToolShared.{h,cpp}                        ← stride walker + edge math
├── LBToolDistribution.{h,cpp}                  ← RNG + noise + jitter
├── LBToolPicker.{h,cpp}                        ← shared piece-picker modal
├── LBToolMaskImage.{h,cpp}                     ← shared PNG mask helper
├── ThumbnailCache.{h,cpp}                      ← PNG → ImTextureID
├── LBToolLine.{h,cpp}                          ← T1 — stride-stepped line
├── LBToolBox.{h,cpp}                           ← T2 — perimeter (4 edges)
├── LBToolBoxFill.{h,cpp}                       ← T2 — filled rectangle
├── LBToolNoiseFill.{h,cpp}                     ← T3 — value-noise scatter
├── LBToolReplace.{h,cpp}                       ← T4 — swap-in-radius
├── LBToolPaint.{h,cpp}                         ← T6 — drag-paint
└── LBToolMaskFill.{h,cpp}                      ← T7 — RGBA-channel routing
```

## Where to read next

- [`../../Documentation/Developers/ToolCore.md`](../../Documentation/Developers/ToolCore.md)
  — design notes (some predate the actual implementations; treat the
  shipped code as the source of truth).
- [`Source/LBToolLine.cpp`](Source/LBToolLine.cpp) — simplest two-click
  brush. Read first.
- [`Source/LBToolPaint.cpp`](Source/LBToolPaint.cpp) — drag-paint pattern;
  also showcases multi-target + mask integration end-to-end.
- [`../com.polyphase.editor.levelbuilder.modular/Source/ModularPlacement.cpp`](../com.polyphase.editor.levelbuilder.modular/Source/ModularPlacement.cpp)
  `ModularSpawnAtTransform` — the canonical sibling spawn-fn signature.
