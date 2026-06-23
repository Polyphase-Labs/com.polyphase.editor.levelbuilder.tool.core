#include "LBToolReplace.h"

#include "LBToolDistribution.h"
#include "LBToolShared.h"
#include "LevelBuilderCoreAPI.h"
#include "LevelBuilderCoreLoader.h"

#if EDITOR
#include "imgui.h"
#include "Plugins/PolyphaseEngineAPI.h"
#include "Engine/Nodes/3D/Node3d.h"
#include "LBToolPicker.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    LBToolReplace sInstance;

    // ---- Per-brush settings ----
    float                     sRadius      = 2.0f;          // meters — big enough to feel "area" by default
    std::string               sSourceAsset = "";            // empty = match any
    std::vector<std::string>  sTargetAssets;                // multi-pick; random per swap
    uint32_t                  sSeed        = 0x12345u;

    // ---- Modal open-state latches ----
    bool sOpenSourcePicker  = false;
    bool sOpenTargetsPicker = false;

    // Set true when the user requests a modal open; consumed by the
    // modal's first frame so it knows to seed its working set from the
    // caller's current value (rather than overwriting on every frame).
    bool sJustOpened = false;

    // Eyedropper: when true, the next viewport click captures the
    // clicked piece's asset name as sSourceAsset and does NOT perform
    // a replace. One-shot — clears itself after capture (or after Esc).
    bool sEyedropperActive = false;

    LevelBuilderCoreAPI* CoreAPI() { return LevelBuilderCoreLoader::Get(); }

    // Pitch-Yaw-Roll (degrees) → quat. Inverse of modular's quat→euler.
    LBQuat EulerToQuat(float pitchDeg, float yawDeg, float rollDeg)
    {
        const float DEG2RAD = 3.14159265358979323846f / 180.0f;
        const float px = pitchDeg * DEG2RAD * 0.5f;
        const float py = yawDeg   * DEG2RAD * 0.5f;
        const float pz = rollDeg  * DEG2RAD * 0.5f;
        const float cx = std::cos(px), sx = std::sin(px);
        const float cy = std::cos(py), sy = std::sin(py);
        const float cz = std::cos(pz), sz = std::sin(pz);
        LBQuat q;
        q.x = sx*cy*cz - cx*sy*sz;
        q.y = cx*sy*cz + sx*cy*sz;
        q.z = cx*cy*sz - sx*sy*cz;
        q.w = cx*cy*cz + sx*sy*sz;
        return q;
    }

    // ---- Replace visitor + context ----
    struct ReplaceTarget
    {
        LBVec3 pos{0,0,0};
        LBQuat rot{0,0,0,1};
    };
    struct ReplaceCtx
    {
        PolyphaseEngineAPI*         eng = nullptr;
        std::vector<ReplaceTarget>  targets;
    };

    int ReplaceVisit(void* node, const char* /*assetName*/, void* userData)
    {
        if (!node) return 0;
        ReplaceCtx* c = (ReplaceCtx*)userData;
        if (!c || !c->eng) return 0;

        Node3D* n3 = (Node3D*)node;
        ReplaceTarget t;
        float px = 0, py = 0, pz = 0;
        if (c->eng->Node3D_GetPosition)
        {
            c->eng->Node3D_GetPosition(n3, &px, &py, &pz);
            t.pos = LBVec3{px, py, pz};
        }
        float pitch = 0, yaw = 0, roll = 0;
        if (c->eng->Node3D_GetRotation)
        {
            c->eng->Node3D_GetRotation(n3, &pitch, &yaw, &roll);
            t.rot = EulerToQuat(pitch, yaw, roll);
        }
        c->targets.push_back(t);
        return 1;
    }

    // ---- Eyedropper visitor ----
    // Stops at the FIRST hit, returns 0 so nothing gets consumed.
    // userData = std::string* to receive the asset name.
    int EyedropperVisit(void* node, const char* assetName, void* userData)
    {
        if (!node) return 0;
        std::string* out = (std::string*)userData;
        if (!out) return 0;
        if (out->empty() && assetName && *assetName)
            *out = assetName;
        return 0;   // never consume — eyedropper just reads
    }
}

