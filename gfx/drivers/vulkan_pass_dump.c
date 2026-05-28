/* gfx/drivers/vulkan_pass_dump.c */

#include "vulkan_pass_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <file/file_path.h>
#include <string/stdstring.h>

#include "../common/vulkan_common.h"
#include "../../verbosity.h"
#include "../../version.h"
#include "../../version_git.h"
#include "ktx2_writer.h"

/* Per-image capture state. */
typedef struct
{
   VkImage         src_image;
   VkFormat        format;
   VkExtent2D      extent;
   VkImageLayout   prev_layout;     /* restored after blit */
   VkDeviceSize    staging_size;
   VkBuffer        staging;
   VkDeviceMemory  memory;
   void           *mapped;
   char           *out_filename;      /* sanitized, basename only */
   char            source_shader[256];/* raw pass name before sanitization */
   char            display_name[64];  /* short name for manifest/KVD JSON */
   int             pass_index;        /* -1 for Original, num_passes-1 for final */
   bool            recorded;          /* set when blit emitted onto cmd */
} pass_image_t;

struct vulkan_pass_dump
{
   vulkan_pass_dump_ctx_t  ctx;

   char                    out_dir[2048];          /* full path of subfolder */
   char                    preset_path[2048];      /* copied from caller */
   char                    swapchain_format_name[64];
   uint64_t                frame_count;
   char                    capture_time_iso[40];

   pass_image_t           *images;          /* size = num_images */
   unsigned                num_images;      /* 1 (original) + offscreen_passes + 1 (final) */
   unsigned                offscreen_count;

   bool                    aborted;
   bool                    offscreen_recorded;
   bool                    final_recorded;
};

bool vulkan_pass_dump_active(const vulkan_pass_dump_t *dump)
{
   return dump != NULL;
}

static void format_iso_time(char *out, size_t out_size)
{
   time_t now = time(NULL);
   struct tm tm_buf;
#if defined(_WIN32)
   gmtime_s(&tm_buf, &now);
#else
   gmtime_r(&now, &tm_buf);
#endif
   strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
}

static void format_filename_timestamp(char *out, size_t out_size)
{
   time_t now = time(NULL);
   struct tm tm_buf;
#if defined(_WIN32)
   localtime_s(&tm_buf, &now);
#else
   localtime_r(&now, &tm_buf);
#endif
   strftime(out, out_size, "%Y-%m-%d_%H-%M-%S", &tm_buf);
}

/* Sanitize a pass name to [A-Za-z0-9_-]; truncate to 32 chars; empty -> "unnamed".
 * Strips directory + extension if a path was passed in (e.g. shader source). */
static void sanitize_pass_name(const char *in, char *out, size_t out_size)
{
   const char *base;
   const char *dot;
   size_t      j = 0;
   size_t      i;

   if (!in || !*in)
   {
      snprintf(out, out_size, "unnamed");
      return;
   }

   base = strrchr(in, '/');
   if (!base) base = strrchr(in, '\\');
   if (base) base++; else base = in;

   dot = strrchr(base, '.');

   for (i = 0; base[i] && (!dot || &base[i] < dot) && j + 1 < out_size && j < 32; i++)
   {
      char c = base[i];
      if (    (c >= 'A' && c <= 'Z')
           || (c >= 'a' && c <= 'z')
           || (c >= '0' && c <= '9')
           || c == '_' || c == '-')
         out[j++] = c;
      else
         out[j++] = '_';
   }
   if (j == 0)
   {
      snprintf(out, out_size, "unnamed");
      return;
   }
   out[j] = '\0';
}

