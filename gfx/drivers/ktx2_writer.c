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
 * 68 bytes that follow it: 13 × uint32 (52 bytes) + 2 × uint64 (16 bytes).
 * Fields are written individually to avoid struct padding issues. */
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

#define KTX2_HEADER_SIZE        68   /* identifier excluded; matches KTX 2.0 spec §3 */
#define KTX2_LEVEL_INDEX_SIZE   24   /* per level */

/* KDF descriptor block constants (Khronos Data Format 1.3). */
#define KDF_VERSION                                  2
#define KDF_MODEL_RGBSDA                             1
#define KDF_MODEL_UNSPECIFIED                        0
#define KDF_PRIMARIES_BT709                          1
#define KDF_PRIMARIES_UNSPECIFIED                    0
#define KDF_TRANSFER_LINEAR                          1
#define KDF_TRANSFER_SRGB                            2
#define KDF_TRANSFER_UNSPECIFIED                     0

/* Qualifier bit flags occupy the upper nibble of the channelType byte. */
#define KDF_SAMPLE_QUAL_FLOAT                        (1u << 7)
#define KDF_SAMPLE_QUAL_SIGNED                       (1u << 6)
#define KDF_SAMPLE_QUAL_EXPONENT                     (1u << 5)
#define KDF_SAMPLE_QUAL_LINEAR                       (1u << 4)

#define KDF_CHANNEL_RGBSDA_RED                       0
#define KDF_CHANNEL_RGBSDA_GREEN                     1
#define KDF_CHANNEL_RGBSDA_BLUE                      2
#define KDF_CHANNEL_RGBSDA_ALPHA                     15

#define KTX2_IMAGE_ALIGNMENT                         16
#define KTX2_DFD_MAX_BYTES                           256

/* Write one 16-byte sample block. */
static size_t ktx2_emit_sample(uint8_t *out,
      uint16_t bit_offset, uint8_t bit_length_minus_one,
      uint8_t channel_type, uint8_t qualifier_bits,
      uint32_t lower, uint32_t upper)
{
   memcpy(out + 0,  &bit_offset, 2);
   out[2]  = bit_length_minus_one;
   out[3]  = (uint8_t)(channel_type | qualifier_bits);
   out[4]  = 0; out[5] = 0; out[6] = 0; out[7] = 0;
   memcpy(out + 8,  &lower, 4);
   memcpy(out + 12, &upper, 4);
   return 16;
}

/* Helper: write the 24-byte basic-block header.  bytes_plane0 is the
 * total byte size of one texel.  Returns 24. */
static size_t ktx2_emit_basic_header(uint8_t *out,
      uint16_t total_block_size,
      uint8_t color_model, uint8_t primaries, uint8_t transfer,
      uint8_t bytes_plane0)
{
   uint32_t vendor_descriptor = 0;     /* vendor 0, descriptor type 0 */
   uint16_t version           = KDF_VERSION;
   memcpy(out + 0, &vendor_descriptor, 4);
   memcpy(out + 4, &version, 2);
   memcpy(out + 6, &total_block_size, 2);
   out[8]  = color_model;
   out[9]  = primaries;
   out[10] = transfer;
   out[11] = 0;                        /* flags = ALPHA_STRAIGHT */
   out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 0;   /* texel block dim */
   out[16] = bytes_plane0;
   out[17] = 0; out[18] = 0; out[19] = 0;
   out[20] = 0; out[21] = 0; out[22] = 0; out[23] = 0;
   return 24;
}

/* Parametric emitter for non-packed RGBA-family formats.
 *   - num_channels: 1, 2, or 4
 *   - bits_per_channel: 8, 16, or 32
 *   - is_float: true for SFLOAT, false otherwise
 *   - is_signed: true for SINT / SNORM (and SFLOAT, which sets the SIGNED bit)
 *   - is_integer: true for UINT/SINT, false for UNORM/SNORM/SFLOAT
 *   - is_srgb: true for SRGB UNORM 8-bit only
 *   - swap_rb: true for B8G8R8A8_* (channel order is B, G, R, A)
 *
 * Returns the number of bytes written to `out`, beginning with the
 * 4-byte total-length prefix per spec.
 */
