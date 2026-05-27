/* gfx/drivers/ktx2_writer.c */

#include "ktx2_writer.h"

#include <stdio.h>
#include <string.h>

/* #include "../../verbosity.h" will be added when logging is implemented in Task 2+ */

const uint8_t ktx2_identifier[12] = {
   0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
   0x0D, 0x0A, 0x1A, 0x0A
};

size_t ktx2_bytes_per_texel(VkFormat fmt)
{
   switch (fmt)
   {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_SRGB:
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
         return 4;
      case VK_FORMAT_R16G16B16A16_UNORM:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
         return 8;
      default:
         return 0;
   }
}

bool ktx2_write_file(const char *out_path, const ktx2_write_params_t *p)
{
   (void)out_path; (void)p;
   return false;  /* filled in subsequent tasks */
}