/* Allocate one host-visible staging buffer sized for the image.  Returns true on success. */
static bool alloc_staging(const vulkan_pass_dump_ctx_t *ctx,
      VkDeviceSize size, VkBuffer *out_buf, VkDeviceMemory *out_mem, void **out_map)
{
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };

   bci.size        = size;
   bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

   if (vkCreateBuffer(ctx->device, &bci, NULL, out_buf) != VK_SUCCESS)
      return false;

   vkGetBufferMemoryRequirements(ctx->device, *out_buf, &req);

   mai.allocationSize  = req.size;
   mai.memoryTypeIndex = vulkan_find_memory_type(ctx->mem_props,
         req.memoryTypeBits,
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

   if (vkAllocateMemory(ctx->device, &mai, NULL, out_mem) != VK_SUCCESS)
   {
      vkDestroyBuffer(ctx->device, *out_buf, NULL);
      *out_buf = VK_NULL_HANDLE;
      return false;
   }
   if (vkBindBufferMemory(ctx->device, *out_buf, *out_mem, 0) != VK_SUCCESS)
   {
      vkFreeMemory(ctx->device, *out_mem, NULL);
      vkDestroyBuffer(ctx->device, *out_buf, NULL);
      *out_buf = VK_NULL_HANDLE;
      *out_mem = VK_NULL_HANDLE;
      return false;
   }
   if (vkMapMemory(ctx->device, *out_mem, 0, size, 0, out_map) != VK_SUCCESS)
   {
      vkFreeMemory(ctx->device, *out_mem, NULL);
      vkDestroyBuffer(ctx->device, *out_buf, NULL);
      *out_buf = VK_NULL_HANDLE;
      *out_mem = VK_NULL_HANDLE;
      return false;
   }
   return true;
}

static void free_staging(VkDevice device, pass_image_t *p)
{
   if (p->mapped)
   {
      vkUnmapMemory(device, p->memory);
      p->mapped = NULL;
   }
   if (p->staging)   vkDestroyBuffer(device, p->staging, NULL);
   if (p->memory)    vkFreeMemory(device, p->memory, NULL);
   p->staging = VK_NULL_HANDLE;
   p->memory  = VK_NULL_HANDLE;
   free(p->out_filename);
   p->out_filename = NULL;
}

static const char *vk_format_name(VkFormat f)
{
   switch (f)
   {
      /* 1-channel */
      case VK_FORMAT_R8_UNORM:                 return "VK_FORMAT_R8_UNORM";
      case VK_FORMAT_R8_UINT:                  return "VK_FORMAT_R8_UINT";
      case VK_FORMAT_R8_SINT:                  return "VK_FORMAT_R8_SINT";
      case VK_FORMAT_R16_UINT:                 return "VK_FORMAT_R16_UINT";
      case VK_FORMAT_R16_SINT:                 return "VK_FORMAT_R16_SINT";
      case VK_FORMAT_R16_SFLOAT:               return "VK_FORMAT_R16_SFLOAT";
      case VK_FORMAT_R32_UINT:                 return "VK_FORMAT_R32_UINT";
      case VK_FORMAT_R32_SINT:                 return "VK_FORMAT_R32_SINT";
      case VK_FORMAT_R32_SFLOAT:               return "VK_FORMAT_R32_SFLOAT";

      /* 2-channel */
      case VK_FORMAT_R8G8_UNORM:               return "VK_FORMAT_R8G8_UNORM";
      case VK_FORMAT_R8G8_UINT:                return "VK_FORMAT_R8G8_UINT";
      case VK_FORMAT_R8G8_SINT:                return "VK_FORMAT_R8G8_SINT";
      case VK_FORMAT_R16G16_UINT:              return "VK_FORMAT_R16G16_UINT";
      case VK_FORMAT_R16G16_SINT:              return "VK_FORMAT_R16G16_SINT";
      case VK_FORMAT_R16G16_SFLOAT:            return "VK_FORMAT_R16G16_SFLOAT";
      case VK_FORMAT_R32G32_UINT:              return "VK_FORMAT_R32G32_UINT";
      case VK_FORMAT_R32G32_SINT:              return "VK_FORMAT_R32G32_SINT";
      case VK_FORMAT_R32G32_SFLOAT:            return "VK_FORMAT_R32G32_SFLOAT";

      /* 4-channel */
      case VK_FORMAT_R8G8B8A8_UNORM:           return "VK_FORMAT_R8G8B8A8_UNORM";
      case VK_FORMAT_R8G8B8A8_UINT:            return "VK_FORMAT_R8G8B8A8_UINT";
      case VK_FORMAT_R8G8B8A8_SINT:            return "VK_FORMAT_R8G8B8A8_SINT";
      case VK_FORMAT_R8G8B8A8_SRGB:            return "VK_FORMAT_R8G8B8A8_SRGB";
      case VK_FORMAT_B8G8R8A8_UNORM:           return "VK_FORMAT_B8G8R8A8_UNORM";
      case VK_FORMAT_B8G8R8A8_SRGB:            return "VK_FORMAT_B8G8R8A8_SRGB";
      case VK_FORMAT_R16G16B16A16_UINT:        return "VK_FORMAT_R16G16B16A16_UINT";
      case VK_FORMAT_R16G16B16A16_SINT:        return "VK_FORMAT_R16G16B16A16_SINT";
      case VK_FORMAT_R16G16B16A16_UNORM:       return "VK_FORMAT_R16G16B16A16_UNORM";
      case VK_FORMAT_R16G16B16A16_SFLOAT:      return "VK_FORMAT_R16G16B16A16_SFLOAT";
      case VK_FORMAT_R32G32B32A32_UINT:        return "VK_FORMAT_R32G32B32A32_UINT";
      case VK_FORMAT_R32G32B32A32_SINT:        return "VK_FORMAT_R32G32B32A32_SINT";
      case VK_FORMAT_R32G32B32A32_SFLOAT:      return "VK_FORMAT_R32G32B32A32_SFLOAT";

      /* Packed */
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
      case VK_FORMAT_A2B10G10R10_UINT_PACK32:  return "VK_FORMAT_A2B10G10R10_UINT_PACK32";
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32:  return "VK_FORMAT_B10G11R11_UFLOAT_PACK32";

      default:                                  return "VkFormat(unknown)";
   }
}