static size_t ktx2_emit_dfd_basic(uint8_t *out,
      unsigned num_channels,
      unsigned bits_per_channel,
      bool is_float,
      bool is_signed,
      bool is_integer,
      bool is_srgb,
      bool swap_rb)
{
   size_t n = 0;
   uint32_t total;
   uint16_t block_size;
   uint8_t  qualifier_bits = 0;
   uint8_t  channel_types[4];
   uint32_t lower = 0, upper = 0;
   uint8_t  bytes_per_texel;
   unsigned i;

   if (num_channels == 0 || num_channels > 4)
      num_channels = 4;

   if (is_float)    qualifier_bits |= KDF_SAMPLE_QUAL_FLOAT;
   if (is_signed)   qualifier_bits |= KDF_SAMPLE_QUAL_SIGNED;

   /* Channel order. */
   channel_types[0] = swap_rb ? KDF_CHANNEL_RGBSDA_BLUE  : KDF_CHANNEL_RGBSDA_RED;
   channel_types[1] = KDF_CHANNEL_RGBSDA_GREEN;
   channel_types[2] = swap_rb ? KDF_CHANNEL_RGBSDA_RED   : KDF_CHANNEL_RGBSDA_BLUE;
   channel_types[3] = KDF_CHANNEL_RGBSDA_ALPHA;
   if (num_channels == 1) channel_types[0] = KDF_CHANNEL_RGBSDA_RED;
   if (num_channels == 2) { channel_types[0] = KDF_CHANNEL_RGBSDA_RED; channel_types[1] = KDF_CHANNEL_RGBSDA_GREEN; }

   /* Sample lower / upper per qualifier and bit width. */
   if (is_float)
   {
      if (bits_per_channel == 32) { lower = 0xBF800000u; upper = 0x3F800000u; }
      else if (bits_per_channel == 16) { lower = 0xBC00u; upper = 0x3C00u; } /* half -1.0, +1.0 */
      else { lower = 0; upper = 0x3F800000u; }   /* fallback */
   }
   else if (is_integer)
   {
      if (is_signed)
      {
         /* INT_MIN, INT_MAX */
         if (bits_per_channel == 8)  { lower = (uint32_t)(int32_t)-128;        upper = 127u; }
         else if (bits_per_channel == 16) { lower = (uint32_t)(int32_t)-32768; upper = 32767u; }
         else                           { lower = 0x80000000u;                 upper = 0x7FFFFFFFu; }
      }
      else
      {
         lower = 0;
         if (bits_per_channel == 8)  upper = 255u;
         else if (bits_per_channel == 16) upper = 65535u;
         else                           upper = 0xFFFFFFFFu;
      }
   }
   else
   {
      /* UNORM (or SRGB which encodes UNORM samples). */
      lower = 0;
      if (bits_per_channel == 8)       upper = 255u;
      else if (bits_per_channel == 16) upper = 65535u;
      else                             upper = 0xFFFFFFFFu;
   }

   bytes_per_texel = (uint8_t)(num_channels * (bits_per_channel / 8));
   block_size      = (uint16_t)(24 + num_channels * 16);
   total           = (uint32_t)(4 + block_size);

   memcpy(out + n, &total, 4); n += 4;

   n += ktx2_emit_basic_header(out + n,
         block_size,
         KDF_MODEL_RGBSDA,
         KDF_PRIMARIES_BT709,
         is_srgb ? KDF_TRANSFER_SRGB : KDF_TRANSFER_LINEAR,
         bytes_per_texel);

   for (i = 0; i < num_channels; i++)
   {
      uint16_t bit_offset = (uint16_t)(i * bits_per_channel);
      uint8_t  bit_len_m1 = (uint8_t)(bits_per_channel - 1);
      /* SRGB applies only to the color channels (R/G/B); alpha is linear.
       * Per the KDF spec, the per-sample LINEAR qualifier flips meaning
       * when the block transfer function is SRGB: setting LINEAR on
       * a sample means *that* sample is linear-encoded. */
      uint8_t qual = qualifier_bits;
      if (is_srgb && channel_types[i] == KDF_CHANNEL_RGBSDA_ALPHA)
         qual |= KDF_SAMPLE_QUAL_LINEAR;
      n += ktx2_emit_sample(out + n, bit_offset, bit_len_m1,
            channel_types[i], qual, lower, upper);
   }
   return n;
}

