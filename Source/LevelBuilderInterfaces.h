/**
 * @file LevelBuilderInterfaces.h
 * @brief Abstract base classes implemented by sibling addons.
 *
 * These are pure-virtual interfaces with NO data members so the vtable
 * layout is stable across DLL boundaries. Sibling addons (modular, grid,
 * voxel, ...) derive from these and hand instances to core via the C ABI
 * in LevelBuilderCoreAPI.h.
 */

#pragma once

#include "LevelBuilderCoreAPI.h"

class LevelBuilderContext;

// ============================================================================
// LevelBuilderTool
//
// A top-level "mode" the user can activate (Modular Placement, Grid Paint,
// Spline Path, etc.). Tools are responsible for tying together a brush,
// a snap provider, and viewport input handling.
// ============================================================================

class LevelBuilderTool
{
public:
    virtual ~LevelBuilderTool() = default;

    virtual const char* GetName() const = 0;

    // Lifecycle — called by core when the tool becomes/stops being active.
    virtual void Activate(LevelBuilderContext* /*context*/) {}
    virtual void Deactivate() {}

    // Editor tick (always, regardless of PIE).
    virtual void TickEditor(float /*deltaTime*/) {}

    // Settings UI rendered inside the Level Builder window's Tool tab.
    // Caller has already pushed an ImGui context — just call ImGui::XXX.
    virtual void DrawSettingsUI() {}

    // Optional viewport input handler. Return true to consume the event.
    virtual bool HandleViewportInput() { return false; }
};

// ============================================================================
// LevelBuilderBrush
//
// A brush is the "how" of placement: simple single-piece, line, rectangle,
// fill, replace, etc. A tool may switch between several brushes.
// ============================================================================

class LevelBuilderBrush
{
public:
    virtual ~LevelBuilderBrush() = default;

    virtual const char* GetName() const = 0;

    virtual bool CanPlace(const LevelBuilderPlacementRequest& request) = 0;
    virtual LevelBuilderPlacementResult Place(const LevelBuilderPlacementRequest& request) = 0;

    // Drawn into the Brush tab when this brush is the active brush.
    // Default no-op so existing brushes (Modular Single, Grid Single)
    // don't need changes. The caller has already pushed an ImGui context.
    virtual void DrawSettingsUI() {}

    // Optional: name of the tool this brush "belongs to" (e.g. modular's
    // single-piece brush returns "Modular Placement"). When non-null, the
    // Brush tab auto-switches the active tool to match whenever the user
    // picks this brush. Tool-agnostic brushes (Line / Box / etc. from
    // tool.core) leave this as nullptr — they operate against whichever
    // tool the user explicitly selected.
    virtual const char* GetOwnerTool() const { return nullptr; }

    // Called every editor frame while THIS brush is the active brush.
    // Default no-op. Drag-input brushes (Paint) override to poll the
    // viewport's LMB/RMB state and stamp themselves on each frame the
    // button is held. Fired from core's DrawViewportPreview so it ticks
    // regardless of which Level Builder tab is open.
    virtual void TickEditor(float /*deltaTime*/) {}

    // Default true — core's click dispatcher gates click delivery on
    // "an active palette item is armed" so a brand-new project doesn't
    // spawn random pieces from stray clicks. Brushes that operate on
    // already-placed pieces (Replace, future MoveSelected, etc.)
    // override to return false: their click is meaningful regardless
    // of palette state.
    virtual bool NeedsArmedPreview() const { return true; }
};

// ============================================================================
// LevelBuilderSnapProvider
//
// Translates a raw viewport-derived position into a snapped position and
// rotation. The "None" provider passes through; grid/socket providers add
// their own logic.
// ============================================================================

class LevelBuilderSnapProvider
{
public:
    virtual ~LevelBuilderSnapProvider() = default;

    virtual const char* GetName() const = 0;

    /**
     * @brief Snap a raw position to the provider's grid/sockets.
     * @return true if the position was snapped (outputs valid),
     *         false if no snap target was found (caller should fall back).
     */
    virtual bool GetSnapTransform(
        const LBVec3& rawPosition,
        LBVec3&       outPosition,
        LBQuat&       outRotation
    ) = 0;
};