vulkan_pass_dump_t *vulkan_pass_dump_arm(
      const vulkan_pass_dump_ctx_t *ctx,
      vulkan_filter_chain_t *chain,
      const char *screenshot_dir,
      const char *core_short_name,
      const char *preset_path)
{
   unsigned i;
   unsigned offscreen;
   vulkan_pass_dump_t *d;
   char timestamp[32];
   char subdir[256];
   char base[2048];

   if (!ctx || !chain || !screenshot_dir)
   {
      RARCH_WARN("[Pass Dump] Invalid arguments.\n");
      return NULL;
   }

   offscreen = vulkan_filter_chain_get_pass_count(chain);
   if (offscreen == 0)
   {
      RARCH_WARN("[Pass Dump] No active slang preset.\n");
      return NULL;
   }
   /* Last pass is the "final" pass; count it separately via record_final. */
   offscreen -= 1;

   if (vulkan_filter_chain_get_original_image(chain) == VK_NULL_HANDLE)
   {
      RARCH_WARN("[Pass Dump] No Original image bound on chain.\n");
      return NULL;
   }

   d = (vulkan_pass_dump_t*)calloc(1, sizeof(*d));
   if (!d)
      return NULL;
   d->ctx              = *ctx;
   d->offscreen_count  = offscreen;
   d->num_images       = 1 + offscreen + 1;     /* Original + offscreen + final */
   d->frame_count      = ctx->frame_count;
   format_iso_time(d->capture_time_iso, sizeof d->capture_time_iso);

   /* Build output directory path. */
   format_filename_timestamp(timestamp, sizeof timestamp);
   snprintf(subdir, sizeof subdir, "%s_%s",
         (core_short_name && *core_short_name) ? core_short_name : "nocore",
         timestamp);
   fill_pathname_join_special(base, screenshot_dir, "slang_dump", sizeof base);
   if (!path_mkdir(base))
   {
      RARCH_WARN("[Pass Dump] Failed to create %s\n", base);
      free(d);
      return NULL;
   }
   fill_pathname_join_special(d->out_dir, base, subdir, sizeof d->out_dir);
   if (!path_mkdir(d->out_dir))
   {
      RARCH_WARN("[Pass Dump] Failed to create %s\n", d->out_dir);
      free(d);
      return NULL;
   }

   if (preset_path)
      strlcpy(d->preset_path, preset_path, sizeof d->preset_path);

   d->images = (pass_image_t*)calloc(d->num_images, sizeof(pass_image_t));
   if (!d->images)
   {
      free(d);
      return NULL;
   }

   /* Slot 0 = Original. */
   {
      pass_image_t *p = &d->images[0];
      size_t bpt;
      p->src_image    = vulkan_filter_chain_get_original_image(chain);
      p->format       = vulkan_filter_chain_get_original_format(chain);
      p->extent       = vulkan_filter_chain_get_original_extent(chain);
      p->prev_layout  = vulkan_filter_chain_get_original_layout(chain);
      p->pass_index   = -1;

      /* The chain sets input_texture.format = VK_FORMAT_UNDEFINED for
       * software-rendered cores (the chain doesn't use the field; the
       * underlying texture carries the format internally).  Caller passes
       * the real format via ctx->original_format_hint so the KTX2 header
       * records the actual format. */
      if (p->format == VK_FORMAT_UNDEFINED
            && ctx->original_format_hint != VK_FORMAT_UNDEFINED)
      {
         RARCH_LOG("[Pass Dump] Chain returned UNDEFINED for Original format; using hint %d.\n",
               (int)ctx->original_format_hint);
         p->format = ctx->original_format_hint;
      }

      bpt = ktx2_bytes_per_texel(p->format);
      if (bpt == 0)
      {
         RARCH_WARN("[Pass Dump] Unknown Original VkFormat %d; assuming 4 bpt.\n", (int)p->format);
         bpt = 4;  /* best-effort */
      }
      p->staging_size = (VkDeviceSize)p->extent.width * p->extent.height * bpt;
      if (!alloc_staging(ctx, p->staging_size, &p->staging, &p->memory, &p->mapped))
      {
         RARCH_WARN("[Pass Dump] Original staging alloc failed.\n");
         goto fail;
      }

      p->out_filename = (char*)malloc(32);
      if (!p->out_filename)
         goto fail;
      snprintf(p->out_filename, 32, "00_original.ktx2");
      strlcpy(p->display_name, "original", sizeof p->display_name);
      p->source_shader[0] = '\0';   /* Original has no shader source */
   }

   /* Slots 1..offscreen_count = offscreen passes. */
   for (i = 0; i < offscreen; i++)
   {
      pass_image_t *p = &d->images[1 + i];
      size_t bpt;
      const char *raw;

      p->src_image    = vulkan_filter_chain_get_pass_image(chain, i);
      p->format       = vulkan_filter_chain_get_pass_format(chain, i);
      p->extent       = vulkan_filter_chain_get_pass_extent(chain, i);
      p->prev_layout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      p->pass_index   = (int)i;

      bpt = ktx2_bytes_per_texel(p->format);
      if (bpt == 0) bpt = 8;   /* worst-case among slang formats — over-allocates safely */
      p->staging_size = (VkDeviceSize)p->extent.width * p->extent.height * bpt;
      if (!alloc_staging(ctx, p->staging_size, &p->staging, &p->memory, &p->mapped))
      {
         RARCH_WARN("[Pass Dump] Pass %u staging alloc failed.\n", i);
         goto fail;
      }

      raw = vulkan_filter_chain_get_pass_name(chain, i);
      strlcpy(p->source_shader, raw ? raw : "", sizeof p->source_shader);
      sanitize_pass_name(raw, p->display_name, sizeof p->display_name);
      p->out_filename = (char*)malloc(96);
      if (!p->out_filename)
         goto fail;
      snprintf(p->out_filename, 96, "%02u_pass%02u_%s.ktx2", 1u + i, i, p->display_name);
   }

   /* Slot num_images-1 = final. Staging allocated lazily in record_final
    * once the swapchain image/format/extent is known. */

   return d;

fail:
   vulkan_pass_dump_free(d);
   return NULL;
}