/* Packed format: A:2, B:10, G:10, R:10 stored as little-endian uint32. */
static size_t ktx2_emit_dfd_a2b10g10r10(uint8_t *out, bool is_int)
{
   size_t n = 0;
   uint16_t block_size = (uint16_t)(24 + 4 * 16);
   uint32_t total      = (uint32_t)(4 + block_size);
   memcpy(out + n, &total, 4); n += 4;
   n += ktx2_emit_basic_header(out + n, block_size,
         KDF_MODEL_RGBSDA, KDF_PRIMARIES_BT709, KDF_TRANSFER_LINEAR, 4);
   /* Spec packing order: A2 occupies the high bits.  Sample order R, G, B, A
    * with ascending bit offsets. */
   uint32_t color_upper = is_int ? 1023u : 1023u;   /* same for UNORM and UINT */
   uint32_t alpha_upper = is_int ? 3u    : 3u;
   n += ktx2_emit_sample(out + n,  0,  9, KDF_CHANNEL_RGBSDA_RED,   0, 0, color_upper);
   n += ktx2_emit_sample(out + n, 10,  9, KDF_CHANNEL_RGBSDA_GREEN, 0, 0, color_upper);
   n += ktx2_emit_sample(out + n, 20,  9, KDF_CHANNEL_RGBSDA_BLUE,  0, 0, color_upper);
   n += ktx2_emit_sample(out + n, 30,  1, KDF_CHANNEL_RGBSDA_ALPHA, 0, 0, alpha_upper);
   return n;
}

/* Packed B:10 G:11 R:11 unsigned float, no alpha. */
static size_t ktx2_emit_dfd_b10g11r11_ufloat(uint8_t *out)
{
   size_t n = 0;
   uint16_t block_size = (uint16_t)(24 + 3 * 16);
   uint32_t total      = (uint32_t)(4 + block_size);
   memcpy(out + n, &total, 4); n += 4;
   n += ktx2_emit_basic_header(out + n, block_size,
         KDF_MODEL_RGBSDA, KDF_PRIMARIES_BT709, KDF_TRANSFER_LINEAR, 4);
   uint8_t qual = KDF_SAMPLE_QUAL_FLOAT;
   n += ktx2_emit_sample(out + n,   0, 10, KDF_CHANNEL_RGBSDA_RED,   qual, 0, 0x3F800000u);
   n += ktx2_emit_sample(out + n,  11, 10, KDF_CHANNEL_RGBSDA_GREEN, qual, 0, 0x3F800000u);
   n += ktx2_emit_sample(out + n,  22,  9, KDF_CHANNEL_RGBSDA_BLUE,  qual, 0, 0x3F800000u);
   return n;
}

static size_t ktx2_emit_empty_dfd(uint8_t *out)
{
   /* 4-byte total length + 24-byte basic header with UNSPECIFIED model, no
    * samples.  Total = 28 bytes. */
   size_t n = 0;
   uint32_t total = 28;
   memcpy(out + n, &total, 4); n += 4;
   n += ktx2_emit_basic_header(out + n, 24,
         KDF_MODEL_UNSPECIFIED, KDF_PRIMARIES_UNSPECIFIED, KDF_TRANSFER_UNSPECIFIED, 0);
   return n;
}

/* Dispatch a DFD for the given VkFormat.  Returns 0 if unrecognized
 * (caller falls back to empty DFD with a warning). */
