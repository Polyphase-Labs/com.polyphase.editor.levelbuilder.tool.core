#include "LBToolMaskFill.h"

#include "LBToolDistribution.h"
#include "LBToolShared.h"
#include "LevelBuilderCoreAPI.h"
#include "LevelBuilderCoreLoader.h"

#if EDITOR
#include "imgui.h"
#include "Plugins/PolyphaseEngineAPI.h"
#include "LBToolPicker.h"
#include "LBToolMaskImage.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    LBToolMaskFill sInstance;

    // ---- Per-brush state ----
    bool   sHasStart = false;
    LBVec3 sStart{0,0,0};

    // Mask state — shared decoder + sampler in LBToolMaskImage.
#if EDITOR
    LBToolMaskImage::MaskState sMask;
#endif

    // ---- Settings ----
    float    sCellStride = 1.0f;
    float    sThreshold  = 0.50f;
    bool     sInvert     = false;
    float    sJitterPos  = 0.0f;
    float    sJitterYaw  = 0.0f;
    uint32_t sSeed       = 0xC0DEu;

    // Channel modes. Default: Luminance only. Per-channel toggles add
    // R / G / B sampling, each with its own targets list. Multiple
    // channels can fire at the same cell — stamps stack.
    bool sUseLuminance = true;
    bool sUseRed       = false;
    bool sUseGreen     = false;
    bool sUseBlue      = false;

    // Target asset lists per channel (multi-pick, random per cell).
    // Empty → falls back to the active palette piece (request.assetName).
    std::vector<std::string> sLumTargets;
    std::vector<std::string> sRTargets;
    std::vector<std::string> sGTargets;
    std::vector<std::string> sBTargets;

    // Modal open-state latches (per-channel + path-picker convenience).
    bool sOpenLumPicker = false;
    bool sOpenRPicker   = false;
    bool sOpenGPicker   = false;
    bool sOpenBPicker   = false;
    bool sJustOpened    = false;   // shared seed-from-out latch

    constexpr int kMaxCells = 256 * 256;

    LevelBuilderCoreAPI* CoreAPI() { return LevelBuilderCoreLoader::Get(); }

    void RectFromCorners(const LBVec3& a, const LBVec3& b,
                         LBVec3& outMin, LBVec3& outMax, float& outY)
    {
        outMin.x = std::min(a.x, b.x);
        outMin.z = std::min(a.z, b.z);
        outMax.x = std::max(a.x, b.x);
        outMax.z = std::max(a.z, b.z);
        outY     = 0.5f * (a.y + b.y);
        outMin.y = outY;
        outMax.y = outY;
    }

// Mask decode + sample now live in LBToolMaskImage (shared with Paint).
// PickFromList lives in LBToolPicker.h.
}

bool LBToolMaskFill::CanPlace(const LevelBuilderPlacementRequest& request)
{
    // Need either an armed palette piece OR at least one channel that
    // has its own target list configured.
    if (request.assetName && *request.assetName) return true;
    if (!sLumTargets.empty() || !sRTargets.empty()
     || !sGTargets.empty() || !sBTargets.empty()) return true;
    return false;
}

LevelBuilderPlacementResult LBToolMaskFill::Place(const LevelBuilderPlacementRequest& request)
{
    LevelBuilderPlacementResult result{};
    result.success = 0;
    result.spawnedNode = nullptr;
    result.errorMessage = nullptr;

    if (!sHasStart)
    {
        sStart = request.position;
        sHasStart = true;
        static const char* kMsg = "MaskFill: corner set; click again to commit";
        result.errorMessage = kMsg;
        return result;
    }

    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api)
    {
        static const char* kErr = "MaskFill: core API unavailable";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }
    void* userData = nullptr;
    LevelBuilderCoreAPI::LBBrushSpawnFn spawn = LBToolShared::ResolveSpawn(api, &userData);
    if (!spawn)
    {
        static const char* kErr = "MaskFill: no spawn fn registered for the active tool";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }

#if EDITOR
    if (!LBToolMaskImage::EnsureLoaded(sMask))
    {
        static const char* kErr = "MaskFill: no mask loaded (set Mask Path)";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }
#else
    static const char* kErr = "MaskFill: runtime-only build (no mask decoder)";
    result.errorMessage = kErr;
    sHasStart = false;
    return result;
