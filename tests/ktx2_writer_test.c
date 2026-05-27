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

   /* File must be larger than 80 bytes now that T4 emits DFD +
    * level index + image data. */
   fseek(f, 0, SEEK_END);
   long file_size = ftell(f);
   CHECK(file_size > 80);
   fseek(f, 0, SEEK_SET);

   uint8_t id[12];
   CHECK(fread(id, 1, 12, f) == 12);
   CHECK(memcmp(id, ktx2_identifier, 12) == 0);

   uint32_t hdr32[13];
   CHECK(fread(hdr32, sizeof(uint32_t), 13, f) == 13);
   CHECK(hdr32[0]  == VK_FORMAT_R8G8B8A8_UNORM);   /* vkFormat */
   CHECK(hdr32[1]  == 1);                           /* typeSize */
   CHECK(hdr32[2]  == 4);                           /* pixelWidth */
   CHECK(hdr32[3]  == 4);                           /* pixelHeight */
   CHECK(hdr32[4]  == 0);                           /* pixelDepth */
   CHECK(hdr32[5]  == 0);                           /* layerCount */
   CHECK(hdr32[6]  == 1);                           /* faceCount */
   CHECK(hdr32[7]  == 1);                           /* levelCount */
   CHECK(hdr32[8]  == 0);                           /* supercompressionScheme */
   CHECK(hdr32[9]  == 80 + 24);                     /* dfdByteOffset: after level index */
   CHECK(hdr32[10]  > 0);                           /* dfdByteLength: non-zero */
   CHECK(hdr32[11] == 0);                           /* kvdByteOffset (T5 fills) */
   CHECK(hdr32[12] == 0);                           /* kvdByteLength */

   uint64_t hdr64[2];
   CHECK(fread(hdr64, sizeof(uint64_t), 2, f) == 2);
   CHECK(hdr64[0] == 0);                            /* sgdByteOffset */
   CHECK(hdr64[1] == 0);                            /* sgdByteLength */

   fclose(f);
   remove(path);
}

static void test_level_index_and_dfd(void)
{
   const char *path = "/tmp/ktx2_test_dfd.ktx2";
   uint8_t pixels[4 * 4 * 4];
   memset(pixels, 0xAA, sizeof pixels);

   ktx2_write_params_t p = {
      .vk_format     = VK_FORMAT_R8G8B8A8_UNORM,
      .width         = 4,
      .height        = 4,
      .pixels        = pixels,
      .pixels_size   = sizeof pixels,
   };
   CHECK(ktx2_write_file(path, &p));

   FILE *f = fopen(path, "rb");
   CHECK(f != NULL); if (!f) return;

   /* Skip identifier (12 bytes). Read full 68-byte header. */
   fseek(f, 12, SEEK_SET);
   uint32_t hdr32[13];
   CHECK(fread(hdr32, 4, 13, f) == 13);
   uint64_t hdr64[2];
   CHECK(fread(hdr64, 8, 2, f) == 2);

   uint32_t dfd_off = hdr32[9];
   uint32_t dfd_len = hdr32[10];
   uint32_t kvd_off = hdr32[11];
   uint32_t kvd_len = hdr32[12];
   uint64_t sgd_off = hdr64[0];
   uint64_t sgd_len = hdr64[1];

   /* DFD must start immediately after level index (one entry of 24 bytes).
    * level index begins at file offset 12 + 68 = 80; DFD follows the
    * 24-byte level index entry, so dfd_off must be 80 + 24 = 104. */
   CHECK(dfd_off == 80 + 24);
   CHECK(dfd_len  > 0);
   CHECK(sgd_off == 0 && sgd_len == 0);
   (void)kvd_off; (void)kvd_len;

   /* Read level index entry. */
   fseek(f, 12 + 68, SEEK_SET);
   uint64_t lvl_off, lvl_len, lvl_unc;
   CHECK(fread(&lvl_off, 8, 1, f) == 1);
   CHECK(fread(&lvl_len, 8, 1, f) == 1);
   CHECK(fread(&lvl_unc, 8, 1, f) == 1);
   CHECK(lvl_len == 4 * 4 * 4);
   CHECK(lvl_unc == lvl_len);
   CHECK(lvl_off % 16 == 0);    /* image alignment */

   /* Read first 4 bytes of DFD: total DFD byte length (including itself). */
   fseek(f, (long)dfd_off, SEEK_SET);
   uint32_t dfd_total;
   CHECK(fread(&dfd_total, 4, 1, f) == 1);
   CHECK(dfd_total == dfd_len);

   fclose(f);
   remove(path);
}

