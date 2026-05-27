/* gfx/drivers/ktx2_writer.h
 *
 * Minimal KTX 2.0 file writer for slang shader pass dumps.
 *
 * Subset emitted: single 2D color image, one mip level, no array layers,
 * no cube faces, no supercompression.  Caller supplies pixel bytes in
 * row-tight layout matching the format's texel block size.
 */
#ifndef KTX2_WRITER_H__
#define KTX2_WRITER_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional key/value entry for the KVD section.
 * `key` is a NUL-terminated string; `value` is binary, `value_len` bytes.
 * The writer takes care of length, alignment, and \0 padding per spec. */
typedef struct ktx2_kv_pair
{
   const char *key;
   const void *value;
   size_t      value_len;
} ktx2_kv_pair_t;

typedef struct ktx2_write_params
{
   VkFormat               vk_format;
   uint32_t               width;
   uint32_t               height;
   const void            *pixels;        /* row-tight, no padding */
   size_t                 pixels_size;   /* width * height * bytes_per_texel(vk_format) */
   const ktx2_kv_pair_t  *kv_pairs;
   size_t                 kv_pair_count;
} ktx2_write_params_t;

/* Write a KTX 2.0 file to `out_path`.  Returns true on success.
 * Logs and returns false on file I/O failure. */
bool ktx2_write_file(const char *out_path, const ktx2_write_params_t *p);

/* Bytes per texel for one of the supported VkFormats.
 * Returns 0 for formats not in the table. */
size_t ktx2_bytes_per_texel(VkFormat fmt);

/* Identifier bytes (12 bytes, fixed magic).  Exposed for tests. */
extern const uint8_t ktx2_identifier[12];

#ifdef __cplusplus
}
#endif

#endif