void vulkan_pass_dump_free(vulkan_pass_dump_t *d)
{
   unsigned i;
   if (!d) return;
   if (d->images)
   {
      for (i = 0; i < d->num_images; i++)
         free_staging(d->ctx.device, &d->images[i]);
      free(d->images);
   }
   free(d);
}

static void record_one_image_to_staging(
      VkCommandBuffer cmd,
      const pass_image_t *p,
      VkImageLayout src_layout_in,
      VkImageLayout dst_layout_out,
      VkImageAspectFlags aspect)
{
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
   VkBufferImageCopy r = {0};

   /* Transition: src_layout_in -> TRANSFER_SRC_OPTIMAL */
   b.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT
                                       | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   b.dstAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;
   b.oldLayout                       = src_layout_in;
   b.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
   b.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
   b.image                           = p->src_image;
   b.subresourceRange.aspectMask     = aspect;
   b.subresourceRange.baseMipLevel   = 0;
   b.subresourceRange.levelCount     = 1;
   b.subresourceRange.baseArrayLayer = 0;
   b.subresourceRange.layerCount     = 1;
   vkCmdPipelineBarrier(cmd,
         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT,
         0, 0, NULL, 0, NULL, 1, &b);

   /* Copy. */
   r.bufferOffset                    = 0;
   r.bufferRowLength                 = 0;  /* tight rows */
   r.bufferImageHeight               = 0;
   r.imageSubresource.aspectMask     = aspect;
   r.imageSubresource.mipLevel       = 0;
   r.imageSubresource.baseArrayLayer = 0;
   r.imageSubresource.layerCount     = 1;
   r.imageOffset.x                   = 0;
   r.imageOffset.y                   = 0;
   r.imageOffset.z                   = 0;
   r.imageExtent.width               = p->extent.width;
   r.imageExtent.height              = p->extent.height;
   r.imageExtent.depth               = 1;
   vkCmdCopyImageToBuffer(cmd,
         p->src_image,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         p->staging,
         1, &r);

   /* Restore: TRANSFER_SRC_OPTIMAL -> dst_layout_out */
   b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                     | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.newLayout     = dst_layout_out;
   vkCmdPipelineBarrier(cmd,
         VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         0, 0, NULL, 0, NULL, 1, &b);
}