static void test_dfd_for_format(VkFormat fmt, size_t expected_pixel_bytes)
{
   const char *path = "/tmp/ktx2_test_fmt.ktx2";
   uint8_t pixels[4 * 4 * 16];   /* room for up to 16 BPT */
   memset(pixels, 0x11, sizeof pixels);

   ktx2_write_params_t p = {
      .vk_format     = fmt,
      .width         = 4,
      .height        = 4,
      .pixels        = pixels,
      .pixels_size   = expected_pixel_bytes,
   };
   CHECK(ktx2_write_file(path, &p));

   /* Just confirm file exists and DFD is non-empty (i.e., the dispatcher
    * recognized the format and emitted a real DFD, not the empty fallback).
    * Empty-DFD length is 28; recognized formats are larger. */
   FILE *f = fopen(path, "rb");
   CHECK(f != NULL); if (!f) return;
   fseek(f, 12 + 9 * 4, SEEK_SET);    /* dfd_byte_offset */
   uint32_t dfd_off;
   CHECK(fread(&dfd_off, 4, 1, f) == 1);
   uint32_t dfd_len;
   CHECK(fread(&dfd_len, 4, 1, f) == 1);
   CHECK(dfd_len > 28);    /* real DFD, not empty-fallback */
   fclose(f);
   remove(path);
}

static void test_dfd_coverage(void)
{
   test_dfd_for_format(VK_FORMAT_R8_UNORM,                4 * 4 * 1);
   test_dfd_for_format(VK_FORMAT_R8G8_UNORM,              4 * 4 * 2);
   test_dfd_for_format(VK_FORMAT_R8G8B8A8_UNORM,          4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_R8G8B8A8_SRGB,           4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_R8G8B8A8_UINT,           4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_R8G8B8A8_SINT,           4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_B8G8R8A8_UNORM,          4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_R16_SFLOAT,              4 * 4 * 2);
   test_dfd_for_format(VK_FORMAT_R16G16_SFLOAT,           4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_R16G16B16A16_SFLOAT,     4 * 4 * 8);
   test_dfd_for_format(VK_FORMAT_R16G16B16A16_UNORM,      4 * 4 * 8);
   test_dfd_for_format(VK_FORMAT_R32_SFLOAT,              4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_R32G32_SFLOAT,           4 * 4 * 8);
   test_dfd_for_format(VK_FORMAT_R32G32B32A32_SFLOAT,     4 * 4 * 16);
   test_dfd_for_format(VK_FORMAT_R32_UINT,                4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_R32_SINT,                4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4 * 4 * 4);
   test_dfd_for_format(VK_FORMAT_B10G11R11_UFLOAT_PACK32,  4 * 4 * 4);
}

