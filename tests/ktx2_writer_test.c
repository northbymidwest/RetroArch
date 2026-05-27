/* tests/ktx2_writer_test.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../gfx/drivers/ktx2_writer.h"

static int failures = 0;

#define CHECK(cond) do { \
   if (!(cond)) { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
   } \
} while (0)

static void test_bytes_per_texel(void)
{
   /* existing — must still pass */
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R8G8B8A8_UNORM)         == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R8G8B8A8_SRGB)          == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_B8G8R8A8_UNORM)         == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_B8G8R8A8_SRGB)          == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R16G16B16A16_UNORM)     == 8);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R16G16B16A16_SFLOAT)    == 8);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_A2B10G10R10_UNORM_PACK32) == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_B10G11R11_UFLOAT_PACK32)  == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_UNDEFINED) == 0);

   /* new coverage */
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R8_UNORM)               == 1);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R8_UINT)                == 1);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R8G8_UNORM)             == 2);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R16_SFLOAT)             == 2);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R16_UINT)               == 2);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R8G8B8A8_UINT)          == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R8G8B8A8_SINT)          == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_A2B10G10R10_UINT_PACK32) == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R16G16_SFLOAT)          == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R32_SFLOAT)             == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R32_UINT)               == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R16G16B16A16_UINT)      == 8);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R16G16B16A16_SINT)      == 8);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R32G32_SFLOAT)          == 8);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R32G32B32A32_SFLOAT)    == 16);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R32G32B32A32_UINT)      == 16);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R32G32B32A32_SINT)      == 16);
}

static void test_identifier_bytes(void)
{
   /* KTX 2.0 spec §3.1 — fixed identifier */
   static const uint8_t expected[12] = {
      0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
      0x0D, 0x0A, 0x1A, 0x0A
   };
   CHECK(memcmp(ktx2_identifier, expected, 12) == 0);
}

int main(void)
{
   test_bytes_per_texel();
   test_identifier_bytes();
   if (failures)
   {
      fprintf(stderr, "%d test(s) failed\n", failures);
      return 1;
   }
   printf("all tests passed\n");
   return 0;
}