#endif

    LBVec3 rmin, rmax;
    float  y = 0.0f;
    RectFromCorners(sStart, request.position, rmin, rmax, y);

    const float stride = (sCellStride > 0.05f) ? sCellStride : 0.05f;
    const float threshU = std::clamp(sThreshold, 0.0f, 1.0f);
    const int   thresh255 = (int)(threshU * 255.0f);
    const int   nx = (int)std::max(1.0f, std::ceil((rmax.x - rmin.x) / stride));
    const int   nz = (int)std::max(1.0f, std::ceil((rmax.z - rmin.z) / stride));
    const int   cells = nx * nz;
    if (cells > kMaxCells)
    {
        static const char* kErr =
            "MaskFill: rectangle too large for current stride (cap hit)";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }

    LBToolDistribution::Rng rng = LBToolDistribution::SeedRng(sSeed);
    int spawned = 0;
    void* lastNode = nullptr;
    const char* fallbackAsset = request.assetName;

#if EDITOR
    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (eng && eng->EditorAction_BeginGroup) eng->EditorAction_BeginGroup("MaskFill");
#endif

    auto trySpawn = [&](float cx, float cz, const char* asset)
    {
        if (!asset || !*asset) return;
        const float jx = sJitterPos * stride * 0.5f *
                         LBToolDistribution::NextFloatRange(rng, -1.0f, 1.0f);
        const float jz = sJitterPos * stride * 0.5f *
                         LBToolDistribution::NextFloatRange(rng, -1.0f, 1.0f);
        LBVec3 pos{ cx + jx, y, cz + jz };
        LBQuat rot = LBToolDistribution::JitteredYaw(LBQuat{0,0,0,1}, sJitterYaw, rng);
        void* node = spawn(asset, &pos, &rot, userData);
        if (node) { lastNode = node; ++spawned; }
    };

    auto channelHit = [&](uint8_t v) -> bool {
        return sInvert ? (v < thresh255) : (v >= thresh255);
    };

    for (int ix = 0; ix < nx; ++ix)
    {
        for (int iz = 0; iz < nz; ++iz)
        {
            const float cx = rmin.x + (ix + 0.5f) * stride;
            const float cz = rmin.z + (iz + 0.5f) * stride;
            const float u = (nx > 1) ? ((float)ix / (float)(nx - 1)) : 0.5f;
            const float v = (nz > 1) ? ((float)iz / (float)(nz - 1)) : 0.5f;

#if EDITOR
            const LBToolMaskImage::Pixel p = LBToolMaskImage::Sample(sMask, u, v);
#else
            const LBToolMaskImage::Pixel p{0,0,0,0};
#endif

            // Per-channel sampling. Multiple channels can stack at one
            // cell — useful for "this cell has a tree (G) and a rock (B)."
            if (sUseLuminance)
            {
                const uint8_t lum = (uint8_t)(((int)p.r + (int)p.g + (int)p.b) / 3);
                if (channelHit(lum))
                    trySpawn(cx, cz, LBToolPicker::PickFromList(sLumTargets, fallbackAsset, rng));
            }
            if (sUseRed   && channelHit(p.r))
                trySpawn(cx, cz, LBToolPicker::PickFromList(sRTargets, fallbackAsset, rng));
            if (sUseGreen && channelHit(p.g))
                trySpawn(cx, cz, LBToolPicker::PickFromList(sGTargets, fallbackAsset, rng));
            if (sUseBlue  && channelHit(p.b))
                trySpawn(cx, cz, LBToolPicker::PickFromList(sBTargets, fallbackAsset, rng));
        }
    }

#if EDITOR
    if (eng && eng->EditorAction_EndGroup) eng->EditorAction_EndGroup();
#endif

    sHasStart = false;
    result.success = (spawned > 0) ? 1 : 0;
    result.spawnedNode = lastNode;
    return result;
}

