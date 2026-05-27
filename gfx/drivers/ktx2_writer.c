/* gfx/drivers/ktx2_writer.c */

#include "ktx2_writer.h"

#include <stdio.h>
#include <string.h>

#ifdef RARCH_INTERNAL
#include "../../verbosity.h"
#else
/* When compiled standalone (for the unit test), provide a no-op
 * RARCH_WARN macro so we don't pull RetroArch's logging infra. */
#define RARCH_WARN(...) ((void)0)
#endif

const uint8_t ktx2_identifier[12] = {
   0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
   0x0D, 0x0A, 0x1A, 0x0A
};

/* KTX 2.0 header layout, on disk all little-endian uint32 / uint64.
 * Identifier (12 bytes) is written separately; this struct covers the
 * 80 bytes that follow it (including the 16-byte sgd offset/length). */
typedef struct
{
   uint32_t vk_format;
   uint32_t type_size;
   uint32_t pixel_width;
   uint32_t pixel_height;
   uint32_t pixel_depth;
   uint32_t layer_count;
   uint32_t face_count;
   uint32_t level_count;
   uint32_t supercompression_scheme;
   uint32_t dfd_byte_offset;
   uint32_t dfd_byte_length;
   uint32_t kvd_byte_offset;
   uint32_t kvd_byte_length;
   uint64_t sgd_byte_offset;
   uint64_t sgd_byte_length;
} ktx2_header_t;

#define KTX2_HEADER_SIZE        80   /* identifier excluded */
#define KTX2_LEVEL_INDEX_SIZE   24   /* per level */

static uint32_t ktx2_type_size_for(VkFormat fmt)
{
   /* typeSize is the size of one "type" used to pack one channel for
    * non-packed formats; for packed/compressed formats, set to 1. */
   switch (fmt)
   {
      case VK_FORMAT_R8_UNORM:
      case VK_FORMAT_R8_UINT:
      case VK_FORMAT_R8_SINT:
      case VK_FORMAT_R8G8_UNORM:
      case VK_FORMAT_R8G8_UINT:
      case VK_FORMAT_R8G8_SINT:
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_UINT:
      case VK_FORMAT_R8G8B8A8_SINT:
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_SRGB:
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      case VK_FORMAT_A2B10G10R10_UINT_PACK32:
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
         return 1;
      case VK_FORMAT_R16_UINT:
      case VK_FORMAT_R16_SINT:
      case VK_FORMAT_R16_SFLOAT:
      case VK_FORMAT_R16G16_UINT:
      case VK_FORMAT_R16G16_SINT:
      case VK_FORMAT_R16G16_SFLOAT:
      case VK_FORMAT_R16G16B16A16_UINT:
      case VK_FORMAT_R16G16B16A16_SINT:
      case VK_FORMAT_R16G16B16A16_UNORM:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
         return 2;
      case VK_FORMAT_R32_UINT:
      case VK_FORMAT_R32_SINT:
      case VK_FORMAT_R32_SFLOAT:
      case VK_FORMAT_R32G32_UINT:
      case VK_FORMAT_R32G32_SINT:
      case VK_FORMAT_R32G32_SFLOAT:
      case VK_FORMAT_R32G32B32A32_UINT:
      case VK_FORMAT_R32G32B32A32_SINT:
      case VK_FORMAT_R32G32B32A32_SFLOAT:
         return 4;
      default:
         return 1;
   }
}

size_t ktx2_bytes_per_texel(VkFormat fmt)
{
   switch (fmt)
   {
      case VK_FORMAT_R8_UNORM:
      case VK_FORMAT_R8_UINT:
      case VK_FORMAT_R8_SINT:
         return 1;

      case VK_FORMAT_R8G8_UNORM:
      case VK_FORMAT_R8G8_UINT:
      case VK_FORMAT_R8G8_SINT:
      case VK_FORMAT_R16_UINT:
      case VK_FORMAT_R16_SINT:
      case VK_FORMAT_R16_SFLOAT:
         return 2;

      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_UINT:
      case VK_FORMAT_R8G8B8A8_SINT:
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_SRGB:
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      case VK_FORMAT_A2B10G10R10_UINT_PACK32:
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
      case VK_FORMAT_R16G16_UINT:
      case VK_FORMAT_R16G16_SINT:
      case VK_FORMAT_R16G16_SFLOAT:
      case VK_FORMAT_R32_UINT:
      case VK_FORMAT_R32_SINT:
      case VK_FORMAT_R32_SFLOAT:
         return 4;

      case VK_FORMAT_R16G16B16A16_UINT:
      case VK_FORMAT_R16G16B16A16_SINT:
      case VK_FORMAT_R16G16B16A16_UNORM:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R32G32_UINT:
      case VK_FORMAT_R32G32_SINT:
      case VK_FORMAT_R32G32_SFLOAT:
         return 8;

      case VK_FORMAT_R32G32B32A32_UINT:
      case VK_FORMAT_R32G32B32A32_SINT:
      case VK_FORMAT_R32G32B32A32_SFLOAT:
         return 16;

      default:
         return 0;
   }
}

bool ktx2_write_file(const char *out_path, const ktx2_write_params_t *p)
{
   FILE *f;
   ktx2_header_t hdr;

   if (!out_path || !p)
      return false;

   f = fopen(out_path, "wb");
   if (!f)
   {
      RARCH_WARN("[KTX2] Failed to open %s for writing.\n", out_path);
      return false;
   }

   if (fwrite(ktx2_identifier, 1, 12, f) != 12)
      goto io_fail;

   memset(&hdr, 0, sizeof hdr);
   hdr.vk_format               = p->vk_format;
   hdr.type_size               = ktx2_type_size_for(p->vk_format);
   hdr.pixel_width             = p->width;
   hdr.pixel_height            = p->height;
   hdr.pixel_depth             = 0;
   hdr.layer_count             = 0;
   hdr.face_count              = 1;
   hdr.level_count             = 1;
   hdr.supercompression_scheme = 0;
   /* Offsets/lengths for DFD/KVD/SGD: filled in Tasks 4-6. For now zero
    * so the header bytes are deterministic and the structural test passes. */

   if (fwrite(&hdr, 1, KTX2_HEADER_SIZE, f) != KTX2_HEADER_SIZE)
      goto io_fail;

   fclose(f);
   return true;

io_fail:
   RARCH_WARN("[KTX2] Write failure on %s.\n", out_path);
   fclose(f);
   remove(out_path);
   return false;
}