void vulkan_pass_dump_record_offscreen(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      vulkan_filter_chain_t *chain)
{
   unsigned i;
   unsigned now_offscreen;

   if (!dump || dump->aborted || dump->offscreen_recorded || !chain)
      return;

   /* Sanity: pass count must match what we armed with. */
   now_offscreen = vulkan_filter_chain_get_pass_count(chain);
   if (now_offscreen >= 1) now_offscreen -= 1;
   if (now_offscreen != dump->offscreen_count)
   {
      RARCH_WARN("[Pass Dump] Pass count changed (%u -> %u) since arm; aborting.\n",
            dump->offscreen_count, now_offscreen);
      dump->aborted = true;
      return;
   }

   /* Original (slot 0). */
   {
      pass_image_t *p = &dump->images[0];
      record_one_image_to_staging(cmd, p,
            p->prev_layout,
            p->prev_layout,
            VK_IMAGE_ASPECT_COLOR_BIT);
      p->recorded = true;
   }

   /* Offscreen passes (slots 1..offscreen_count). */
   for (i = 0; i < dump->offscreen_count; i++)
   {
      pass_image_t *p = &dump->images[1 + i];
      record_one_image_to_staging(cmd, p,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);
      p->recorded = true;
   }

   dump->offscreen_recorded = true;
}