#if EDITOR
namespace
{
    // One-row UI for a channel's enable toggle + Targets thumbnail
    // strip + Edit button. Returns true if the user requested to open
    // this channel's picker modal.
    bool DrawChannelRow(const char* label,
                       const ImVec4& tintColor,
                       bool& enableFlag,
                       std::vector<std::string>& targets,
                       const std::vector<LBToolPicker::PieceChoice>& pieces,
                       const std::string& projectRoot,
                       const std::string& kitFolder,
                       const char* idSuffix)
    {
        bool openRequested = false;

        ImGui::PushID(idSuffix);
        ImGui::PushStyleColor(ImGuiCol_Text, tintColor);
        ImGui::Checkbox(label, &enableFlag);
        ImGui::PopStyleColor();

        if (enableFlag)
        {
            ImGui::Indent();
            // Thumbnail strip of currently-selected targets.
            if (targets.empty())
            {
                ImGui::TextDisabled("(no targets — uses active palette piece)");
            }
            else
            {
                for (int i = 0; i < (int)targets.size(); ++i)
                {
                    ImGui::PushID(i);
                    const LBToolPicker::PieceChoice* pc =
                        LBToolPicker::FindByAsset(pieces, targets[i]);
                    LBToolPicker::DrawThumbButton(pc, projectRoot, kitFolder,
                                                  32.0f, false, "tgt_thumb", "?");
                    if (ImGui::IsItemHovered() && pc)
                        ImGui::SetTooltip("%s", pc->display.c_str());
                    ImGui::PopID();
                    if (i + 1 < (int)targets.size()) ImGui::SameLine();
                }
            }
            if (ImGui::Button("Edit targets…##edit_targets",
                              ImVec2(150, 0)))
                openRequested = true;
            ImGui::Unindent();
        }
        ImGui::PopID();
        return openRequested;
    }
}
#endif

