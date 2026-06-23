/**
 * @file ComPolyphaseEditorLevelbuilderToolCore.cpp
 * @brief Entry point for com.polyphase.editor.levelbuilder.tool.core.
 *
 * tool.core ships engine-agnostic brushes (Line, Box, BoxFill, NoiseFill,
 * Replace, Paint, MaskFill) on top of the spawn-fn hook surface added to
 * the core API in v4. Each brush calls
 * api->GetSpawnFnForActiveTool() to find the active sibling's spawn
 * function, then invokes it once per shape point — so tool.core never
 * needs to know what a kit is or how a particular sibling routes
 * StaticMesh vs Scene spawning.
 *
 * On load:
 *   - Resolve the core addon's API (late-bound via GetProcAddress).
 *   - Bootstrap the engine's ImGui context so brush settings UIs work.
 *   - Initialize each shipped brush (Phase T1 ships Line only).
 *
 * On unload (hot-reload safe):
 *   - Shutdown each brush (which unregisters via the core API).
 *   - Drop the cached core API pointer so the next load re-resolves it.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/ImGuiPluginContext.h"
#include "imgui.h"
#endif

#include "LevelBuilderCoreLoader.h"
#include "LBToolLine.h"
#include "LBToolBox.h"
#include "LBToolBoxFill.h"
#include "LBToolNoiseFill.h"
#include "LBToolPaint.h"

static PolyphaseEngineAPI* sEngineAPI = nullptr;
#if EDITOR
static EditorUIHooks*      sHooks     = nullptr;
static uint64_t            sHookId    = 0;
#endif

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;

#if EDITOR
    if (api && api->GetImGuiContext)
    {
        ImGuiPluginContext ctx{};
        api->GetImGuiContext(&ctx);
        if (ctx.context)
        {
            ImGui::SetCurrentContext(ctx.context);
            ImGui::SetAllocatorFunctions(ctx.allocFunc, ctx.freeFunc, ctx.allocUserData);
        }
    }
#endif

    LevelBuilderCoreAPI* core = LevelBuilderCoreLoader::Get();
    if (!core)
    {
        if (api && api->LogWarning)
            api->LogWarning("[LevelBuilderToolCore] core addon not loaded yet — "
                            "ensure com.polyphase.editor.levelbuilder.core is loaded first");
        return 0;  // still return success — user can reload after core
    }

    LBToolLine::Initialize(core);
    LBToolBox::Initialize(core);
    LBToolBoxFill::Initialize(core);
    LBToolNoiseFill::Initialize(core);
    LBToolPaint::Initialize(core);

    if (api && api->LogDebug)
        api->LogDebug("[LevelBuilderToolCore] loaded — Line / Box / BoxFill / NoiseFill / Paint brushes registered");

    return 0;
}

static void OnUnload()
{
    LevelBuilderCoreAPI* core = LevelBuilderCoreLoader::Get();
    if (core)
    {
        // Unregister in reverse-registration order so any in-flight
        // dispatch resolves to a fully-still-valid brush instance.
        LBToolPaint::Shutdown(core);
        LBToolNoiseFill::Shutdown(core);
        LBToolBoxFill::Shutdown(core);
        LBToolBox::Shutdown(core);
        LBToolLine::Shutdown(core);
    }

    LevelBuilderCoreLoader::Reset();

    if (sEngineAPI && sEngineAPI->LogDebug)
        sEngineAPI->LogDebug("[LevelBuilderToolCore] unloaded");

    sEngineAPI = nullptr;
#if EDITOR
    sHooks  = nullptr;
    sHookId = 0;
#endif
}

static void RegisterTypes(void* /*nodeFactory*/) {}

static void RegisterScriptFuncs(struct lua_State* /*L*/) {}

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks  = hooks;
    sHookId = hookId;

    // Viewport overlay for the Line brush — draws the start-point sphere,
    // the live line to the hovered hit, and stride markers along it. The
    // overlay itself no-ops unless Line is the active brush AND has a
    // start point, so it costs nothing for users who never touch Line.
    //
    // Brush settings still render through core's Brush tab via
    // LevelBuilderBrush::DrawSettingsUI (v4).
    if (hooks && hooks->RegisterViewportOverlay)
    {
        hooks->RegisterViewportOverlay(hookId,
                                       "level_builder_line_brush",
                                       &LBToolLine_DrawViewportOverlayTrampoline,
                                       nullptr);
        hooks->RegisterViewportOverlay(hookId,
                                       "level_builder_box_brush",
                                       &LBToolBox_DrawViewportOverlayTrampoline,
                                       nullptr);
        hooks->RegisterViewportOverlay(hookId,
                                       "level_builder_box_fill_brush",
                                       &LBToolBoxFill_DrawViewportOverlayTrampoline,
                                       nullptr);
        hooks->RegisterViewportOverlay(hookId,
                                       "level_builder_noise_fill_brush",
                                       &LBToolNoiseFill_DrawViewportOverlayTrampoline,
                                       nullptr);
        hooks->RegisterViewportOverlay(hookId,
                                       "level_builder_paint_brush",
                                       &LBToolPaint_DrawViewportOverlayTrampoline,
                                       nullptr);
    }
}
#endif

extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion       = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName       = "com.polyphase.editor.levelbuilder.tool.core";
    desc->pluginVersion    = "0.1.0";
    desc->OnLoad           = OnLoad;
    desc->OnUnload         = OnUnload;
    desc->Tick             = nullptr;
    desc->TickEditor       = nullptr;  // paint brushes (T6) will wire this
    desc->RegisterTypes    = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI = RegisterEditorUI;
#else
    desc->RegisterEditorUI = nullptr;
#endif
    desc->OnEditorPreInit  = nullptr;
    desc->OnEditorReady    = nullptr;
    return 0;
}