void vulkan_pass_dump_record_final(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      vulkan_filter_chain_t *chain,
      VkImage swapchain_image,
      VkFormat swapchain_format,
      VkExtent2D swapchain_extent)
{
   pass_image_t *p;
   size_t bpt;
   const char *raw;

   if (!dump || dump->aborted || dump->final_recorded || !dump->offscreen_recorded)
      return;

   p = &dump->images[dump->num_images - 1];

   bpt = ktx2_bytes_per_texel(swapchain_format);
   if (bpt == 0) bpt = 8;   /* defensive overallocation */
   p->src_image    = swapchain_image;
   p->format       = swapchain_format;
   p->extent       = swapchain_extent;
   p->prev_layout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   p->pass_index   = (int)dump->offscreen_count;
   p->staging_size = (VkDeviceSize)swapchain_extent.width
                     * swapchain_extent.height * bpt;

   if (!alloc_staging(&dump->ctx, p->staging_size,
            &p->staging, &p->memory, &p->mapped))
   {
      RARCH_WARN("[Pass Dump] Final-pass staging alloc failed; aborting.\n");
      dump->aborted = true;
      return;
   }

   /* Resolve final-pass name via the chain (T13). */
   raw = chain
      ? vulkan_filter_chain_get_pass_name(chain, dump->offscreen_count)
      : "";
   strlcpy(p->source_shader, raw ? raw : "", sizeof p->source_shader);
   sanitize_pass_name(raw, p->display_name, sizeof p->display_name);

   p->out_filename = (char*)malloc(96);
   if (!p->out_filename)
   {
      RARCH_WARN("[Pass Dump] Final-pass filename alloc failed; aborting.\n");
      dump->aborted = true;
      return;
   }
   snprintf(p->out_filename, 96, "%02u_pass%02u_%s_final.ktx2",
         dump->num_images - 1u, dump->offscreen_count, p->display_name);

   record_one_image_to_staging(cmd, p,
         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_IMAGE_ASPECT_COLOR_BIT);
   p->recorded = true;

   /* Store canonical format name for manifest. */
   strlcpy(dump->swapchain_format_name, vk_format_name(swapchain_format),
         sizeof dump->swapchain_format_name);

   dump->final_recorded = true;
}

static void compose_kvd_for(const vulkan_pass_dump_t *d, const pass_image_t *p,
      char *json_out, size_t json_size)
{
   snprintf(json_out, json_size,
         "{\"pass\":%d,\"name\":\"%s\",\"source_shader\":%s%s%s,"
         "\"input_extent\":[%u,%u],\"output_extent\":[%u,%u],"
         "\"frame_index\":%llu,\"capture_time\":\"%s\"}",
         p->pass_index,
         p->display_name[0] ? p->display_name : "unnamed",
         p->source_shader[0] ? "\"" : "",
         p->source_shader[0] ? p->source_shader : "null",
         p->source_shader[0] ? "\"" : "",
         d->images[0].extent.width, d->images[0].extent.height,
         p->extent.width, p->extent.height,
         (unsigned long long)d->frame_count,
         d->capture_time_iso);
}

static bool write_one_ktx2(const vulkan_pass_dump_t *d, const pass_image_t *p)
{
   char  path[2048];
   char  kvd_json[512];
   ktx2_kv_pair_t kvs[2];
   ktx2_write_params_t wp;

   fill_pathname_join_special(path, d->out_dir, p->out_filename, sizeof path);

   compose_kvd_for(d, p, kvd_json, sizeof kvd_json);

   kvs[0].key       = "KTXorientation";
   kvs[0].value     = "rd";
   kvs[0].value_len = 3;   /* "rd" + trailing \0 */
   kvs[1].key       = "RAretroarch";
   kvs[1].value     = kvd_json;
   kvs[1].value_len = strlen(kvd_json) + 1;

   memset(&wp, 0, sizeof wp);
   wp.vk_format     = p->format;
   wp.width         = p->extent.width;
   wp.height        = p->extent.height;
   wp.pixels        = p->mapped;
   wp.pixels_size   = (size_t)p->staging_size;
   wp.kv_pairs      = kvs;
   wp.kv_pair_count = 2;
   return ktx2_write_file(path, &wp);
}

/* Escape a string for inclusion in JSON.  Writes at most cap-1 bytes plus
 * terminator to out.  Handles '"', '\', and control characters; passes
 * everything else through as-is. */