void LBToolMaskFill::DrawSettingsUI()
{
#if EDITOR
    ImGui::TextUnformatted("MaskFill — paint with a B&W or RGB mask");
    ImGui::Separator();

    LevelBuilderCoreAPI* api = CoreAPI();
    PolyphaseEngineAPI* eng = api ? (PolyphaseEngineAPI*)api->GetEngineAPI() : nullptr;
    EditorUIHooks* uiHooks = eng ? eng->editorUI : nullptr;
    std::vector<LBToolPicker::PieceChoice> pieces =
        LBToolPicker::CollectActiveKitPieces(api);
    const std::string projectRoot = LBToolPicker::CachedProjectRoot();
    const std::string kitFolder   = LBToolPicker::GetActiveKitFolder(api);

    // ---- Mask path + Browse + Reload + 96x96 thumbnail preview ----
    // Single shared call — also used by Paint's mask brush.
    LBToolMaskImage::DrawMaskUI(sMask, "mf");

    ImGui::Spacing();

    // ---- Per-stamp settings (shared across channels) ----
    ImGui::SliderFloat("Cell stride (m)", &sCellStride, 0.1f, 10.0f, "%.2f");
    ImGui::SliderFloat("Threshold",       &sThreshold,  0.0f, 1.0f, "%.2f");
    ImGui::Checkbox  ("Invert (place where pixel BELOW threshold)", &sInvert);
    ImGui::SliderFloat("Position jitter", &sJitterPos,  0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Yaw jitter (deg)", &sJitterYaw, 0.0f, 180.0f, "%.0f");

    int seedI = (int)sSeed;
    if (ImGui::InputInt("Seed", &seedI))
        sSeed = (uint32_t)(seedI <= 0 ? 1 : seedI);
    ImGui::SameLine();
    if (ImGui::Button("Reroll##mf_seed"))
        sSeed = (sSeed * 1664525u + 1013904223u) | 1u;

    ImGui::Spacing();

    // ---- Channels ----
    if (ImGui::CollapsingHeader("Channels & Targets##mf_channels",
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Each enabled channel samples the mask "
                            "independently — multiple can fire at one cell.");

        if (DrawChannelRow("Luminance (R+G+B avg)",
                           ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                           sUseLuminance, sLumTargets, pieces,
                           projectRoot, kitFolder, "lum"))
        {
            sOpenLumPicker = true; sJustOpened = true;
        }
        if (DrawChannelRow("Red channel",
                           ImVec4(1.0f, 0.40f, 0.40f, 1.0f),
                           sUseRed, sRTargets, pieces,
                           projectRoot, kitFolder, "r"))
        {
            sOpenRPicker = true; sJustOpened = true;
        }
        if (DrawChannelRow("Green channel",
                           ImVec4(0.40f, 1.0f, 0.40f, 1.0f),
                           sUseGreen, sGTargets, pieces,
                           projectRoot, kitFolder, "g"))
        {
            sOpenGPicker = true; sJustOpened = true;
        }
        if (DrawChannelRow("Blue channel",
                           ImVec4(0.40f, 0.60f, 1.0f, 1.0f),
                           sUseBlue, sBTargets, pieces,
                           projectRoot, kitFolder, "b"))
        {
            sOpenBPicker = true; sJustOpened = true;
        }
    }

    ImGui::Spacing();
    if (sHasStart)
    {
        ImGui::TextColored(ImVec4(0.85f, 0.40f, 0.95f, 1.0f),
            "Click 2 to commit. Start: (%.2f, %.2f, %.2f)",
            sStart.x, sStart.y, sStart.z);
        if (ImGui::Button("Cancel##mf_cancel")) sHasStart = false;
    }
    else
    {
        ImGui::TextDisabled("Click in the viewport to set the first corner.");
    }

    // (mask status text is now part of DrawMaskUI above)

    // ---- Modals — opened from latches above ----
    if (sOpenLumPicker) { ImGui::OpenPopup("MaskFill: Luminance Targets##mf_lum_modal");  sOpenLumPicker = false; }
    if (sOpenRPicker)   { ImGui::OpenPopup("MaskFill: Red Targets##mf_r_modal");          sOpenRPicker   = false; }
    if (sOpenGPicker)   { ImGui::OpenPopup("MaskFill: Green Targets##mf_g_modal");        sOpenGPicker   = false; }
    if (sOpenBPicker)   { ImGui::OpenPopup("MaskFill: Blue Targets##mf_b_modal");         sOpenBPicker   = false; }

    LBToolPicker::DrawPiecePickerModal("MaskFill: Luminance Targets##mf_lum_modal",
                                       "Luminance — multi-select",
                                       /*multiSelect=*/true, /*allowAny=*/false,
                                       sJustOpened, &sLumTargets);
    LBToolPicker::DrawPiecePickerModal("MaskFill: Red Targets##mf_r_modal",
                                       "Red channel — multi-select",
                                       /*multiSelect=*/true, /*allowAny=*/false,
                                       sJustOpened, &sRTargets);
    LBToolPicker::DrawPiecePickerModal("MaskFill: Green Targets##mf_g_modal",
                                       "Green channel — multi-select",
                                       /*multiSelect=*/true, /*allowAny=*/false,
                                       sJustOpened, &sGTargets);
    LBToolPicker::DrawPiecePickerModal("MaskFill: Blue Targets##mf_b_modal",
                                       "Blue channel — multi-select",
                                       /*multiSelect=*/true, /*allowAny=*/false,
                                       sJustOpened, &sBTargets);
#endif
}

void LBToolMaskFill::Initialize(LevelBuilderCoreAPI* api)
{
    if (!api) return;
    if (api->RegisterBrush) api->RegisterBrush("MaskFill", &sInstance);
}

void LBToolMaskFill::Shutdown(LevelBuilderCoreAPI* api)
{
    if (!api) return;
    if (api->UnregisterBrush) api->UnregisterBrush("MaskFill");
    sHasStart = false;
#if EDITOR
    sMask.rgba.clear();
    sMask.w = sMask.h = 0;
    sMask.loadedPath.clear();
#endif
}

// Viewport overlay — magenta rectangle outline while picking corner 2.
extern "C" void LBToolMaskFill_DrawViewportOverlayTrampoline(
    float /*vx*/, float /*vy*/, float /*vw*/, float /*vh*/, void* /*ud*/)
{
#if EDITOR
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api || !api->GetActiveBrushName || !api->GetEngineAPI) return;
    const char* active = api->GetActiveBrushName();
    if (!active || std::strcmp(active, "MaskFill") != 0) return;
    if (!sHasStart) return;

    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (!eng || !eng->Gizmos_DrawLine || !eng->Gizmos_SetColor) return;

    LBVec3 hit;
    LBQuat hitRot;
    LBVec3 dummy;
    if (api->GetPreviewTransform) api->GetPreviewTransform(&hit, &hitRot, &dummy);

    LBVec3 rmin, rmax; float y = 0.0f;
    RectFromCorners(sStart, hit, rmin, rmax, y);

    eng->Gizmos_SetColor(0.95f, 0.30f, 0.95f, 0.9f);
    eng->Gizmos_DrawLine(rmin.x, y, rmin.z, rmax.x, y, rmin.z);
    eng->Gizmos_DrawLine(rmax.x, y, rmin.z, rmax.x, y, rmax.z);
    eng->Gizmos_DrawLine(rmax.x, y, rmax.z, rmin.x, y, rmax.z);
    eng->Gizmos_DrawLine(rmin.x, y, rmax.z, rmin.x, y, rmin.z);

    if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
#endif
}
