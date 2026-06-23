#include "LBToolMaskImage.h"

#if EDITOR

#include "LevelBuilderCoreAPI.h"
#include "LevelBuilderCoreLoader.h"
#include "ThumbnailCache.h"

#include "imgui.h"
#include "Plugins/PolyphaseEngineAPI.h"
#include "Plugins/EditorUIHooks.h"

// stb_image — STB_IMAGE_STATIC keeps every stbi_* symbol internal so we
// don't collide with the engine's own copy or with ThumbnailCache's.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdio>
#include <cstring>

namespace
{
    LevelBuilderCoreAPI* CoreAPI() { return LevelBuilderCoreLoader::Get(); }
}

namespace LBToolMaskImage
{
    bool EnsureLoaded(MaskState& s)
    {
        std::string current = s.pathBuf;
        if (current.empty())
        {
            s.loadedPath.clear();
            s.rgba.clear();
            s.w = s.h = 0;
            return false;
        }
        if (current == s.loadedPath && !s.rgba.empty()) return true;

        int w = 0, h = 0, c = 0;
        // STBI_rgb_alpha forces 4-channel even if the source is greyscale;
        // greyscale gets duplicated into R/G/B with alpha=255.
        stbi_uc* px = stbi_load(current.c_str(), &w, &h, &c, STBI_rgb_alpha);
        if (!px)
        {
            s.rgba.clear();
            s.w = s.h = 0;
            s.loadedPath.clear();
            s.lastStatus = "Mask: failed to decode " + current;
            return false;
        }
        s.w = w; s.h = h;
        s.rgba.assign(px, px + (size_t)w * h * 4);
        stbi_image_free(px);
        s.loadedPath = current;

        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Mask: loaded %dx%d (%s)", w, h, current.c_str());
        s.lastStatus = buf;
        return true;
    }

    void ForceReload(MaskState& s)
    {
        s.loadedPath.clear();      // force the cache miss
        EnsureLoaded(s);
    }

    Pixel Sample(const MaskState& s, float u, float v)
    {
        if (s.rgba.empty() || s.w <= 0 || s.h <= 0) return {0,0,0,0};
        int px = (int)(u * (float)(s.w - 1) + 0.5f);
        int py = (int)(v * (float)(s.h - 1) + 0.5f);
        if (px < 0) px = 0; if (px >= s.w) px = s.w - 1;
        if (py < 0) py = 0; if (py >= s.h) py = s.h - 1;
        py = (s.h - 1) - py;        // image up → world +Z
        const uint8_t* p = &s.rgba[((size_t)py * s.w + px) * 4];
        return Pixel{ p[0], p[1], p[2], p[3] };
    }

    bool DrawMaskUI(MaskState& s, const char* idPrefix)
    {
        char idBuf[64];
        bool changed = false;

        // ---- Path text input ----
        std::snprintf(idBuf, sizeof(idBuf), "##%s_mask_path", idPrefix);
        ImGui::SetNextItemWidth(360);
        if (ImGui::InputText(idBuf, s.pathBuf, sizeof(s.pathBuf)))
            changed = true;
        ImGui::SameLine();

        // ---- Browse (file dialog, v7+) ----
        LevelBuilderCoreAPI* api = CoreAPI();
        PolyphaseEngineAPI* eng = api ? (PolyphaseEngineAPI*)api->GetEngineAPI() : nullptr;
        EditorUIHooks* uiHooks = eng ? eng->editorUI : nullptr;

        std::snprintf(idBuf, sizeof(idBuf), "Browse…##%s_mask_browse", idPrefix);
        if (uiHooks && uiHooks->ShowOpenFileDialog)
        {
            if (ImGui::Button(idBuf))
            {
                char picked[512] = {0};
                if (uiHooks->ShowOpenFileDialog("Pick mask PNG",
                                                "PNG / image (*.png;*.jpg;*.bmp)|*.png;*.jpg;*.bmp",
                                                nullptr,
                                                picked, sizeof(picked)))
                {
                    std::snprintf(s.pathBuf, sizeof(s.pathBuf), "%s", picked);
                    EnsureLoaded(s);
                    changed = true;
                }
            }
        }
        else
        {
            ImGui::BeginDisabled(true);
            ImGui::Button(idBuf);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("File dialog requires engine plugin API v7+");
        }
        ImGui::SameLine();

        // ---- Reload ----
        std::snprintf(idBuf, sizeof(idBuf), "Reload##%s_mask_reload", idPrefix);
        if (ImGui::Button(idBuf))
            ForceReload(s);

        // ---- Thumbnail preview + dimensions ----
        // Lazy-load if path is set but cache is empty (e.g. on re-enter).
        if (!s.rgba.empty() == false && s.pathBuf[0] != '\0')
            EnsureLoaded(s);

        ImTextureID maskTex = 0;
        if (!s.loadedPath.empty())
            maskTex = ThumbnailCache::Get(s.loadedPath);
        if (maskTex != 0)
        {
            ImGui::Image(maskTex, ImVec2(96.0f, 96.0f));
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        if (s.w > 0 && s.h > 0)
        {
            ImGui::TextDisabled("Mask: %dx%d", s.w, s.h);
            ImGui::TextDisabled("(image up = world +Z)");
        }
        else
        {
            ImGui::TextDisabled("No mask loaded.");
        }
        if (!s.lastStatus.empty())
            ImGui::TextWrapped("%s", s.lastStatus.c_str());
        ImGui::EndGroup();

        return changed;
    }
}

#endif // EDITOR