static void json_escape(const char *in, char *out, size_t cap)
{
   size_t j = 0;
   if (!in) in = "";
   for (size_t i = 0; in[i] && j + 2 < cap; i++)
   {
      unsigned char c = (unsigned char)in[i];
      if (c == '"' || c == '\\')
      {
         out[j++] = '\\';
         out[j++] = c;
      }
      else if (c < 0x20)
      {
         static const char hex[] = "0123456789ABCDEF";
         if (j + 6 >= cap) break;
         out[j++] = '\\'; out[j++] = 'u';
         out[j++] = '0';  out[j++] = '0';
         out[j++] = hex[(c >> 4) & 0xF];
         out[j++] = hex[c & 0xF];
      }
      else
         out[j++] = (char)c;
   }
   out[j] = '\0';
}

static void write_manifest(const vulkan_pass_dump_t *d)
{
   char path[2048];
   char preset_escaped[4096];
   FILE *f;
   unsigned i;

   json_escape(d->preset_path, preset_escaped, sizeof preset_escaped);

   fill_pathname_join_special(path, d->out_dir, "manifest.json", sizeof path);
   f = fopen(path, "wb");
   if (!f)
   {
      RARCH_WARN("[Pass Dump] Could not write %s\n", path);
      return;
   }

   fprintf(f,
         "{\n"
         "  \"schema\": 1,\n"
         "  \"video_driver\": \"vulkan\",\n"
         "  \"retroarch_version\": \"%s\",\n"
         "  \"retroarch_sha\": \"%s\",\n"
         "  \"preset_path\": \"%s\",\n"
         "  \"num_passes\": %u,\n"
         "  \"swapchain_format\": \"%s\",\n"
         "  \"frame_index\": %llu,\n"
         "  \"capture_time\": \"%s\",\n"
         "  \"files\": [\n",
         PACKAGE_VERSION,
#ifdef HAVE_GIT_VERSION
         retroarch_git_version,
#else
         "",
#endif
         preset_escaped[0] ? preset_escaped : "",
         d->offscreen_count + 1u,
         d->swapchain_format_name[0] ? d->swapchain_format_name : "VkFormat(unknown)",
         (unsigned long long)d->frame_count,
         d->capture_time_iso);

   for (i = 0; i < d->num_images; i++)
   {
      const pass_image_t *p = &d->images[i];
      fprintf(f,
            "    {\"path\": \"%s\", \"pass\": %d, \"extent\": [%u, %u], \"vk_format\": \"%s\"}%s\n",
            p->out_filename ? p->out_filename : "(unknown)",
            p->pass_index,
            p->extent.width, p->extent.height,
            vk_format_name(p->format),
            (i + 1 == d->num_images) ? "" : ",");
   }

   fprintf(f, "  ]\n}\n");
   fclose(f);
}

void vulkan_pass_dump_flush(vulkan_pass_dump_t *d)
{
   unsigned i;
   unsigned n_ranges = 0;
   VkMappedMemoryRange ranges[64];

   if (!d) return;

   if (d->aborted || !d->offscreen_recorded)
   {
      RARCH_WARN("[Pass Dump] Discarding aborted or never-recorded dump.\n");
      vulkan_pass_dump_free(d);
      return;
   }

   /* HOST_COHERENT memory, but call invalidate for portability. */
   for (i = 0; i < d->num_images && n_ranges < 64; i++)
   {
      VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
      if (!d->images[i].memory) continue;
      r.memory = d->images[i].memory;
      r.offset = 0;
      r.size   = VK_WHOLE_SIZE;
      ranges[n_ranges++] = r;
   }
   if (n_ranges > 0)
      vkInvalidateMappedMemoryRanges(d->ctx.device, n_ranges, ranges);

   /* Write KTX2 files in slot order: Original, then offscreen 0..N-2, then final. */
   for (i = 0; i < d->num_images; i++)
   {
      const pass_image_t *p = &d->images[i];
      if (!p->recorded || !p->mapped) continue;
      if (!write_one_ktx2(d, p))
         RARCH_WARN("[Pass Dump] Failed to write %s\n",
               p->out_filename ? p->out_filename : "(unknown)");
   }

   write_manifest(d);

   RARCH_LOG("[Pass Dump] Wrote %u files to %s\n", d->num_images, d->out_dir);

   vulkan_pass_dump_free(d);
}
