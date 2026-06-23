#include "LBToolPicker.h"

#if EDITOR

#include "LevelBuilderCoreLoader.h"
#include "ThumbnailCache.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <unordered_set>

namespace LBToolPicker
{
    static LevelBuilderCoreAPI* CoreAPI() { return LevelBuilderCoreLoader::Get(); }

    std::vector<PieceChoice> CollectActiveKitPieces(LevelBuilderCoreAPI* api)
    {
        std::vector<PieceChoice> out;
        if (!api || !api->Kit_GetActiveIndex || !api->Kit_GetInfo
                 || !api->Kit_GetPieceInfo)
            return out;

        int kitIdx = api->Kit_GetActiveIndex();
        if (kitIdx < 0) return out;
        LBKitInfo ki{};
        if (!api->Kit_GetInfo(kitIdx, &ki)) return out;
        out.reserve(ki.pieceCount);
        for (int i = 0; i < ki.pieceCount; ++i)
        {
            LBPieceInfo pi{};
            if (!api->Kit_GetPieceInfo(kitIdx, i, &pi)) continue;
            PieceChoice pc;
            pc.display  = pi.name      ? pi.name      : (pi.assetName ? pi.assetName : "<unnamed>");
            pc.asset    = pi.assetName ? pi.assetName : "";
            pc.category = pi.category  ? pi.category  : "";
            pc.iconPath = pi.iconPath  ? pi.iconPath  : "";
            if (!pc.asset.empty()) out.push_back(std::move(pc));
        }
        return out;
    }

    const PieceChoice* FindByAsset(const std::vector<PieceChoice>& pieces,
                                   const std::string& asset)
    {
        for (const auto& p : pieces)
            if (p.asset == asset) return &p;
        return nullptr;
    }

    const std::string& CachedProjectRoot()
    {
        static std::string sRoot;
        LevelBuilderCoreAPI* api = CoreAPI();
        if (api && api->GetProjectRoot)
        {
            const char* p = api->GetProjectRoot();
            if (p && *p) { sRoot = p; return sRoot; }
        }
        sRoot.clear();
        return sRoot;
    }

    std::string GetActiveKitFolder(LevelBuilderCoreAPI* api)
    {
        if (!api || !api->Kit_GetActiveIndex || !api->Kit_GetInfo) return {};
        int kitIdx = api->Kit_GetActiveIndex();
        if (kitIdx < 0) return {};
        LBKitInfo ki{};
        if (!api->Kit_GetInfo(kitIdx, &ki)) return {};
        if (!ki.sourceFile || !*ki.sourceFile) return {};
        return std::filesystem::path(ki.sourceFile).parent_path().string();
    }

    std::string ResolveIconAbs(const std::string& iconPath,
                               const std::string& projectRoot,
                               const std::string& kitFolder)
    {
        if (iconPath.empty()) return {};
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path ip(iconPath);
        if (ip.is_absolute()) return iconPath;
        if (!kitFolder.empty())
        {
            fs::path a = fs::path(kitFolder) / iconPath;
            if (fs::is_regular_file(a, ec)) return a.string();
        }
        if (!projectRoot.empty())
        {
            fs::path a = fs::path(projectRoot) / iconPath;
            return a.string();
        }
        return iconPath;
    }

    ImTextureID FetchThumbnail(const PieceChoice& pc,
                               const std::string& projectRoot,
                               const std::string& kitFolder)
    {
        if (pc.iconPath.empty()) return 0;
        std::string abs = ResolveIconAbs(pc.iconPath, projectRoot, kitFolder);
        if (abs.empty()) return 0;
        return ThumbnailCache::Get(abs);
    }