static void test_dfd_byte_content(void)
{
   /* Case 1: R8G8B8A8_SRGB
    * Expected DFD layout (starting at file offset dfd_off):
    *   [0..3]  total length = 4 + 24 + 4 × 16 = 92 (little-endian)
    *   [4..7]  vendor (17b) + descriptor type (15b) = 0 0 0 0
    *   [8..9]  version = 2 (KDF_VERSION), [10..11] block size = 88
    *   [12]    color_model = 1 (RGBSDA)
    *   [13]    primaries  = 1 (BT709)
    *   [14]    transfer   = 2 (SRGB)  -- THIS IS THE KEY ASSERTION
    *   [15]    flags      = 0
    *   [16..23] texel block dims (all 0), bytes_plane0 = 4, rest 0
    *   [24..39] sample 0: R (offset 0, length 8 -> stored 7, type 0, qualifier 0)
    *   ...
    *   [72..87] sample 3: A (offset 24, length 8 -> 7, type 15 (ALPHA),
    *                         qualifier = LINEAR (0x10) -- THE OTHER KEY ASSERTION)
    */
   {
      const char *path = "/tmp/ktx2_test_srgb.ktx2";
      uint8_t pixels[4 * 4 * 4];
      memset(pixels, 0x00, sizeof pixels);
      ktx2_write_params_t p = {
         .vk_format     = VK_FORMAT_R8G8B8A8_SRGB,
         .width         = 4, .height = 4,
         .pixels = pixels, .pixels_size = sizeof pixels,
      };
      CHECK(ktx2_write_file(path, &p));
      FILE *f = fopen(path, "rb"); CHECK(f != NULL); if (!f) return;

      /* Read DFD offset from header. */
      fseek(f, 12 + 9 * 4, SEEK_SET);
      uint32_t dfd_off;
      CHECK(fread(&dfd_off, 4, 1, f) == 1);

      /* Total length first 4 bytes of DFD. */
      fseek(f, (long)dfd_off, SEEK_SET);
      uint32_t dfd_total;
      CHECK(fread(&dfd_total, 4, 1, f) == 1);
      CHECK(dfd_total == 4 + 24 + 4 * 16);

      /* Basic block header — read color_model/primaries/transfer/flags. */
      uint8_t basic[8];
      fseek(f, (long)(dfd_off + 4 + 8), SEEK_SET);   /* skip vendor+version+blocksize = 8 bytes */
      CHECK(fread(basic, 1, 8, f) == 8);
      CHECK(basic[0] == 1);   /* RGBSDA */
      CHECK(basic[1] == 1);   /* BT709 */
      CHECK(basic[2] == 2);   /* SRGB transfer */

      /* Sample 3 (alpha) — bytes 4–7 of the 16-byte sample block,
       * channel_type byte at offset 3. */
      long sample3 = (long)(dfd_off + 4 + 24 + 3 * 16);   /* +3 byte = channel_type */
      fseek(f, sample3 + 3, SEEK_SET);
      uint8_t alpha_chan_type;
      CHECK(fread(&alpha_chan_type, 1, 1, f) == 1);
      CHECK((alpha_chan_type & 0x0F) == 15);          /* ALPHA channel id */
      CHECK((alpha_chan_type & 0x10) == 0x10);        /* LINEAR qualifier set */

      fclose(f);
      remove(path);
   }

   /* Case 2: B10G11R11_UFLOAT_PACK32
    * Expected sample layout (3 channels):
    *   sample[0]: R, bit_offset = 0,  bit_length-1 = 10 (so 11-bit R), float qualifier
    *   sample[1]: G, bit_offset = 11, bit_length-1 = 10
    *   sample[2]: B, bit_offset = 22, bit_length-1 = 9  (10-bit B)
    */
   {
      const char *path = "/tmp/ktx2_test_b10g11r11.ktx2";
      uint8_t pixels[4 * 4 * 4];
      memset(pixels, 0x00, sizeof pixels);
      ktx2_write_params_t p = {
         .vk_format     = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
         .width         = 4, .height = 4,
         .pixels = pixels, .pixels_size = sizeof pixels,
      };
      CHECK(ktx2_write_file(path, &p));
      FILE *f = fopen(path, "rb"); CHECK(f != NULL); if (!f) return;

      fseek(f, 12 + 9 * 4, SEEK_SET);
      uint32_t dfd_off;
      CHECK(fread(&dfd_off, 4, 1, f) == 1);

      /* Walk to sample 0 (offset 4 + 24 from dfd start). */
      long s0 = (long)(dfd_off + 4 + 24);
      fseek(f, s0, SEEK_SET);
      uint16_t bit_offset; uint8_t bit_len_m1; uint8_t chan_type;
      CHECK(fread(&bit_offset, 2, 1, f) == 1);
      CHECK(fread(&bit_len_m1, 1, 1, f) == 1);
      CHECK(fread(&chan_type,  1, 1, f) == 1);
      CHECK(bit_offset == 0);
      CHECK(bit_len_m1 == 10);
      CHECK((chan_type & 0x0F) == 0);             /* RED */
      CHECK((chan_type & 0x80) == 0x80);          /* FLOAT qualifier */

      /* Sample 1: G at bit_offset 11. */
      fseek(f, s0 + 16, SEEK_SET);
      CHECK(fread(&bit_offset, 2, 1, f) == 1);
      CHECK(fread(&bit_len_m1, 1, 1, f) == 1);
      CHECK(fread(&chan_type,  1, 1, f) == 1);
      CHECK(bit_offset == 11);
      CHECK(bit_len_m1 == 10);
      CHECK((chan_type & 0x0F) == 1);             /* GREEN */

      /* Sample 2: B at bit_offset 22, length 10-1 = 9. */
      fseek(f, s0 + 32, SEEK_SET);
      CHECK(fread(&bit_offset, 2, 1, f) == 1);
      CHECK(fread(&bit_len_m1, 1, 1, f) == 1);
      CHECK(fread(&chan_type,  1, 1, f) == 1);
      CHECK(bit_offset == 22);
      CHECK(bit_len_m1 == 9);
      CHECK((chan_type & 0x0F) == 2);             /* BLUE */

      fclose(f);
      remove(path);
   }
}

int main(void)
{
   test_level_index_and_dfd();
   test_dfd_coverage();
   test_dfd_byte_content();
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