static size_t ktx2_emit_dfd_for(VkFormat fmt, uint8_t *out, size_t out_cap)
{
   (void)out_cap;
   switch (fmt)
   {
      /* 1-channel formats */
      case VK_FORMAT_R8_UNORM:    return ktx2_emit_dfd_basic(out, 1, 8,  false, false, false, false, false);
      case VK_FORMAT_R8_UINT:     return ktx2_emit_dfd_basic(out, 1, 8,  false, false, true,  false, false);
      case VK_FORMAT_R8_SINT:     return ktx2_emit_dfd_basic(out, 1, 8,  false, true,  true,  false, false);
      case VK_FORMAT_R16_UINT:    return ktx2_emit_dfd_basic(out, 1, 16, false, false, true,  false, false);
      case VK_FORMAT_R16_SINT:    return ktx2_emit_dfd_basic(out, 1, 16, false, true,  true,  false, false);
      case VK_FORMAT_R16_SFLOAT:  return ktx2_emit_dfd_basic(out, 1, 16, true,  true,  false, false, false);
      case VK_FORMAT_R32_UINT:    return ktx2_emit_dfd_basic(out, 1, 32, false, false, true,  false, false);
      case VK_FORMAT_R32_SINT:    return ktx2_emit_dfd_basic(out, 1, 32, false, true,  true,  false, false);
      case VK_FORMAT_R32_SFLOAT:  return ktx2_emit_dfd_basic(out, 1, 32, true,  true,  false, false, false);

      /* 2-channel formats */
      case VK_FORMAT_R8G8_UNORM:    return ktx2_emit_dfd_basic(out, 2, 8,  false, false, false, false, false);
      case VK_FORMAT_R8G8_UINT:     return ktx2_emit_dfd_basic(out, 2, 8,  false, false, true,  false, false);
      case VK_FORMAT_R8G8_SINT:     return ktx2_emit_dfd_basic(out, 2, 8,  false, true,  true,  false, false);
      case VK_FORMAT_R16G16_UINT:   return ktx2_emit_dfd_basic(out, 2, 16, false, false, true,  false, false);
      case VK_FORMAT_R16G16_SINT:   return ktx2_emit_dfd_basic(out, 2, 16, false, true,  true,  false, false);
      case VK_FORMAT_R16G16_SFLOAT: return ktx2_emit_dfd_basic(out, 2, 16, true,  true,  false, false, false);
      case VK_FORMAT_R32G32_UINT:   return ktx2_emit_dfd_basic(out, 2, 32, false, false, true,  false, false);
      case VK_FORMAT_R32G32_SINT:   return ktx2_emit_dfd_basic(out, 2, 32, false, true,  true,  false, false);
      case VK_FORMAT_R32G32_SFLOAT: return ktx2_emit_dfd_basic(out, 2, 32, true,  true,  false, false, false);

      /* 4-channel formats */
      case VK_FORMAT_R8G8B8A8_UNORM:    return ktx2_emit_dfd_basic(out, 4, 8,  false, false, false, false, false);
      case VK_FORMAT_R8G8B8A8_UINT:     return ktx2_emit_dfd_basic(out, 4, 8,  false, false, true,  false, false);
      case VK_FORMAT_R8G8B8A8_SINT:     return ktx2_emit_dfd_basic(out, 4, 8,  false, true,  true,  false, false);
      case VK_FORMAT_R8G8B8A8_SRGB:     return ktx2_emit_dfd_basic(out, 4, 8,  false, false, false, true,  false);
      case VK_FORMAT_B8G8R8A8_UNORM:    return ktx2_emit_dfd_basic(out, 4, 8,  false, false, false, false, true);
      case VK_FORMAT_B8G8R8A8_SRGB:     return ktx2_emit_dfd_basic(out, 4, 8,  false, false, false, true,  true);
      case VK_FORMAT_R16G16B16A16_UINT:   return ktx2_emit_dfd_basic(out, 4, 16, false, false, true,  false, false);
      case VK_FORMAT_R16G16B16A16_SINT:   return ktx2_emit_dfd_basic(out, 4, 16, false, true,  true,  false, false);
      case VK_FORMAT_R16G16B16A16_UNORM:  return ktx2_emit_dfd_basic(out, 4, 16, false, false, false, false, false);
      case VK_FORMAT_R16G16B16A16_SFLOAT: return ktx2_emit_dfd_basic(out, 4, 16, true,  true,  false, false, false);
      case VK_FORMAT_R32G32B32A32_UINT:   return ktx2_emit_dfd_basic(out, 4, 32, false, false, true,  false, false);
      case VK_FORMAT_R32G32B32A32_SINT:   return ktx2_emit_dfd_basic(out, 4, 32, false, true,  true,  false, false);
      case VK_FORMAT_R32G32B32A32_SFLOAT: return ktx2_emit_dfd_basic(out, 4, 32, true,  true,  false, false, false);

      /* Packed formats */
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return ktx2_emit_dfd_a2b10g10r10(out, false);
      case VK_FORMAT_A2B10G10R10_UINT_PACK32:  return ktx2_emit_dfd_a2b10g10r10(out, true);
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32:  return ktx2_emit_dfd_b10g11r11_ufloat(out);

      default:                                  return 0;
   }
}