bool LBToolReplace::CanPlace(const LevelBuilderPlacementRequest& request)
{
    // Eyedropper just READS — doesn't need targets or a palette piece
    // to be armed. Without this case CanPlace returns false and the
    // registry bails before Place runs, blocking the eyedropper capture.
    if (sEyedropperActive) return true;
    if (!sTargetAssets.empty()) return true;
    return request.assetName && *request.assetName;
}

LevelBuilderPlacementResult LBToolReplace::Place(const LevelBuilderPlacementRequest& request)
{
    LevelBuilderPlacementResult result{};
    result.success = 0;
    result.spawnedNode = nullptr;
    result.errorMessage = nullptr;

    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api)
    {
        static const char* kErr = "Replace: core API unavailable";
        result.errorMessage = kErr;
        return result;
    }

    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (!eng)
    {
        static const char* kErr = "Replace: engine API unavailable";
        result.errorMessage = kErr;
        return result;
    }

    if (!api->GetEnumerateFnForActiveTool)
    {
        static const char* kErr =
            "Replace: requires core API v7+ (enumerate-fn hooks)";
        result.errorMessage = kErr;
        return result;
    }
    void* enumUd = nullptr;
    LevelBuilderCoreAPI::LBEnumeratePlacementsFn enumFn =
        api->GetEnumerateFnForActiveTool(&enumUd);
    if (!enumFn)
    {
        static const char* kErr =
            "Replace: no enumerate fn registered for the active tool";
        result.errorMessage = kErr;
        return result;
    }

    // ---- Eyedropper short-circuit ----
    // If the user armed "Pick from scene", this click captures the
    // asset name of the first placed piece in radius and bails — no
    // destroy, no spawn, no undo entry.
    if (sEyedropperActive)
    {
        std::string picked;
        // No filter — eyedropper grabs whatever the user pointed at.
        // Use the configured radius (small by default = single-piece).
        const float pickRadius = (sRadius > 0.01f) ? sRadius : 0.10f;
        enumFn(&request.position, pickRadius, /*filter=*/nullptr,
               &EyedropperVisit, &picked, enumUd);
        sEyedropperActive = false;
        if (!picked.empty())
        {
            sSourceAsset = picked;
            static const char* kMsg = "Replace: source captured from scene";
            result.errorMessage = kMsg;
        }
        else
        {
            static const char* kMsg = "Replace: nothing under cursor to pick";
            result.errorMessage = kMsg;
        }
        return result;   // success=0 since no actual placement happened
    }

    const char* filter = sSourceAsset.empty() ? nullptr : sSourceAsset.c_str();

    ReplaceCtx ctx;
    ctx.eng = eng;
    const float radius = (sRadius > 0.01f) ? sRadius : 0.01f;
    enumFn(&request.position, radius, filter, &ReplaceVisit, &ctx, enumUd);

    if (ctx.targets.empty())
    {
        static const char* kMsg = "Replace: no matching piece under cursor";
        result.errorMessage = kMsg;
        return result;
    }

    void* spawnUd = nullptr;
    LevelBuilderCoreAPI::LBBrushSpawnFn spawn =
        LBToolShared::ResolveSpawn(api, &spawnUd);
    if (!spawn)
    {
        static const char* kErr =
            "Replace: no spawn fn registered for the active tool";
        result.errorMessage = kErr;
        return result;
    }

    LBToolDistribution::Rng rng = LBToolDistribution::SeedRng(sSeed);
    sSeed = (sSeed * 1103515245u + 12345u) | 1u;

    auto pickTarget = [&]() -> const char* {
        if (sTargetAssets.empty()) return request.assetName;
        if (sTargetAssets.size() == 1) return sTargetAssets[0].c_str();
        const float u = LBToolDistribution::NextFloat01(rng);
        int pick = (int)(u * (float)sTargetAssets.size());
        if (pick < 0) pick = 0;
        if (pick >= (int)sTargetAssets.size()) pick = (int)sTargetAssets.size() - 1;
        return sTargetAssets[pick].c_str();
    };

#if EDITOR
    if (eng->EditorAction_BeginGroup) eng->EditorAction_BeginGroup("Replace");
