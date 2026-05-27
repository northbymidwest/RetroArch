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

static void test_header_layout(void)
{
   const char *path = "/tmp/ktx2_test_header.ktx2";
   uint8_t pixels[4 * 4 * 4];
   memset(pixels, 0x55, sizeof pixels);

   ktx2_write_params_t p = {
      .vk_format     = VK_FORMAT_R8G8B8A8_UNORM,
      .width         = 4,
      .height        = 4,
      .pixels        = pixels,
      .pixels_size   = sizeof pixels,
      .kv_pairs      = NULL,
      .kv_pair_count = 0,
   };
   CHECK(ktx2_write_file(path, &p));

   FILE *f = fopen(path, "rb");
   CHECK(f != NULL);
   if (!f) return;

   /* File should be exactly 12 (identifier) + 68 (header) = 80 bytes;
    * DFD/KVD/image-data come in later tasks. */
   fseek(f, 0, SEEK_END);
   long file_size = ftell(f);
   CHECK(file_size == 80);
   fseek(f, 0, SEEK_SET);

   uint8_t id[12];
   CHECK(fread(id, 1, 12, f) == 12);
   CHECK(memcmp(id, ktx2_identifier, 12) == 0);

   uint32_t hdr32[13];
   CHECK(fread(hdr32, sizeof(uint32_t), 13, f) == 13);
   CHECK(hdr32[0]  == VK_FORMAT_R8G8B8A8_UNORM);   /* vkFormat */
   CHECK(hdr32[1]  == 1);                          /* typeSize */
   CHECK(hdr32[2]  == 4);                          /* pixelWidth */
   CHECK(hdr32[3]  == 4);                          /* pixelHeight */
   CHECK(hdr32[4]  == 0);                          /* pixelDepth */
   CHECK(hdr32[5]  == 0);                          /* layerCount */
   CHECK(hdr32[6]  == 1);                          /* faceCount */
   CHECK(hdr32[7]  == 1);                          /* levelCount */
   CHECK(hdr32[8]  == 0);                          /* supercompressionScheme */
   CHECK(hdr32[9]  == 0);                          /* dfdByteOffset (T4) */
   CHECK(hdr32[10] == 0);                          /* dfdByteLength */
   CHECK(hdr32[11] == 0);                          /* kvdByteOffset */
   CHECK(hdr32[12] == 0);                          /* kvdByteLength */

   uint64_t hdr64[2];
   CHECK(fread(hdr64, sizeof(uint64_t), 2, f) == 2);
   CHECK(hdr64[0] == 0);                           /* sgdByteOffset */
   CHECK(hdr64[1] == 0);                           /* sgdByteLength */

   fclose(f);
   remove(path);
}

int main(void)
{
   test_header_layout();
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
