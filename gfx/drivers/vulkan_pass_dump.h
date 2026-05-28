/* gfx/drivers/vulkan_pass_dump.h
 *
 * Vulkan slang-shader pass dump: capture each pass's color attachment
 * (plus the Original input texture) for one frame to KTX 2.0 files.
 * See docs/superpowers/specs/2026-05-27-slang-pass-dump-design.md.
 */
#ifndef VULKAN_PASS_DUMP_H__
#define VULKAN_PASS_DUMP_H__

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#include "../drivers_shader/shader_vulkan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vulkan_pass_dump vulkan_pass_dump_t;

/* Driver context fields the dump module needs.  Passed via this small
 * struct rather than pulling in the entire vk_t to keep coupling low. */
typedef struct vulkan_pass_dump_ctx
{
   VkDevice                                device;
   VkPhysicalDevice                        gpu;
   const VkPhysicalDeviceMemoryProperties *mem_props;
   uint64_t                                frame_count;
   /* The chain's input_texture.format is VK_FORMAT_UNDEFINED for SW-rendered
    * cores (the chain doesn't need it; the underlying texture handles
    * format internally).  This hint lets the dump module record the real
    * format in KTX2 headers anyway.  Set to VK_FORMAT_UNDEFINED to disable. */
   VkFormat                                original_format_hint;
} vulkan_pass_dump_ctx_t;

vulkan_pass_dump_t *vulkan_pass_dump_arm(
      const vulkan_pass_dump_ctx_t *ctx,
      vulkan_filter_chain_t *chain,
      const char *screenshot_dir,
      const char *core_short_name,
      const char *preset_path);

/* Stubs (filled in T10-T12). */
void vulkan_pass_dump_record_offscreen(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      vulkan_filter_chain_t *chain);

void vulkan_pass_dump_record_final(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      vulkan_filter_chain_t *chain,
      VkImage swapchain_image,
      VkFormat swapchain_format,
      VkExtent2D swapchain_extent);

void vulkan_pass_dump_flush(vulkan_pass_dump_t *dump);

bool vulkan_pass_dump_active(const vulkan_pass_dump_t *dump);

void vulkan_pass_dump_free(vulkan_pass_dump_t *dump);

#ifdef __cplusplus
}
#endif

#endif