#endif

    int spawned = 0;
    void* lastSpawned = nullptr;
    for (size_t i = 0; i < ctx.targets.size(); ++i)
    {
        const ReplaceTarget& t = ctx.targets[i];
        LBVec3 pos = t.pos;
        LBQuat rot = t.rot;
        const char* asset = pickTarget();
        if (!asset || !*asset) continue;
        void* n = spawn(asset, &pos, &rot, spawnUd);
        if (n) { ++spawned; lastSpawned = n; }
    }

#if EDITOR
    if (eng->EditorAction_EndGroup) eng->EditorAction_EndGroup();
#endif

    result.success     = (spawned > 0) ? 1 : 0;
    result.spawnedNode = lastSpawned;
    return result;
}

#if EDITOR
// PieceChoice + helpers + modal moved to LBToolPicker. Re-using the
// shared symbols here keeps the brushes thin.
using LBToolPicker::PieceChoice;
using LBToolPicker::CollectActiveKitPieces;
using LBToolPicker::FindByAsset;
using LBToolPicker::CachedProjectRoot;
using LBToolPicker::GetActiveKitFolder;
using LBToolPicker::DrawThumbButton;
using LBToolPicker::DrawPiecePickerModal;
#endif

void LBToolReplace::DrawSettingsUI()
{
#if EDITOR
    ImGui::TextUnformatted("Replace — click a placed piece to swap it");
    ImGui::Separator();

    LevelBuilderCoreAPI* api = CoreAPI();
    std::vector<PieceChoice> pieces = CollectActiveKitPieces(api);
    const std::string  projectRoot = CachedProjectRoot();
    const std::string  kitFolder   = GetActiveKitFolder(api);

    // ---- Radius ----
    ImGui::SliderFloat("Radius (m)", &sRadius, 0.01f, 10.0f, "%.2f");
    ImGui::TextDisabled(
        "Larger radius = bulk swap inside a disc. Shrink to ~0.1m for "
        "single-piece precision.");

    ImGui::Spacing();

    // ---- Source (A) ----
    ImGui::TextUnformatted("Source (A) — what to replace");
    {
        const PieceChoice* pc = sSourceAsset.empty()
                              ? nullptr
                              : FindByAsset(pieces, sSourceAsset);
        if (DrawThumbButton(pc, projectRoot, kitFolder, 48.0f, false,
                            "src_btn",
                            sSourceAsset.empty() ? "Any" : "?"))
        {
            sOpenSourcePicker = true;
            sJustOpened       = true;
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s",
                    sSourceAsset.empty() ? "<any piece in radius>"
                    : (pc ? pc->display.c_str() : sSourceAsset.c_str()));
        // Pick-from-scene eyedropper. Toggleable so the user can cancel
        // by hitting it again. While armed, the next viewport click
        // captures the clicked piece's asset and bails (no swap).
        if (sEyedropperActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.00f, 0.78f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.85f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
            if (ImGui::SmallButton("Cancel Pick##src_pick"))
                sEyedropperActive = false;
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f),
                               "Click a piece in the scene…");
        }
        else
        {
            if (ImGui::SmallButton("Pick from scene##src_pick"))
                sEyedropperActive = true;
        }
        if (!sSourceAsset.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##src")) sSourceAsset.clear();
        }
        ImGui::EndGroup();
    }

    ImGui::Spacing();

    // ---- Targets (B) ----
    ImGui::TextUnformatted("Targets (B) — what to spawn in its place");
    if (sTargetAssets.empty())
    {
        ImGui::TextDisabled("(empty → uses active palette piece)");
    }
    else
    {
        for (int i = 0; i < (int)sTargetAssets.size(); ++i)
        {
            ImGui::PushID(i);
            const PieceChoice* pc = FindByAsset(pieces, sTargetAssets[i]);
            // Click thumb → swap THIS slot (re-pick); X → remove.
            if (DrawThumbButton(pc, projectRoot, kitFolder, 48.0f, false,
                                "tgt_btn",
                                "?"))
            {
                // Open targets picker, but mark the working set up to
                // only contain this asset so the user can refine. Easier:
                // open the multi-picker as-is and let user toggle.
                sOpenTargetsPicker = true;
                sJustOpened        = true;
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("%s", pc ? pc->display.c_str()
                                 : sTargetAssets[i].c_str());
            if (ImGui::SmallButton("Remove##tgt"))
            {
                sTargetAssets.erase(sTargetAssets.begin() + i);
                ImGui::EndGroup();
                ImGui::PopID();
                break;
            }
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }

    if (ImGui::Button("+ Add target / Edit set", ImVec2(180, 28)))
    {
        sOpenTargetsPicker = true;
        sJustOpened        = true;
    }
    if (sTargetAssets.size() > 1)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(random pick per replacement)");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Spawns at each target's current world transform.");
    ImGui::TextDisabled("Wrapped in one undo step.");

    // ---- Modals — opened from the latches above ----
    // OpenPopup must run AFTER the buttons that set the latch; placing
    // it here ensures the popup id is on the stack at the right time.
    if (sOpenSourcePicker)
    {
        ImGui::OpenPopup("Pick Source##replace_src_modal");
        sOpenSourcePicker = false;
    }
    if (sOpenTargetsPicker)
    {
        ImGui::OpenPopup("Pick Targets##replace_tgt_modal");
        sOpenTargetsPicker = false;
    }

    std::vector<std::string> srcPicked = { sSourceAsset };
    if (DrawPiecePickerModal("Pick Source##replace_src_modal",
                             "Source — single select",
                             /*multiSelect=*/false,
                             /*allowAny=*/true,
                             sJustOpened,
                             &srcPicked))
    {
        sSourceAsset = srcPicked.empty() ? std::string() : srcPicked[0];
    }
    if (DrawPiecePickerModal("Pick Targets##replace_tgt_modal",
                             "Targets — multi-select (random pick per swap)",
                             /*multiSelect=*/true,
                             /*allowAny=*/false,
                             sJustOpened,
                             &sTargetAssets))
    {
        // sTargetAssets already overwritten by the modal on confirm.
    }
#endif
}

