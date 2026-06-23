/**
 * @file LevelBuilderCoreAPI.h
 * @brief Stable C ABI exposed by the Level Builder Core addon to sibling
 *        addons (modular, grid, voxel, etc.) loaded in the same process.
 *
 * Sibling addons resolve `LevelBuilderCore_GetAPI` at runtime via
 * GetModuleHandle/GetProcAddress (Windows) or dlsym(RTLD_DEFAULT) (POSIX).
 * They MUST NOT link against the core addon's import library — the lookup
 * is intentionally late-bound so the core can hot-reload without dragging
 * everything else down.
 *
 * Types crossing the DLL boundary are POD / C-ABI: no std::string, no
 * std::vector, no allocator-bearing types. Strings are `const char*`
 * pointers; the caller owns lifetime unless the contract says otherwise.
 *
 * Abstract base classes (LevelBuilderTool/Brush/SnapProvider) are
 * pure-virtual with NO data members. Sibling addons may derive from them
 * and pass derived pointers back to core — the vtable layout matches as
 * long as both DLLs are built with the same MSVC ABI.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
    #ifdef LEVELBUILDER_CORE_BUILD
        #define LEVELBUILDER_CORE_API __declspec(dllexport)
    #else
        #define LEVELBUILDER_CORE_API __declspec(dllimport)
    #endif
#else
    #define LEVELBUILDER_CORE_API __attribute__((visibility("default")))
#endif

// v13: Rebuild-from-world hooks — siblings register a callback that
//      walks the scene and re-registers placed pieces. Core fires
//      every registered callback on Level Builder mode activate so
//      Paint Erase / Replace see pieces that were placed BEFORE this
//      session (project save+load, hot-reload). Manual "Rebuild From
//      World" button in each sibling's Debug accordion still works.
//
// v12: Replace eyedropper — LBVisitFn now receives the piece's asset
//      name alongside the node pointer. Lets tool.core's Replace brush
//      "pick source from scene" mode read the asset of whatever was
//      clicked without a separate registry-lookup ABI. Old visitors
//      that ignore the new parameter still compile if they switch to
//      the new typedef — both Paint (Erase) and Replace use it.
//
// v11: Replace brush asset filter — LBEnumeratePlacementsFn gains a
//      `sourceAssetFilter` parameter. Null / empty = no filter (legacy
//      behavior). Non-empty = sibling only visits placed pieces whose
//      asset name matches the filter. Lets the Replace brush swap
//      only specific source pieces (e.g. all "wall_grey_001" → final
//      art) without touching neighboring unrelated pieces.
//
// v10: R4 piece-level mutation surface — Kit_CreateKit / Kit_DeleteKit /
// Kit_SetKitName for kit-level ops, Kit_AddPiece / Kit_RemovePiece +
// Kit_SetPieceName / Asset / Category / IconPath for piece-level ops. Kit
// metadata setters already shipped in v6 (Kit_SetMetaString); socket
// mutation shipped in v5 (Kit_AddSocket et al). Batch semantics are
// implicit — every mutation edits core's in-memory kit; Kit_Save flushes
// the whole kit as one JSON snapshot.
//
// v9: removed addon-side LBHistory shim (v8). Undo now goes through the
// engine's PolyphaseEngineAPI::EditorAction_Push / _BeginGroup / _EndGroup
// (engine plugin-API v6+), so the level-builder's spawn / destroy
// interleaves correctly with the editor's native ActionManager.
#define LEVEL_BUILDER_CORE_API_VERSION 13

// ===== Forward declarations =====

class LevelBuilderTool;
class LevelBuilderBrush;
class LevelBuilderSnapProvider;
class LevelBuilderPalette;

// ===== C-ABI POD types =====

struct LBVec3 { float x, y, z; };
struct LBQuat { float x, y, z, w; };

enum LevelBuilderToolState
{
    LBToolState_Inactive   = 0,
    LBToolState_Hovering   = 1,
    LBToolState_Previewing = 2,
    LBToolState_Placing    = 3,
    LBToolState_Dragging   = 4
};

enum LevelBuilderPreviewState
{
    LBPreview_Hidden          = 0,
    LBPreview_Valid           = 1,
    LBPreview_Invalid         = 2,
    LBPreview_Overlapping     = 3,
    LBPreview_MissingAsset    = 4
};

struct LevelBuilderPlacementRequest
{
    const char* assetName;   // engine asset name (UUID or path-based)
    LBVec3      position;
    LBQuat      rotation;
    LBVec3      scale;
    void*       parentNode;  // Node* (opaque); nullptr = scene root
};

struct LevelBuilderPlacementResult
{
    int          success;       // 0=fail, 1=ok
    void*        spawnedNode;   // Node* (opaque) — nullptr on failure
    const char*  errorMessage;  // static string, valid until next Place() call
};

struct LevelBuilderPaletteItem
{
    const char* displayName;
    const char* assetName;
    const char* category;
    const char* iconPath;
    const char* tags;
};

// ===== v3 — kit snapshot POD types =====
//
// Filled by Kit_GetInfo / Kit_GetPieceInfo / Kit_GetSocketInfo. The string
// pointers inside reference core's internal storage (LBKit / LBPiece /
// LBSocket member std::string buffers); they are valid until the next
// Kit_Reload, Kit_Save, or any mutation API call. Siblings that need to
// hang on across frames should copy out of the struct immediately.

struct LBKitInfo
{
    const char* name;
    const char* sourceFile;   // empty if kit is in-memory only (built-in)
    int         pieceCount;
};

struct LBPieceInfo
{
    const char* name;
    const char* assetName;
    const char* category;
    const char* iconPath;
    float       size[3];
    int         socketCount;
};

// v6 — sharing metadata snapshot (see KitSharing.md §3). Filled by
// Kit_GetMetaInfo. String pointers are valid until the next Kit_Reload,
// Kit_Save, or any Kit_SetMetaString mutation. `tagCount`/`dependencyCount`
// reflect the kit's current state — siblings iterate via Kit_GetTagAt /
// Kit_GetDependencyInfo.
struct LBKitMetaInfo
{
    const char* kitId;
    const char* kitVersion;
    const char* author;
    const char* authorUrl;
    const char* license;
    const char* licenseUrl;
    const char* description;
    const char* previewImage;
    const char* homepage;
    const char* minimumPolyphaseVersion;
    int         tagCount;
    int         dependencyCount;
    int         kitIdSynthesized;   // 1 = id is a content-hash placeholder
};

struct LBKitDependencyInfo
{
    const char* kitId;
    const char* version;
};

// Metadata field IDs accepted by Kit_SetMetaString. Stable ordering —
// new fields append at the end so callers compiled against an older
// header stay binary-compatible.
enum LBKitMetaField
{
    LBKitMeta_KitId                   = 0,
    LBKitMeta_KitVersion              = 1,
    LBKitMeta_Author                  = 2,
    LBKitMeta_AuthorUrl               = 3,
    LBKitMeta_License                 = 4,
    LBKitMeta_LicenseUrl              = 5,
    LBKitMeta_Description             = 6,
    LBKitMeta_PreviewImage            = 7,
    LBKitMeta_Homepage                = 8,
    LBKitMeta_MinimumPolyphaseVersion = 9
};

struct LBSocketInfo
{
    const char* name;
    const char* type;
    float       position[3];
    float       rotationDeg[3];   // YXZ Euler
    int         compatibleTypeCount;
};

// ===== Cross-DLL API surface =====
//
// Function-pointer table returned by LevelBuilderCore_GetAPI(). New entries
// MUST be appended at the end of the struct to preserve ABI for siblings
// built against older headers. Sibling addons MUST null-check each pointer
// before calling so they keep working on older core builds.

struct LevelBuilderCoreAPI
{
    uint32_t apiVersion;        // Must match LEVEL_BUILDER_CORE_API_VERSION

    // ----- Tool registry -----
    void  (*RegisterTool)(const char* name, LevelBuilderTool* tool);
    void  (*UnregisterTool)(const char* name);
    LevelBuilderTool* (*FindTool)(const char* name);
    void  (*SetActiveTool)(const char* name);
    const char* (*GetActiveToolName)();

    // ----- Snap provider registry -----
    void  (*RegisterSnapProvider)(const char* name, LevelBuilderSnapProvider* sp);
    void  (*UnregisterSnapProvider)(const char* name);
    LevelBuilderSnapProvider* (*FindSnapProvider)(const char* name);
    void  (*SetActiveSnapProvider)(const char* name);
    const char* (*GetActiveSnapProviderName)();

    // ----- Brush registry -----
    void  (*RegisterBrush)(const char* name, LevelBuilderBrush* brush);
    void  (*UnregisterBrush)(const char* name);
    LevelBuilderBrush* (*FindBrush)(const char* name);
    void  (*SetActiveBrush)(const char* name);
    const char* (*GetActiveBrushName)();

    // ----- Palette registry -----
    void  (*RegisterPalette)(const char* name, LevelBuilderPalette* palette);
    void  (*UnregisterPalette)(const char* name);
    LevelBuilderPalette* (*FindPalette)(const char* name);
    void  (*SetActivePalette)(const char* name);
    const char* (*GetActivePaletteName)();
    LevelBuilderPalette* (*GetActivePalette)();

    // ----- Palette item ops (so siblings don't need to include Palette.h) -----
    LevelBuilderPalette* (*CreatePalette)(const char* name);
    void  (*Palette_Clear)(LevelBuilderPalette* p);
    void  (*Palette_AddItem)(LevelBuilderPalette* p, const LevelBuilderPaletteItem* item);
    int   (*Palette_GetItemCount)(LevelBuilderPalette* p);
    int   (*Palette_GetItem)(LevelBuilderPalette* p, int index, LevelBuilderPaletteItem* outItem);
    void  (*Palette_SetActiveIndex)(LevelBuilderPalette* p, int index);
    int   (*Palette_GetActiveIndex)(LevelBuilderPalette* p);

    // ----- Placement & spawn helpers -----
    LevelBuilderPlacementResult (*Place)(const LevelBuilderPlacementRequest* request);

    // ----- Preview ghost -----
    void (*SetPreviewAsset)(const char* assetName);
    void (*SetPreviewTransform)(const LBVec3* position, const LBQuat* rotation, const LBVec3* scale);
    void (*SetPreviewState)(int previewState);  // LevelBuilderPreviewState
    void (*HidePreview)();
    int  (*GetPreviewState)();
    void (*GetPreviewTransform)(LBVec3* outPos, LBQuat* outRot, LBVec3* outScale);
    const char* (*GetPreviewAssetName)();

    // ----- UI -----
    void (*OpenLevelBuilderWindow)();
    void (*CloseLevelBuilderWindow)();
    int  (*IsLevelBuilderWindowOpen)();

    // ----- Extension tab registration (modular adds its own tab) -----
    typedef void (*ExtensionTabDrawFn)(void* userData);
    void (*RegisterExtensionTab)(const char* tabName, ExtensionTabDrawFn drawFn, void* userData);
    void (*UnregisterExtensionTab)(const char* tabName);

    // ----- Logging passthrough (so siblings can log via core's engine API) -----
    void (*LogDebug)(const char* msg);
    void (*LogWarning)(const char* msg);
    void (*LogError)(const char* msg);

    // ----- Engine API access (for siblings that need raw engine) -----
    // Returns a PolyphaseEngineAPI* held by core. Lifetime tied to core's
    // OnLoad → OnUnload. Siblings MUST NOT cache past their own OnUnload.
    void* (*GetEngineAPI)();

    // ----- v2: viewport hover + click dispatch ---------------------------
    //
    // The core driver polls the engine's viewport raycast every frame from
    // inside its viewport-overlay callback and stashes the result in the
    // LevelBuilderContext. Siblings read it back via Viewport_GetHoverHit
    // and subscribe to per-tool click events via RegisterToolViewportInput.

    // Returns 1 if a valid hover hit exists this frame, 0 otherwise.
    // outNormal is (0,1,0) for ground-plane fallback hits.
    int  (*Viewport_GetHoverHit)(LBVec3* outPos, LBVec3* outNormal, void** outNode);

    // Click subscription, keyed by tool name. Core only fires `cb` when
    // (a) `toolName` matches the active tool AND (b) the user released the
    // left mouse button inside the viewport AND (c) the click wasn't a drag.
    // Re-registering the same toolName replaces the previous entry — this
    // is what makes sibling hot-reload safe. Pass `cb=null` (or call the
    // unregister variant) to clear the subscription.
    typedef void (*LBViewportInputFn)(const LBVec3* hitPos,
                                      const LBVec3* hitNormal,
                                      void*         hitNode,
                                      int           button,   // 0=L, 1=R, 2=M
                                      void*         userData);
    void (*RegisterToolViewportInput)(const char* toolName,
                                      LBViewportInputFn cb,
                                      void* userData);
    void (*UnregisterToolViewportInput)(const char* toolName);

    // ===== v3 — core-owned kit registry ===================================
    //
    // Core now owns kit data, JSON parsing, and project-root resolution.
    // Siblings (modular, grid, future voxel/spline addons) read kits and
    // pieces by index; the indices are stable within a single load and
    // invalidate on Kit_Reload / Kit_Save / any mutation that adds or
    // removes pieces.

    // --- Enumeration ---------------------------------------------------
    int         (*Kit_GetCount)();
    const char* (*Kit_GetNameAt)(int kitIdx);
    int         (*Kit_FindIndex)(const char* name);
    int         (*Kit_GetInfo)(int kitIdx, LBKitInfo* out);  // 1 on success

    int         (*Kit_GetPieceInfo)(int kitIdx, int pieceIdx, LBPieceInfo* out);
    int         (*Kit_FindPieceByAsset)(int kitIdx, const char* assetName);

    int         (*Kit_GetSocketInfo)(int kitIdx, int pieceIdx, int socketIdx, LBSocketInfo* out);
    const char* (*Kit_GetSocketCompatibleType)(int kitIdx, int pieceIdx, int socketIdx, int compatIdx);

    // --- Active selection (a shared UX state across sibling tabs) ------
    const char* (*Kit_GetActiveName)();
    void        (*Kit_SetActiveByName)(const char* name);
    int         (*Kit_GetActiveIndex)();

    // --- Per-piece mutation (v3 minimum, expanded in v4) ---------------
    void        (*Kit_SetPieceSize)(int kitIdx, int pieceIdx, float x, float y, float z);

    // --- I/O -----------------------------------------------------------
    // Save the kit at kitIdx back to its sourceFile. Returns 0 on success;
    // non-zero on failure (errBuf gets a human-readable message).
    int         (*Kit_Save)(int kitIdx, char* outErrBuf, int errBufSize);

    // Re-scan <project>/Kits/. Discards in-memory registry then rebuilds
    // from disk. Returns the kit count after reload. Errors land in the
    // per-call last-reload list (see Kit_GetLastReload*).
    int         (*Kit_Reload)();
    int         (*Kit_GetLastReloadKitCount)();
    int         (*Kit_GetLastReloadErrorCount)();
    const char* (*Kit_GetLastReloadError)(int errIdx);

    // --- Project root resolver (was per-sibling in v2) -----------------
    const char* (*GetProjectRoot)();         // "" if unresolved
    const char* (*GetProjectKitsFolder)();   // "" if unresolved
    void        (*SetProjectRootOverride)(const char* path);
    void        (*ClearProjectRootCache)();

    // --- Utility math (shared by every kit-consuming sibling) ----------
    int         (*Kit_ComposeSocketWorld)(int kitIdx, int pieceIdx, int socketIdx,
                                          const LBVec3* pieceWorldPos,
                                          const LBQuat* pieceWorldRot,
                                          LBVec3* outSocketWorldPos,
                                          LBQuat* outSocketWorldRot);

    int         (*Kit_SocketsAreCompatible)(int kitIdx,
                                            int pieceIdxA, int socketIdxA,
                                            int pieceIdxB, int socketIdxB);

    void        (*Kit_EulerDegToQuat)(float pitchDeg, float yawDeg, float rollDeg,
                                      LBQuat* outQuat);

    // ===== v4 — brush spawn-fn hooks (T1 of tool.core) ====================
    //
    // The tool.core addon ships generic brushes (Line, Box, BoxFill, …) that
    // don't know how to spawn anything by themselves — every sibling addon
    // (modular, grid, voxel, …) owns its own spawn semantics (StaticMesh vs
    // Scene auto-detect, snap behaviour, naming, placed-piece bookkeeping).
    //
    // Each sibling registers ONE spawn function keyed by its tool name. A
    // generic brush, when committing a placement, calls
    // GetSpawnFnForActiveTool() — which returns the spawn fn registered
    // against whatever tool is currently active — and invokes it once per
    // shape point.
    //
    // `assetName` semantics:
    //   - null / empty → "use the active palette item" (most common; lets
    //     the user keep the same palette selection while clicking a shape).
    //   - non-null     → "spawn this specific asset" (used by ReplaceAssets,
    //     stamp brushes, NoiseFill picking from a weighted set, …).
    //
    // Returns the spawned engine Node* (opaque), or null on failure.
    typedef void* (*LBBrushSpawnFn)(const char*   assetName,
                                    const LBVec3* position,
                                    const LBQuat* rotation,
                                    void*         userData);

    void          (*RegisterSpawnFn)(const char* toolName, LBBrushSpawnFn fn, void* userData);
    void          (*UnregisterSpawnFn)(const char* toolName);
    LBBrushSpawnFn (*GetSpawnFnForActiveTool)(void** outUserData);

    // ===== v5 — socket mutation surface ====================================
    int  (*Kit_AddSocket)(int kitIdx, int pieceIdx);
    int  (*Kit_RemoveSocket)(int kitIdx, int pieceIdx, int socketIdx);
    void (*Kit_SetSocketName)    (int kitIdx, int pieceIdx, int socketIdx, const char* name);
    void (*Kit_SetSocketType)    (int kitIdx, int pieceIdx, int socketIdx, const char* type);
    void (*Kit_SetSocketPosition)(int kitIdx, int pieceIdx, int socketIdx, float x, float y, float z);
    void (*Kit_SetSocketRotation)(int kitIdx, int pieceIdx, int socketIdx,
                                  float pitchDeg, float yawDeg, float rollDeg);
    void (*Kit_SetSocketCompatibleTypes)(int kitIdx, int pieceIdx, int socketIdx,
                                         const char* const* types, int count);

    // ===== v6 — kit sharing metadata (Phase S1) ============================
    //
    // Read + mutate the metadata block in `kit.json` documented in
    // KitSharing.md §3. String pointers from snapshots are valid until
    // the next Kit_Reload, Kit_Save, or any Kit_SetMetaString call.

    // Snapshot getter. Returns 1 on success, 0 if kitIdx is out of range.
    int         (*Kit_GetMetaInfo)(int kitIdx, LBKitMetaInfo* out);

    // Tag array enumeration. tagIdx in [0, tagCount).
    int         (*Kit_GetTagCount)(int kitIdx);
    const char* (*Kit_GetTagAt)(int kitIdx, int tagIdx);
    void        (*Kit_ClearTags)(int kitIdx);
    void        (*Kit_AddTag)(int kitIdx, const char* tag);

    // Dependency array enumeration (read-only at v6 — edit via JSON for now).
    int (*Kit_GetDependencyInfo)(int kitIdx, int depIdx, LBKitDependencyInfo* out);

    // Mutate a single metadata string field. `field` is LBKitMetaField.
    // Passing nullptr or "" clears the field. Out-of-range field is a no-op.
    void        (*Kit_SetMetaString)(int kitIdx, int field, const char* value);

    // The kit's preview image, resolved to an absolute path on disk.
    // Returns "" when the kit has no previewImage or no sourceFile.
    // Useful for the import-confirmation dialog (S2).
    const char* (*Kit_GetPreviewImageAbsPath)(int kitIdx);

    // ===== v6 — kit sharing I/O (Phase S2) =================================
    //
    // Pack/unpack the `.kit` archive format documented in
    // KitSharing.md §3 / KitAuthoringUI.md §4. The format is a
    // store-only zip with `kit.json` at the root plus optional preview /
    // thumbnail files at the same relative paths the kit references.

    // Pack the kit at kitIdx into a fresh .kit file at dstPath. Returns
    // 0 on success; non-zero on failure (errBuf gets a human message).
    int (*Kit_ExportToKitFile)(int kitIdx, const char* dstPath,
                               char* outErrBuf, int errBufSize);

    // Peek a .kit archive without extracting it. Fills the LBKitMetaInfo
    // snapshot for display in the import-confirmation dialog. `outName`
    // gets the kit's display name (kitName field). Returns 0 on success.
    int (*Kit_PreviewKitFile)(const char* srcPath,
                              LBKitMetaInfo* outMeta,
                              char* outName, int outNameSize,
                              char* outErrBuf, int errBufSize);

    // Extract a .kit into the project's Kits/ folder. `conflictMode`
    // maps to LBKitShare::ConflictMode (0=Replace, 1=RenameNew, 2=Abort).
    // On success outDstJsonPath receives the path the new kit.json
    // landed at. Caller is expected to call Kit_Reload() afterwards.
    int (*Kit_ImportKitFile)(const char* srcPath, int conflictMode,
                             char* outDstJsonPath, int outDstSize,
                             char* outErrBuf, int errBufSize);

    // ===== v7 — Paint brush drag input + placed-piece enumeration =========
    //
    // Drag-input poll: returns 1 while the named mouse button is held
    // and the cursor is inside the viewport. Driven by core's
    // DrawViewportPreview from the engine's v5 Viewport_GetMouseState.
    // Used by the Paint brush (LBToolPaint) every frame in its
    // TickEditor to decide whether to stamp / erase.
    int (*Viewport_IsLmbDown)();
    int (*Viewport_IsRmbDown)();

    // Sibling-provided enumeration of placed pieces within a horizontal
    // disc on the XZ plane. Used by:
    //   - Paint brush Erase mode (find pieces to destroy)
    //   - ReplaceAssetsWith bulk replace (T4 follow-up)
    //
    // Modular implements this by walking ModularPlacedRegistry; grid
    // implements it via its own placement bookkeeping. The brush invokes
    // GetEnumerateFnForActiveTool() to find the right callback for the
    // sibling that owns the active tool.
    //
    // The sibling-side fn calls `visit(node, visitUserData)` once per
    // matching node. `visit` returns:
    //   0  → keep the node as-is (just inspecting).
    //   1  → CONSUME — the sibling destroys the node AND removes it from
    //        its own placed-piece registry. Paint Erase returns 1; bulk
    //        find/replace returns 1 after re-spawning the replacement.
    //
    // The sibling-side enumerate fn owns the destruction so it can also
    // clean its own registry in the same transaction — that prevents
    // the use-after-free that bit us in v7 first-cut (Paint dragging
    // over already-erased nodes re-destroyed dangling pointers).
    // v12: assetName is the sibling's stored asset name for the piece
    // (== request.assetName when it was placed). Lifetime is the sibling
    // registry entry's — valid for the duration of the visit call.
    // Visitors that don't need it can just ignore the parameter.
    typedef int (*LBVisitFn)(void* node, const char* assetName,
                             void* visitUserData);
    typedef void (*LBEnumeratePlacementsFn)(
        const LBVec3* center,
        float         radius,
        const char*   sourceAssetFilter,  // v11: null/empty = no filter
        LBVisitFn     visit,
        void*         visitUserData,
        void*         siblingUserData);

    void (*RegisterEnumerateFn)(const char* toolName,
                                LBEnumeratePlacementsFn fn,
                                void* userData);
    void (*UnregisterEnumerateFn)(const char* toolName);
    LBEnumeratePlacementsFn (*GetEnumerateFnForActiveTool)(void** outUserData);

    // v8 / History_* entries were removed in v9. Undo / redo now lives in
    // the engine's ActionManager — siblings call
    // PolyphaseEngineAPI::EditorAction_Push / _BeginGroup / _EndGroup
    // directly. The level-builder defines a uniform LBPiecePresenceRecord
    // userData shape in each sibling (modular, grid, …) and does its own
    // delete-cleanup via the free callback.

    // ===== v10 — R4 piece-level mutation surface ==========================
    //
    // Completes the core-kits R4 refactor: kit-level create / rename /
    // delete, plus per-piece add / remove and string-field setters.
    // Socket mutation is v5; kit-metadata setters are v6; this is the
    // final mutation hole.
    //
    // Semantics:
    //   - All mutators edit core's in-memory LBKit. Kit_Save flushes the
    //     whole kit to its sourceFile in one JSON write — that's the
    //     "transaction" boundary the R4 plan asked for; no per-edit
    //     auto-save and no transaction stack.
    //   - Kit_AddPiece / Kit_RemovePiece invalidate piece indices on
    //     that kit. Sibling UIs that cached pieceIdx must re-resolve
    //     (e.g. via Kit_FindPieceByAsset) on the next frame.
    //   - Kit_CreateKit names the new kit's sourceFile to
    //     <project>/Kits/<sanitized-name>.json. Kit_Save persists it
    //     there; the file isn't written by Kit_CreateKit itself.
    //   - Kit_SetKitName renames the kit in the registry's by-name map
    //     and updates the active-kit pointer if it pointed at the old
    //     name. It does NOT rename the sourceFile on disk — call
    //     Kit_Save first if you want the rename reflected on disk
    //     (Save writes to the same sourceFile path; if the rename also
    //     needs the file moved, the UI does that out-of-band today).
    //   - Kit_DeleteKit removes from the registry only. The sourceFile
    //     stays on disk unless the caller deletes it separately.

    // Kit-level mutation.
    int  (*Kit_CreateKit)(const char* name);   // returns new kitIdx, -1 on failure
    int  (*Kit_DeleteKit)(int kitIdx);          // returns 1 on success
    int  (*Kit_SetKitName)(int kitIdx, const char* newName);  // 1 on success

    // Piece-level add / remove.
    int  (*Kit_AddPiece)(int kitIdx, const char* name, const char* assetName);   // returns new pieceIdx
    int  (*Kit_RemovePiece)(int kitIdx, int pieceIdx);                            // 1 on success

    // Per-piece string setters. NULL clears the field. Out-of-range index
    // is a no-op. Strings are copied into core's storage; caller may free
    // its buffer immediately after the call.
    void (*Kit_SetPieceName)    (int kitIdx, int pieceIdx, const char* name);
    void (*Kit_SetPieceAsset)   (int kitIdx, int pieceIdx, const char* assetName);
    void (*Kit_SetPieceCategory)(int kitIdx, int pieceIdx, const char* category);
    void (*Kit_SetPieceIconPath)(int kitIdx, int pieceIdx, const char* iconPath);

    // ===== v13 — Rebuild-from-world hooks =====
    // Sibling registers a callback that walks the active scene and re-
    // registers placed pieces into its own placed-piece registry. Core
    // fires every registered callback automatically when the user
    // activates Level Builder mode, so Paint Erase / Replace see pre-
    // existing placements without the user having to click the
    // "Rebuild From World" button in the Debug accordion.
    typedef void (*LBRebuildFromWorldFn)(void* userData);
    void (*RegisterRebuildFromWorldFn)(const char* toolName,
                                       LBRebuildFromWorldFn fn,
                                       void* userData);
    void (*UnregisterRebuildFromWorldFn)(const char* toolName);
};

// ===== Entry point =====
//
// Exported from the core addon DLL. Sibling addons fetch this via
// GetProcAddress("LevelBuilderCore_GetAPI"). The returned pointer is
// owned by the core addon and valid until the core unloads.

#ifdef __cplusplus
extern "C" {
#endif

LEVELBUILDER_CORE_API LevelBuilderCoreAPI* LevelBuilderCore_GetAPI();

// Function-pointer type used by sibling addons that resolve the entry
// point via GetProcAddress / dlsym instead of linking against core.
typedef LevelBuilderCoreAPI* (*LevelBuilderCore_GetAPIFn)();

#ifdef __cplusplus
}
#endif
