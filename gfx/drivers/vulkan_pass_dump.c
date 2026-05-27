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
   char           *out_filename;    /* sanitized, basename only */
   char            display_name[64];/* short name for manifest/KVD JSON */
   int             pass_index;      /* -1 for Original, num_passes-1 for final */
   bool            recorded;        /* set when blit emitted onto cmd */
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

      bpt = ktx2_bytes_per_texel(p->format);
      if (bpt == 0)
      {
         RARCH_WARN("[Pass Dump] Unknown Original VkFormat %d.\n", (int)p->format);
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
      vulkan_pass_dump_t *dump, VkCommandBuffer cmd, vulkan_filter_chain_t *chain,
      VkImage img, VkFormat fmt, VkExtent2D ext)
{
   (void)dump; (void)cmd; (void)chain; (void)img; (void)fmt; (void)ext;
}

void vulkan_pass_dump_flush(vulkan_pass_dump_t *dump)
{
   /* T9: just free; real flush lands in T12. */
   vulkan_pass_dump_free(dump);
}