void LBToolReplace::Initialize(LevelBuilderCoreAPI* api)
{
    if (!api) return;
    if (api->RegisterBrush) api->RegisterBrush("Replace", &sInstance);
}

void LBToolReplace::Shutdown(LevelBuilderCoreAPI* api)
{
    if (!api) return;
    if (api->UnregisterBrush) api->UnregisterBrush("Replace");
    sTargetAssets.clear();
}

// Viewport overlay — wire circle at cursor in cyan.
extern "C" void LBToolReplace_DrawViewportOverlayTrampoline(
    float /*vx*/, float /*vy*/, float /*vw*/, float /*vh*/, void* /*ud*/)
{
#if EDITOR
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api || !api->GetActiveBrushName || !api->GetEngineAPI) return;
    const char* active = api->GetActiveBrushName();
    if (!active || std::strcmp(active, "Replace") != 0) return;

    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (!eng || !eng->Gizmos_DrawLine || !eng->Gizmos_SetColor) return;

    LBVec3 hit, hitN; void* hitNode = nullptr;
    if (!api->Viewport_GetHoverHit) return;
    if (!api->Viewport_GetHoverHit(&hit, &hitN, &hitNode)) return;

    // Cyan in normal mode, yellow while the eyedropper is armed so the
    // user can see that the next click WON'T swap.
    if (sEyedropperActive) eng->Gizmos_SetColor(1.0f, 0.85f, 0.20f, 0.95f);
    else                   eng->Gizmos_SetColor(0.20f, 0.85f, 1.0f, 0.9f);

    const int kSegs = 32;
    const float TWO_PI = 6.2831853071795864769f;
    LBVec3 prev{ hit.x + sRadius, hit.y, hit.z };
    for (int i = 1; i <= kSegs; ++i)
    {
        const float t = (float)i / (float)kSegs * TWO_PI;
        LBVec3 next{ hit.x + sRadius * std::cos(t),
                     hit.y,
                     hit.z + sRadius * std::sin(t) };
        eng->Gizmos_DrawLine(prev.x, prev.y, prev.z, next.x, next.y, next.z);
        prev = next;
    }
    if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
#endif
}