    bool DrawThumbButton(const PieceChoice* pc,
                         const std::string& projectRoot,
                         const std::string& kitFolder,
                         float size,
                         bool selected,
                         const char* idStr,
                         const char* placeholderLabel)
    {
        ImGui::PushID(idStr);
        bool clicked = false;

        ImTextureID tex = pc ? FetchThumbnail(*pc, projectRoot, kitFolder) : 0;

        if (tex != 0)
        {
            const ImVec4 bg = selected
                              ? ImVec4(0.20f, 0.85f, 1.0f, 0.45f)
                              : ImVec4(0, 0, 0, 0);
            clicked = ImGui::ImageButton("##thumb", tex, ImVec2(size, size),
                                         ImVec2(0,0), ImVec2(1,1), bg);
        }
        else
        {
            if (selected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.45f, 0.80f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.90f, 1.0f));
            }
            std::string lbl = pc ? pc->display.substr(0, 12) : placeholderLabel;
            clicked = ImGui::Button(lbl.c_str(), ImVec2(size + 8, size + 8));
            if (selected) { ImGui::PopStyleColor(); ImGui::PopStyleColor(); }
        }

        if (selected)
        {
            ImVec2 mn = ImGui::GetItemRectMin();
            ImVec2 mx = ImGui::GetItemRectMax();
            ImU32  col = IM_COL32(50, 220, 255, 255);
            ImGui::GetWindowDrawList()->AddRect(mn, mx, col, 4.0f, 0, 3.0f);
        }

        ImGui::PopID();
        return clicked;
    }

    bool DrawPiecePickerModal(const char* modalId,
                              const char* headerLabel,
                              bool multiSelect,
                              bool allowAny,
                              bool& seedFromOut,
                              std::vector<std::string>* out)
    {
        if (!out) return false;
        bool confirmed = false;

        // Working set persists across frames the modal is open. Re-
        // seeded from *out whenever the caller flips seedFromOut true.
        static std::unordered_set<std::string> sWorkingSet;
        if (ImGui::IsPopupOpen(modalId) && seedFromOut)
        {
            sWorkingSet.clear();
            for (const auto& a : *out) sWorkingSet.insert(a);
            seedFromOut = false;
        }

        ImGui::SetNextWindowSize(ImVec2(680.0f, 520.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::BeginPopupModal(modalId, nullptr, 0))
            return false;

        LevelBuilderCoreAPI* api = CoreAPI();
        std::vector<PieceChoice> pieces = CollectActiveKitPieces(api);
        const std::string projectRoot = CachedProjectRoot();
        const std::string kitFolder   = GetActiveKitFolder(api);

        const char* activeKitName = (api && api->Kit_GetActiveName)
                                  ? api->Kit_GetActiveName() : "<no kit>";
        ImGui::Text("%s — Active kit: %s   (%d piece(s))",
                    headerLabel,
                    (activeKitName && *activeKitName) ? activeKitName : "<none>",
                    (int)pieces.size());
        ImGui::Separator();

        if (allowAny && !multiSelect)
        {
            if (ImGui::Button("<any piece in radius>", ImVec2(220, 30)))
            {
                out->clear();
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return true;
            }
            ImGui::Spacing();
            ImGui::Separator();
        }

        const float thumbSize = 64.0f;
        const float cellW     = thumbSize + 16.0f;
        const float avail     = ImGui::GetContentRegionAvail().x;
        const int   cols      = std::max(1, (int)(avail / cellW));

        ImGui::BeginChild("##picker_scroll",
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 8),
                          true);
        for (int i = 0; i < (int)pieces.size(); ++i)
        {
            const PieceChoice& pc = pieces[i];
            const bool isSel = (sWorkingSet.find(pc.asset) != sWorkingSet.end());

            ImGui::BeginGroup();
            char idBuf[64];
            std::snprintf(idBuf, sizeof(idBuf), "pick_%d", i);
            bool clicked = DrawThumbButton(&pc, projectRoot, kitFolder,
                                           thumbSize, isSel, idBuf);
            std::string lbl = pc.display.size() > 14
                              ? (pc.display.substr(0, 12) + "..")
                              : pc.display;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cellW);
            ImGui::TextWrapped("%s", lbl.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();

            if (clicked)
            {
                if (multiSelect)
                {
                    if (isSel) sWorkingSet.erase(pc.asset);
                    else       sWorkingSet.insert(pc.asset);
                }
                else
                {
                    out->clear();
                    out->push_back(pc.asset);
                    ImGui::EndChild();
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    return true;
                }
            }

            if (((i + 1) % cols) != 0) ImGui::SameLine();
        }
        ImGui::EndChild();

        if (multiSelect)
        {
            if (ImGui::Button("Confirm", ImVec2(120, 0)))
            {
                out->clear();
                out->reserve(sWorkingSet.size());
                for (const auto& a : sWorkingSet) out->push_back(a);
                std::sort(out->begin(), out->end());
                ImGui::CloseCurrentPopup();
                confirmed = true;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
        return confirmed;
    }
}

#endif // EDITOR
