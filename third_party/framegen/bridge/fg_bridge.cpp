// SwanStationPS5 - frame generation for the presenter (fg_bridge.h): PS5SX2's interpolator
// (third_party/framegen/ps5_framegen.cpp) behind a C interface, its Vulkan
// commands looked up from SwanStationPS5's device.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../../src/platform/vk/fg_bridge.h"
#include "OrbisFrameGenPacing.h"
#include "ps5_framegen.h"

#include <cstdio>
#include <new>
#include <string>

#define FG_VK_DEFINE(name) PFN_##name name;
FG_VK_INSTANCE_FUNCS(FG_VK_DEFINE)
FG_VK_DEVICE_FUNCS(FG_VK_DEFINE)
#undef FG_VK_DEFINE

struct FgInterpolator
{
    ps5::framegen::interpolator fg;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkImage stash[3] = {};
    VkDeviceMemory stash_memory[3] = {};
    VkImageView stash_view[3] = {};
    bool stash_ready = false;
    uint32_t last_generated = 1;
};

static bool stash_create(FgInterpolator *fg, VkPhysicalDevice gpu)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (int i = 0; i < 3; ++i)
    {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = ps5::framegen::interpolator::k_format;
        ci.extent = {fg->width, fg->height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(fg->device, &ci, nullptr, &fg->stash[i]) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(fg->device, fg->stash[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < props.memoryTypeCount && type == UINT32_MAX; ++t)
            if ((req.memoryTypeBits & (1u << t)) &&
                (props.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = t;
        if (type == UINT32_MAX)
            return false;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(fg->device, &ai, nullptr, &fg->stash_memory[i]) != VK_SUCCESS ||
            vkBindImageMemory(fg->device, fg->stash[i], fg->stash_memory[i], 0) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = fg->stash[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(fg->device, &vi, nullptr, &fg->stash_view[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

static void stash_destroy(FgInterpolator *fg)
{
    for (int i = 0; i < 3; ++i)
    {
        if (fg->stash_view[i])
            vkDestroyImageView(fg->device, fg->stash_view[i], nullptr);
        if (fg->stash[i])
            vkDestroyImage(fg->device, fg->stash[i], nullptr);
        if (fg->stash_memory[i])
            vkFreeMemory(fg->device, fg->stash_memory[i], nullptr);
        fg->stash_view[i] = VK_NULL_HANDLE;
        fg->stash[i] = VK_NULL_HANDLE;
        fg->stash_memory[i] = VK_NULL_HANDLE;
    }
}

extern "C" FgInterpolator *ssfg_create(PFN_vkVoidFunction (*get_instance_proc)(VkInstance, const char *),
                                           VkInstance instance, VkPhysicalDevice gpu, VkDevice device, uint32_t width,
                                           uint32_t height, VkFormat format, char *error, size_t error_size)
{
    bool loaded = true;
#define FG_VK_LOAD_INSTANCE(name)                                                                  \
    name = reinterpret_cast<PFN_##name>(get_instance_proc(instance, #name));                       \
    loaded = loaded && name;
    FG_VK_INSTANCE_FUNCS(FG_VK_LOAD_INSTANCE)
#undef FG_VK_LOAD_INSTANCE
#define FG_VK_LOAD_DEVICE(name)                                                                    \
    name = vkGetDeviceProcAddr ? reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)) : nullptr; \
    loaded = loaded && name;
    FG_VK_DEVICE_FUNCS(FG_VK_LOAD_DEVICE)
#undef FG_VK_LOAD_DEVICE
    if (!loaded)
    {
        std::snprintf(error, error_size, "a Vulkan command is missing");
        return nullptr;
    }
    FgInterpolator *fg = new (std::nothrow) FgInterpolator;
    if (!fg)
    {
        std::snprintf(error, error_size, "no memory");
        return nullptr;
    }
    std::string why;
    if (!fg->fg.create(gpu, device, width, height, why, format))
    {
        std::snprintf(error, error_size, "%s", why.c_str());
        delete fg;
        return nullptr;
    }
    fg->device = device;
    fg->width = width;
    fg->height = height;
    if (!stash_create(fg, gpu))
    {
        std::snprintf(error, error_size, "no memory for the generated frames");
        stash_destroy(fg);
        fg->fg.destroy();
        delete fg;
        return nullptr;
    }
    return fg;
}

extern "C" void ssfg_destroy(FgInterpolator *fg)
{
    if (fg)
    {
        stash_destroy(fg);
        fg->fg.destroy();
    }
    delete fg;
}

extern "C" void ssfg_prepare(FgInterpolator *fg, VkCommandBuffer cmd)
{
    fg->fg.prepare(cmd);
}

extern "C" VkImage ssfg_frame_image(FgInterpolator *fg)
{
    return fg->fg.frame_image();
}

extern "C" VkImageView ssfg_frame_view(FgInterpolator *fg)
{
    return fg->fg.frame_view();
}

extern "C" bool ssfg_record(FgInterpolator *fg, VkCommandBuffer cmd, bool reset)
{
    return fg->fg.record(cmd, reset);
}

static void stash_copy(FgInterpolator *fg, VkCommandBuffer cmd, uint32_t i)
{
    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {fg->width, fg->height, 1};
    vkCmdCopyImage(cmd, fg->fg.output_image(), VK_IMAGE_LAYOUT_GENERAL, fg->stash[i], VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

extern "C" bool ssfg_record_n(FgInterpolator *fg, VkCommandBuffer cmd, bool reset, uint32_t generated)
{
    generated = generated < 1 ? 1 : generated > 3 ? 3 : generated;
    if (generated > 1 && !fg->stash_ready)
    {
        VkImageMemoryBarrier b[3] = {};
        for (int i = 0; i < 3; ++i)
        {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].image = fg->stash[i];
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 3, b);
        fg->stash_ready = true;
    }
    fg->fg.set_generated(generated);
    fg->last_generated = generated;
    const bool made = fg->fg.record(cmd, reset);
    if (made && generated > 1)
    {
        stash_copy(fg, cmd, 0);
        for (uint32_t i = 1; i < generated; ++i)
        {
            if (!fg->fg.interpolate_next(cmd, i))
                return false;
            stash_copy(fg, cmd, i);
        }
    }
    return made;
}

extern "C" VkImageView ssfg_generated_view(FgInterpolator *fg, uint32_t i)
{
    return fg->last_generated > 1 && i < 3 ? fg->stash_view[i] : fg->fg.output_view();
}

extern "C" VkImageView ssfg_output_view(FgInterpolator *fg)
{
    return fg->fg.output_view();
}

extern "C" void ssfg_advance(FgInterpolator *fg)
{
    fg->fg.advance();
}

struct FgPacing
{
    orbis_fg::Pacing pacing;
};

extern "C" FgPacing *ssfg_pacing_create(void)
{
    return new (std::nothrow) FgPacing;
}

extern "C" void ssfg_pacing_destroy(FgPacing *pacing)
{
    delete pacing;
}

extern "C" FgDecision ssfg_pacing_frame(FgPacing *pacing, uint32_t vsyncs, uint32_t already, double core_hz,
                                        double display_hz, double now, double speed, bool nominal,
                                        uint32_t max_generated)
{
    const orbis_fg::Decision d =
        pacing->pacing.Frame(vsyncs, already, core_hz, display_hz, now, speed, nominal, max_generated);
    FgDecision out;
    out.engaged = d.engaged;
    out.reset = d.reset;
    out.presents = d.presents;
    out.generated = d.generated;
    out.state = static_cast<int>(pacing->pacing.GetState());
    out.vsyncs = pacing->pacing.MedianVsyncs();
    out.refreshes = pacing->pacing.Refreshes();
    return out;
}

extern "C" const char *ssfg_pacing_state_name(int state)
{
    static const char *const names[] = {"warming up", "no room (the game's frames fill every refresh)",
                                        "unsteady (the game's frames are not of one length)",
                                        "not at normal speed", "paused (the game slowed)", "generating"};
    return state >= 0 && state < 6 ? names[state] : "?";
}

extern "C" const char *ssfg_pacing_event(FgPacing *pacing)
{
    switch (pacing->pacing.TakeEvent())
    {
        case orbis_fg::Event::Paused: return "the game slowed while frames were generated: none for a while";
        case orbis_fg::Event::ResumedCostly: return "the game ran at full speed without generated frames: the next pause is longer";
        case orbis_fg::Event::ResumedSlowGame: return "the game is as slow without generated frames: generating again";
        default: return nullptr;
    }
}
