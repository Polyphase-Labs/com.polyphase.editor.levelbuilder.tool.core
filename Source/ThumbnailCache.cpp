#include "ThumbnailCache.h"

#if EDITOR

// We use the engine's renderer types (Image, DestroyQueue, DeviceWaitIdle)
// which only link when the engine is linked at runtime via the DLL plugin
// path. The static-build flavor embeds the addon into the shipped game
// binary BEFORE the engine is linked, so those externals are unresolved
// at the addon's link stage. Gate the renderer code on the DLL-export
// macro (only defined in the DLL build — build.bat sets it; the engine's
// static-build wrapper does not). In a shipped game the thumbnail cache
// is a no-op, which is correct: shipped games don't render the editor
// piece-browser.
#if defined(POLYPHASE_PLUGIN_EXPORT) || defined(OCTAVE_PLUGIN_EXPORT)
#define LB_THUMBNAIL_CACHE_HAS_RENDERER 1
#else
#define LB_THUMBNAIL_CACHE_HAS_RENDERER 0
#endif

#if LB_THUMBNAIL_CACHE_HAS_RENDERER && API_VULKAN
#include "Graphics/Vulkan/Image.h"
#include "Graphics/Vulkan/VulkanUtils.h"
#include "backends/imgui_impl_vulkan.h"

// File-local stb_image — STB_IMAGE_STATIC makes every stbi_* symbol have
// internal linkage in this TU, so we won't collide with the engine's own
// stb_image implementation in Polyphase.lib.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#endif

#include <unordered_map>

namespace
{
    struct Entry
    {
        ImTextureID texId = 0;
#if LB_THUMBNAIL_CACHE_HAS_RENDERER && API_VULKAN
        Image*      image = nullptr;
#endif
    };

    std::unordered_map<std::string, Entry> sCache;
}

namespace ThumbnailCache
{
    ImTextureID Get(const std::string& absPath)
    {
        auto it = sCache.find(absPath);
        if (it != sCache.end())
            return it->second.texId;

#if LB_THUMBNAIL_CACHE_HAS_RENDERER && API_VULKAN
        int width = 0, height = 0, channels = 0;
        stbi_uc* pixels = stbi_load(absPath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (!pixels)
        {
            sCache[absPath] = {};   // negative cache — don't reopen every frame
            return 0;
        }

        ImageDesc imgDesc;
        imgDesc.mWidth     = (uint32_t)width;
        imgDesc.mHeight    = (uint32_t)height;
        imgDesc.mFormat    = VK_FORMAT_R8G8B8A8_UNORM;
        imgDesc.mUsage     = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgDesc.mMipLevels = 1;
        imgDesc.mLayers    = 1;

        SamplerDesc sampDesc;
        sampDesc.mMagFilter   = VK_FILTER_LINEAR;
        sampDesc.mMinFilter   = VK_FILTER_LINEAR;
        sampDesc.mAddressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        Image* image = new Image(imgDesc, sampDesc, "ModularPieceThumbnail");
        image->Update(pixels);
        image->Transition(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        stbi_image_free(pixels);

        ImTextureID texId = (ImTextureID)ImGui_ImplVulkan_AddTexture(
            image->GetSampler(),
            image->GetView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        Entry e;
        e.texId = texId;
        e.image = image;
        sCache[absPath] = e;
        return texId;
#else
        sCache[absPath] = {};
        return 0;
#endif
    }

    void Clear()
    {
#if LB_THUMBNAIL_CACHE_HAS_RENDERER && API_VULKAN
        if (sCache.empty()) return;
        DeviceWaitIdle();
        for (auto& kv : sCache)
        {
            if (kv.second.texId != 0)
                ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)kv.second.texId);
            if (kv.second.image != nullptr)
                GetDestroyQueue()->Destroy(kv.second.image);
        }
#endif
        sCache.clear();
    }
}

#endif // EDITOR