static uint64_t ktx2_align_up(uint64_t v, uint64_t a)
{
   return (v + a - 1) & ~(a - 1);
}

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
   uint8_t dfd_buf[KTX2_DFD_MAX_BYTES];
   size_t  dfd_len;
   uint64_t level_index_offset, dfd_offset, kvd_offset, kvd_len, image_offset;
   uint64_t level_off, level_len, level_unc;

   if (!out_path || !p)
      return false;

   f = fopen(out_path, "wb");
   if (!f)
   {
      RARCH_WARN("[KTX2] Failed to open %s for writing.\n", out_path);
      return false;
   }

   /* Layout. */
   level_index_offset = 12 + KTX2_HEADER_SIZE;
   dfd_offset         = level_index_offset + KTX2_LEVEL_INDEX_SIZE;

   dfd_len = ktx2_emit_dfd_for(p->vk_format, dfd_buf, sizeof dfd_buf);
   if (dfd_len == 0)
   {
      RARCH_WARN("[KTX2] No DFD entry for VkFormat %d; writing empty DFD.\n",
            (int)p->vk_format);
      dfd_len = ktx2_emit_empty_dfd(dfd_buf);
   }

   kvd_offset    = dfd_offset + dfd_len;
   kvd_len       = 0;                           /* T5 fills this */
   image_offset  = ktx2_align_up(kvd_offset + kvd_len, KTX2_IMAGE_ALIGNMENT);

   memset(&hdr, 0, sizeof hdr);
   hdr.vk_format               = p->vk_format;
   hdr.type_size               = ktx2_type_size_for(p->vk_format);
   hdr.pixel_width             = p->width;
   hdr.pixel_height            = p->height;
   hdr.face_count              = 1;
   hdr.level_count             = 1;
   hdr.dfd_byte_offset         = (uint32_t)dfd_offset;
   hdr.dfd_byte_length         = (uint32_t)dfd_len;
   hdr.kvd_byte_offset         = (uint32_t)(kvd_len ? kvd_offset : 0);
   hdr.kvd_byte_length         = (uint32_t)kvd_len;

   /* Identifier. */
   if (fwrite(ktx2_identifier, 1, 12, f) != 12) goto io_fail;

   /* Header — field by field for spec layout. */
   if (   fwrite(&hdr.vk_format,               4, 1, f) != 1
       || fwrite(&hdr.type_size,               4, 1, f) != 1
       || fwrite(&hdr.pixel_width,             4, 1, f) != 1
       || fwrite(&hdr.pixel_height,            4, 1, f) != 1
       || fwrite(&hdr.pixel_depth,             4, 1, f) != 1
       || fwrite(&hdr.layer_count,             4, 1, f) != 1
       || fwrite(&hdr.face_count,              4, 1, f) != 1
       || fwrite(&hdr.level_count,             4, 1, f) != 1
       || fwrite(&hdr.supercompression_scheme, 4, 1, f) != 1
       || fwrite(&hdr.dfd_byte_offset,         4, 1, f) != 1
       || fwrite(&hdr.dfd_byte_length,         4, 1, f) != 1
       || fwrite(&hdr.kvd_byte_offset,         4, 1, f) != 1
       || fwrite(&hdr.kvd_byte_length,         4, 1, f) != 1
       || fwrite(&hdr.sgd_byte_offset,         8, 1, f) != 1
       || fwrite(&hdr.sgd_byte_length,         8, 1, f) != 1)
      goto io_fail;

   /* Level index (single entry). */
   level_off = image_offset;
   level_len = (uint64_t)p->pixels_size;
   level_unc = level_len;
   if (fwrite(&level_off, 8, 1, f) != 1) goto io_fail;
   if (fwrite(&level_len, 8, 1, f) != 1) goto io_fail;
   if (fwrite(&level_unc, 8, 1, f) != 1) goto io_fail;

   /* DFD. */
   if (fwrite(dfd_buf, 1, dfd_len, f) != dfd_len) goto io_fail;

   /* (KVD section: T5.) */

   /* Pad to image alignment. */
   while ((uint64_t)ftell(f) < image_offset)
   {
      uint8_t pad = 0;
      if (fwrite(&pad, 1, 1, f) != 1) goto io_fail;
   }

   /* Image data. */
   if (p->pixels && p->pixels_size > 0)
      if (fwrite(p->pixels, 1, p->pixels_size, f) != p->pixels_size) goto io_fail;

   fclose(f);
   return true;

io_fail:
   RARCH_WARN("[KTX2] Write failure on %s.\n", out_path);
   fclose(f);
   remove(out_path);
   return false;
}
