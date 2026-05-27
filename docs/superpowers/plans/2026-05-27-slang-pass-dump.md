# Slang Shader Pass Dump Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Vulkan-only RetroArch debug feature that dumps each slang shader pass's color output, plus the raw emulator frame ("Original"), to disk as KTX 2.0 files when a user-bound hotkey is pressed.

**Architecture:** Two layers. The slang filter chain (`gfx/drivers_shader/shader_vulkan.cpp`) exposes read-only accessors for per-pass framebuffers and the Original input texture — rendering path is untouched. A new module (`gfx/drivers/vulkan_pass_dump.c`) orchestrates capture: when armed, it inserts per-image blits to host-visible staging buffers onto the chain's existing command buffer; after the frame's fence signals, it writes one KTX 2.0 file per image plus a `manifest.json`. A new standalone KTX 2.0 writer (`gfx/drivers/ktx2_writer.c`) is responsible for the file format only.

**Tech Stack:** C (writer + dump module + driver wiring), C++ (chain accessors), Vulkan, RetroArch's existing path/log utilities. Spec: `docs/superpowers/specs/2026-05-27-slang-pass-dump-design.md`.

**Companion spec:** Read first. The plan does not duplicate every rationale; the spec is the source of truth for *why*.

---

## File Structure

### New files

| File | Lines (approx) | Responsibility |
|---|---|---|
| `gfx/drivers/ktx2_writer.h` | 60 | Public C API of the writer |
| `gfx/drivers/ktx2_writer.c` | 400 | KTX 2.0 file emission — header, level index, DFD per VkFormat, KVD, image data |
| `gfx/drivers/vulkan_pass_dump.h` | 50 | Public C API of the dump module |
| `gfx/drivers/vulkan_pass_dump.c` | 500 | Arm/record/flush, staging buffers, manifest, name sanitization |
| `tests/ktx2_writer_test.c` | 250 | Standalone unit test (opt-in target) |

### Modified files

| File | Change |
|---|---|
| `gfx/drivers_shader/shader_vulkan.h` | Add 9 accessor prototypes (Task 8) |
| `gfx/drivers_shader/shader_vulkan.cpp` | Add 9 accessor implementations (Task 8) |
| `gfx/drivers/vulkan.c` | Add `pass_dump` field on `vk_t`; arm/record/flush call sites (Task 14) |
| `gfx/video_driver.h` | Add `video_driver_dump_slang_passes(void)` prototype (Task 15) |
| `gfx/video_driver.c` | Implement dispatch (Task 15) |
| `input/input_defines.h` | Add `RARCH_DUMP_SLANG_PASSES` enum value (Task 16) |
| `configuration.c` | Add `DECLARE_META_BIND` row in `input_config_bind_map[]` (Task 16) |
| `intl/msg_hash_lbl.h` | Add `MSG_HASH(MENU_ENUM_LABEL_INPUT_META_DUMP_SLANG_PASSES, ...)` (Task 16) |
| `intl/msg_hash_us.h` | Add English strings (Task 16) |
| `msg_hash.h` | Add the three new `MENU_ENUM_LABEL*` enum constants (Task 16) |
| `menu/menu_displaylist.c` | Add binding row in the hotkeys menu listing (Task 16) |
| `command.h` | Add `CMD_EVENT_DUMP_SLANG_PASSES` enum value (Task 17) |
| `retroarch.c` | Add `case CMD_EVENT_DUMP_SLANG_PASSES:` handler in `command_event()` switch (Task 17) |
| `runloop.c` | Add `HOTKEY_CHECK` for new action (Task 17) |
| `Makefile.common` | Add `ktx2_writer.o` + `vulkan_pass_dump.o` under `HAVE_VULKAN` (Task 7, Task 14) |

---

## Phase 1 — KTX 2.0 Writer (Tasks 1–7)

The writer is pure C with no Vulkan dependencies. Each task is TDD: failing test → minimal implementation → passing test → commit.

### Task 1: Writer skeleton + identifier emission

**Files:**
- Create: `gfx/drivers/ktx2_writer.h`
- Create: `gfx/drivers/ktx2_writer.c`
- Create: `tests/ktx2_writer_test.c`

- [ ] **Step 1: Write the header file**

```c
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
```

- [ ] **Step 2: Write the writer source with just the identifier constant**

```c
/* gfx/drivers/ktx2_writer.c */

#include "ktx2_writer.h"

#include <stdio.h>
#include <string.h>

#include "../../verbosity.h"

const uint8_t ktx2_identifier[12] = {
   0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
   0x0D, 0x0A, 0x1A, 0x0A
};

size_t ktx2_bytes_per_texel(VkFormat fmt)
{
   (void)fmt;
   return 0;  /* filled in Task 2 */
}

bool ktx2_write_file(const char *out_path, const ktx2_write_params_t *p)
{
   (void)out_path; (void)p;
   return false;  /* filled in subsequent tasks */
}
```

- [ ] **Step 3: Write the first failing test (identifier bytes)**

```c
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
   test_identifier_bytes();
   if (failures)
   {
      fprintf(stderr, "%d test(s) failed\n", failures);
      return 1;
   }
   printf("all tests passed\n");
   return 0;
}
```

- [ ] **Step 4: Compile and run the test**

Run:
```
clang -std=c99 -Wall -Wextra -I. -I deps/Vulkan-Headers/include \
   tests/ktx2_writer_test.c gfx/drivers/ktx2_writer.c \
   -o /tmp/ktx2_writer_test && /tmp/ktx2_writer_test
```

Expected: `all tests passed`.

If `deps/Vulkan-Headers/include` is missing on the host, substitute with the system Vulkan headers location (`/opt/homebrew/include` on macOS with the LunarG SDK, `/usr/include` on Linux).

- [ ] **Step 5: Commit**

```bash
git add gfx/drivers/ktx2_writer.h gfx/drivers/ktx2_writer.c tests/ktx2_writer_test.c
git commit -m "ktx2_writer: skeleton + identifier constant"
```

---

### Task 2: VkFormat→bytes-per-texel table

**Files:**
- Modify: `gfx/drivers/ktx2_writer.c` (`ktx2_bytes_per_texel`)
- Modify: `tests/ktx2_writer_test.c` (new test)

- [ ] **Step 1: Add the failing test**

Append to `tests/ktx2_writer_test.c` (before `main`):

```c
static void test_bytes_per_texel(void)
{
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R8G8B8A8_UNORM)         == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R8G8B8A8_SRGB)          == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_B8G8R8A8_UNORM)         == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_B8G8R8A8_SRGB)          == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R16G16B16A16_UNORM)     == 8);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_R16G16B16A16_SFLOAT)    == 8);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_A2B10G10R10_UNORM_PACK32) == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_B10G11R11_UFLOAT_PACK32)  == 4);
   CHECK(ktx2_bytes_per_texel(VK_FORMAT_UNDEFINED) == 0);
}
```

And call it from `main` before the existing test.

- [ ] **Step 2: Run, verify it fails**

Same compile command as Task 1 Step 4. Expected: 8 `CHECK` failures for nonzero formats (writer still returns 0).

- [ ] **Step 3: Implement**

Replace `ktx2_bytes_per_texel` in `gfx/drivers/ktx2_writer.c`:

```c
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
```

- [ ] **Step 4: Run, verify it passes**

Same compile command. Expected: `all tests passed`.

- [ ] **Step 5: Commit**

```bash
git add gfx/drivers/ktx2_writer.c tests/ktx2_writer_test.c
git commit -m "ktx2_writer: VkFormat -> bytes-per-texel table"
```

---

### Task 3: Header struct + writer for header + identifier

**Files:**
- Modify: `gfx/drivers/ktx2_writer.c`
- Modify: `tests/ktx2_writer_test.c`

- [ ] **Step 1: Add the failing test**

```c
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

   uint8_t id[12];
   CHECK(fread(id, 1, 12, f) == 12);
   CHECK(memcmp(id, ktx2_identifier, 12) == 0);

   uint32_t hdr[17];
   CHECK(fread(hdr, sizeof(uint32_t), 17, f) == 17);
   CHECK(hdr[0]  == VK_FORMAT_R8G8B8A8_UNORM);   /* vkFormat */
   CHECK(hdr[1]  == 1);                          /* typeSize */
   CHECK(hdr[2]  == 4);                          /* pixelWidth */
   CHECK(hdr[3]  == 4);                          /* pixelHeight */
   CHECK(hdr[4]  == 0);                          /* pixelDepth */
   CHECK(hdr[5]  == 0);                          /* layerCount */
   CHECK(hdr[6]  == 1);                          /* faceCount */
   CHECK(hdr[7]  == 1);                          /* levelCount */
   CHECK(hdr[8]  == 0);                          /* supercompressionScheme */

   fclose(f);
   remove(path);
}
```

Add the call to `main`.

- [ ] **Step 2: Run, verify it fails** (`ktx2_write_file` returns false)

- [ ] **Step 3: Implement the header writer**

In `gfx/drivers/ktx2_writer.c`, add at file scope:

```c
/* KTX 2.0 header layout, on disk all little-endian uint32 / uint64.
 * Total size: 68 bytes (without trailing SGD offsets, which we still
 * include as uint64 = 16 bytes; effective header total = 80 bytes). */
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
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_B8G8R8A8_UNORM:
      case VK_FORMAT_B8G8R8A8_SRGB:
         return 1;
      case VK_FORMAT_R16G16B16A16_UNORM:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
         return 2;
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
         return 1;
      default:
         return 1;
   }
}
```

Replace `ktx2_write_file` with:

```c
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
```

- [ ] **Step 4: Run, verify it passes**

- [ ] **Step 5: Commit**

```bash
git add gfx/drivers/ktx2_writer.c tests/ktx2_writer_test.c
git commit -m "ktx2_writer: identifier + header struct"
```

---

### Task 4: Level index + DFD per format

**Files:**
- Modify: `gfx/drivers/ktx2_writer.c`
- Modify: `tests/ktx2_writer_test.c`

- [ ] **Step 1: Add failing tests**

```c
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

   /* Skip identifier (12) and header (80). */
   fseek(f, 12, SEEK_SET);

   /* Read header into a struct mirroring on-disk layout. */
   uint32_t vk_format, type_size, w, h, d, layer, face, level, sc;
   uint32_t dfd_off, dfd_len, kvd_off, kvd_len;
   uint64_t sgd_off, sgd_len;
   CHECK(fread(&vk_format, 4, 1, f) == 1);
   CHECK(fread(&type_size, 4, 1, f) == 1);
   CHECK(fread(&w,         4, 1, f) == 1);
   CHECK(fread(&h,         4, 1, f) == 1);
   CHECK(fread(&d,         4, 1, f) == 1);
   CHECK(fread(&layer,     4, 1, f) == 1);
   CHECK(fread(&face,      4, 1, f) == 1);
   CHECK(fread(&level,     4, 1, f) == 1);
   CHECK(fread(&sc,        4, 1, f) == 1);
   CHECK(fread(&dfd_off,   4, 1, f) == 1);
   CHECK(fread(&dfd_len,   4, 1, f) == 1);
   CHECK(fread(&kvd_off,   4, 1, f) == 1);
   CHECK(fread(&kvd_len,   4, 1, f) == 1);
   CHECK(fread(&sgd_off,   8, 1, f) == 1);
   CHECK(fread(&sgd_len,   8, 1, f) == 1);

   /* DFD must start immediately after level index (one entry of 24 bytes).
    * level index begins at file offset 12 + 80 = 92.  DFD offset is the
    * byte offset from start of file. */
   CHECK(dfd_off == 92 + 24);
   CHECK(dfd_len  > 0);
   CHECK(sgd_off == 0 && sgd_len == 0);

   /* Read level index entry. */
   fseek(f, 92, SEEK_SET);
   uint64_t lvl_off, lvl_len, lvl_uncompressed;
   CHECK(fread(&lvl_off,          8, 1, f) == 1);
   CHECK(fread(&lvl_len,          8, 1, f) == 1);
   CHECK(fread(&lvl_uncompressed, 8, 1, f) == 1);
   CHECK(lvl_len == 4 * 4 * 4);
   CHECK(lvl_uncompressed == lvl_len);
   CHECK(lvl_off % 16 == 0);    /* image data alignment per spec */

   /* Read first 4 bytes of DFD: total DFD byte length (including itself). */
   fseek(f, (long)dfd_off, SEEK_SET);
   uint32_t dfd_total;
   CHECK(fread(&dfd_total, 4, 1, f) == 1);
   CHECK(dfd_total == dfd_len);

   fclose(f);
   remove(path);
}
```

Add the call to `main`.

- [ ] **Step 2: Run, verify it fails** (offsets and lengths are zero).

- [ ] **Step 3: Implement DFD + level index**

Add to `gfx/drivers/ktx2_writer.c`:

```c
/* KDF descriptor block constants (Khronos Data Format 1.3). */
#define KDF_VERSION                                  2
#define KDF_MODEL_RGBSDA                             1
#define KDF_MODEL_UNSPECIFIED                        0
#define KDF_PRIMARIES_BT709                          1
#define KDF_TRANSFER_LINEAR                          1
#define KDF_TRANSFER_SRGB                            2

#define KDF_SAMPLE_FLOAT                             (1u << 7)
#define KDF_SAMPLE_SIGNED                            (1u << 6)
#define KDF_CHANNEL_RGBSDA_RED                       0
#define KDF_CHANNEL_RGBSDA_GREEN                     1
#define KDF_CHANNEL_RGBSDA_BLUE                      2
#define KDF_CHANNEL_RGBSDA_ALPHA                     15

#define KTX2_IMAGE_ALIGNMENT                         16

/* One descriptor block fits in 24 + 16*N bytes for N samples (channels).
 * We support up to 4 samples per format. */
typedef struct
{
   uint32_t total_length;          /* 4 bytes: total DFD byte length */
   /* Following: one descriptor block. */
   uint16_t vendor_id__descriptor_type;     /* packed: 17b + 15b but we write as uint32 split */
   uint16_t version_number;                 /* 16b */
   uint16_t descriptor_block_size;          /* 16b */
   uint8_t  color_model;
   uint8_t  color_primaries;
   uint8_t  transfer_function;
   uint8_t  flags;
   uint8_t  texel_block_dim[4];
   uint8_t  bytes_planes[8];
   /* Followed by 16 bytes per sample. */
} ktx2_dfd_header_t;

/* Sample block (16 bytes). */
typedef struct
{
   uint16_t bit_offset;
   uint8_t  bit_length;             /* one less than actual bit length */
   uint8_t  channel_type__qualifiers;
   uint8_t  sample_position[4];
   uint32_t sample_lower;
   uint32_t sample_upper;
} ktx2_dfd_sample_t;

/* Bytes per texel times 8 = total bit count for the format, but we also
 * track per-channel layouts.  For each supported VkFormat we emit a
 * fixed DFD, generated below. */

static size_t ktx2_emit_sample(uint8_t *out,
      uint16_t bit_offset, uint8_t bit_length_minus_one,
      uint8_t channel_type, uint8_t qualifiers,
      uint32_t lower, uint32_t upper)
{
   /* Bytes per sample block: 16 */
   memcpy(out + 0,  &bit_offset, 2);
   out[2]  = bit_length_minus_one;
   out[3]  = (uint8_t)(channel_type | (qualifiers << 4));
   out[4]  = 0; out[5] = 0; out[6] = 0; out[7] = 0;  /* sample positions */
   memcpy(out + 8,  &lower, 4);
   memcpy(out + 12, &upper, 4);
   return 16;
}

static size_t ktx2_emit_dfd_unorm_rgba8(uint8_t *out,
      uint8_t color_model, uint8_t transfer, uint8_t r_chan, uint8_t g_chan, uint8_t b_chan, uint8_t a_chan)
{
   /* 4 samples × 16 bytes + 24 byte basic block header = 88 bytes of block.
    * Plus 4 bytes total-length prefix = 92 bytes overall. */
   size_t n = 0;

   /* total DFD byte length (filled at end). */
   uint32_t total = 4 + 24 + 4 * 16;
   memcpy(out + n, &total, 4); n += 4;

   /* Basic block header (24 bytes). */
   uint32_t vendor_descriptor = 0;  /* vendor 0, descriptor type 0 (basic) */
   memcpy(out + n, &vendor_descriptor, 4); n += 4;
   uint16_t version = KDF_VERSION;
   uint16_t block_size = 24 + 4 * 16;
   memcpy(out + n, &version,    2); n += 2;
   memcpy(out + n, &block_size, 2); n += 2;
   out[n++] = color_model;
   out[n++] = KDF_PRIMARIES_BT709;
   out[n++] = transfer;
   out[n++] = 0;                            /* flags = ALPHA_STRAIGHT */
   out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;  /* texel block dim */
   /* bytesPlane0..3 = 4, rest 0 */
   out[n++] = 4; out[n++] = 0; out[n++] = 0; out[n++] = 0;
   out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;

   /* 4 samples: R, G, B, A — each 8 bits wide. */
   n += ktx2_emit_sample(out + n,  0, 7, r_chan, 0, 0, 255);
   n += ktx2_emit_sample(out + n,  8, 7, g_chan, 0, 0, 255);
   n += ktx2_emit_sample(out + n, 16, 7, b_chan, 0, 0, 255);
   n += ktx2_emit_sample(out + n, 24, 7, a_chan, 0, 0, 255);

   return n;
}

static size_t ktx2_emit_dfd_for(VkFormat fmt, uint8_t *out, size_t out_cap)
{
   /* Returns 0 on unsupported format (caller emits empty DFD with warning).
    * out_cap is at least 256 in all call sites. */
   (void)out_cap;

   switch (fmt)
   {
      case VK_FORMAT_R8G8B8A8_UNORM:
         return ktx2_emit_dfd_unorm_rgba8(out, KDF_MODEL_RGBSDA, KDF_TRANSFER_LINEAR,
               KDF_CHANNEL_RGBSDA_RED, KDF_CHANNEL_RGBSDA_GREEN,
               KDF_CHANNEL_RGBSDA_BLUE, KDF_CHANNEL_RGBSDA_ALPHA);
      case VK_FORMAT_R8G8B8A8_SRGB:
         return ktx2_emit_dfd_unorm_rgba8(out, KDF_MODEL_RGBSDA, KDF_TRANSFER_SRGB,
               KDF_CHANNEL_RGBSDA_RED, KDF_CHANNEL_RGBSDA_GREEN,
               KDF_CHANNEL_RGBSDA_BLUE, KDF_CHANNEL_RGBSDA_ALPHA);
      case VK_FORMAT_B8G8R8A8_UNORM:
         return ktx2_emit_dfd_unorm_rgba8(out, KDF_MODEL_RGBSDA, KDF_TRANSFER_LINEAR,
               KDF_CHANNEL_RGBSDA_BLUE, KDF_CHANNEL_RGBSDA_GREEN,
               KDF_CHANNEL_RGBSDA_RED, KDF_CHANNEL_RGBSDA_ALPHA);
      case VK_FORMAT_B8G8R8A8_SRGB:
         return ktx2_emit_dfd_unorm_rgba8(out, KDF_MODEL_RGBSDA, KDF_TRANSFER_SRGB,
               KDF_CHANNEL_RGBSDA_BLUE, KDF_CHANNEL_RGBSDA_GREEN,
               KDF_CHANNEL_RGBSDA_RED, KDF_CHANNEL_RGBSDA_ALPHA);

      case VK_FORMAT_R16G16B16A16_UNORM:
      {
         /* 4 samples × 16 bits each. */
         size_t n = 0;
         uint32_t total = 4 + 24 + 4 * 16;
         memcpy(out + n, &total, 4); n += 4;
         uint32_t vd = 0; memcpy(out + n, &vd, 4); n += 4;
         uint16_t version = KDF_VERSION;
         uint16_t block_size = 24 + 4 * 16;
         memcpy(out + n, &version, 2); n += 2;
         memcpy(out + n, &block_size, 2); n += 2;
         out[n++] = KDF_MODEL_RGBSDA;
         out[n++] = KDF_PRIMARIES_BT709;
         out[n++] = KDF_TRANSFER_LINEAR;
         out[n++] = 0;
         out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         out[n++] = 8; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         n += ktx2_emit_sample(out + n,   0, 15, KDF_CHANNEL_RGBSDA_RED,   0, 0, 65535);
         n += ktx2_emit_sample(out + n,  16, 15, KDF_CHANNEL_RGBSDA_GREEN, 0, 0, 65535);
         n += ktx2_emit_sample(out + n,  32, 15, KDF_CHANNEL_RGBSDA_BLUE,  0, 0, 65535);
         n += ktx2_emit_sample(out + n,  48, 15, KDF_CHANNEL_RGBSDA_ALPHA, 0, 0, 65535);
         return n;
      }

      case VK_FORMAT_R16G16B16A16_SFLOAT:
      {
         size_t n = 0;
         uint32_t total = 4 + 24 + 4 * 16;
         memcpy(out + n, &total, 4); n += 4;
         uint32_t vd = 0; memcpy(out + n, &vd, 4); n += 4;
         uint16_t version = KDF_VERSION;
         uint16_t block_size = 24 + 4 * 16;
         memcpy(out + n, &version, 2); n += 2;
         memcpy(out + n, &block_size, 2); n += 2;
         out[n++] = KDF_MODEL_RGBSDA;
         out[n++] = KDF_PRIMARIES_BT709;
         out[n++] = KDF_TRANSFER_LINEAR;
         out[n++] = 0;
         out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         out[n++] = 8; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         /* sample.lower/upper for SFLOAT: bit pattern of -1.0f / +1.0f. */
         uint32_t lower_neg1 = 0xBF800000;  /* -1.0f */
         uint32_t upper_pos1 = 0x3F800000;  /* +1.0f */
         uint8_t qualifiers  = (KDF_SAMPLE_FLOAT | KDF_SAMPLE_SIGNED) >> 4;
         n += ktx2_emit_sample(out + n,   0, 15, KDF_CHANNEL_RGBSDA_RED,   qualifiers, lower_neg1, upper_pos1);
         n += ktx2_emit_sample(out + n,  16, 15, KDF_CHANNEL_RGBSDA_GREEN, qualifiers, lower_neg1, upper_pos1);
         n += ktx2_emit_sample(out + n,  32, 15, KDF_CHANNEL_RGBSDA_BLUE,  qualifiers, lower_neg1, upper_pos1);
         n += ktx2_emit_sample(out + n,  48, 15, KDF_CHANNEL_RGBSDA_ALPHA, qualifiers, lower_neg1, upper_pos1);
         return n;
      }

      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      {
         /* packed: A:2, B:10, G:10, R:10 stored as LE uint32 */
         size_t n = 0;
         uint32_t total = 4 + 24 + 4 * 16;
         memcpy(out + n, &total, 4); n += 4;
         uint32_t vd = 0; memcpy(out + n, &vd, 4); n += 4;
         uint16_t version = KDF_VERSION;
         uint16_t block_size = 24 + 4 * 16;
         memcpy(out + n, &version, 2); n += 2;
         memcpy(out + n, &block_size, 2); n += 2;
         out[n++] = KDF_MODEL_RGBSDA;
         out[n++] = KDF_PRIMARIES_BT709;
         out[n++] = KDF_TRANSFER_LINEAR;
         out[n++] = 0;
         out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         out[n++] = 4; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         n += ktx2_emit_sample(out + n,   0,  9, KDF_CHANNEL_RGBSDA_RED,   0, 0, 1023);
         n += ktx2_emit_sample(out + n,  10,  9, KDF_CHANNEL_RGBSDA_GREEN, 0, 0, 1023);
         n += ktx2_emit_sample(out + n,  20,  9, KDF_CHANNEL_RGBSDA_BLUE,  0, 0, 1023);
         n += ktx2_emit_sample(out + n,  30,  1, KDF_CHANNEL_RGBSDA_ALPHA, 0, 0, 3);
         return n;
      }

      case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
      {
         /* B:10 G:11 R:11 unsigned float, no alpha. */
         size_t n = 0;
         uint32_t total = 4 + 24 + 3 * 16;
         memcpy(out + n, &total, 4); n += 4;
         uint32_t vd = 0; memcpy(out + n, &vd, 4); n += 4;
         uint16_t version = KDF_VERSION;
         uint16_t block_size = 24 + 3 * 16;
         memcpy(out + n, &version, 2); n += 2;
         memcpy(out + n, &block_size, 2); n += 2;
         out[n++] = KDF_MODEL_RGBSDA;
         out[n++] = KDF_PRIMARIES_BT709;
         out[n++] = KDF_TRANSFER_LINEAR;
         out[n++] = 0;
         out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         out[n++] = 4; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
         uint8_t qualifiers = KDF_SAMPLE_FLOAT >> 4;
         /* Spec: for shared-exponent and 11/10 unsigned float, sample
          * lower/upper are 0 and 1.0f bit pattern. */
         n += ktx2_emit_sample(out + n,   0, 10, KDF_CHANNEL_RGBSDA_RED,   qualifiers, 0, 0x3F800000);
         n += ktx2_emit_sample(out + n,  11, 10, KDF_CHANNEL_RGBSDA_GREEN, qualifiers, 0, 0x3F800000);
         n += ktx2_emit_sample(out + n,  22,  9, KDF_CHANNEL_RGBSDA_BLUE,  qualifiers, 0, 0x3F800000);
         return n;
      }

      default:
         return 0;  /* caller emits minimal "unknown" DFD + warning */
   }
}

static size_t ktx2_emit_empty_dfd(uint8_t *out)
{
   /* Minimal DFD: 4-byte total length + 24-byte basic header with
    * UNSPECIFIED model, no samples.  Total = 28 bytes. */
   size_t n = 0;
   uint32_t total = 28;
   memcpy(out + n, &total, 4); n += 4;
   uint32_t vd = 0; memcpy(out + n, &vd, 4); n += 4;
   uint16_t version = KDF_VERSION;
   uint16_t block_size = 24;
   memcpy(out + n, &version, 2); n += 2;
   memcpy(out + n, &block_size, 2); n += 2;
   out[n++] = KDF_MODEL_UNSPECIFIED;
   out[n++] = 0;  /* primaries */
   out[n++] = 0;  /* transfer */
   out[n++] = 0;  /* flags */
   out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
   out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
   out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
   return n;
}

static uint64_t ktx2_align_up(uint64_t v, uint64_t a)
{
   return (v + a - 1) & ~(a - 1);
}
```

Replace the `ktx2_write_file` body with the level-index-aware version (no KVD yet — that's Task 5; no image data — that's Task 6):

```c
bool ktx2_write_file(const char *out_path, const ktx2_write_params_t *p)
{
   FILE *f;
   ktx2_header_t hdr;
   uint8_t dfd_buf[256];
   size_t dfd_len;
   uint64_t level_index_offset, dfd_offset, kvd_offset, kvd_len, image_offset;

   if (!out_path || !p)
      return false;

   f = fopen(out_path, "wb");
   if (!f)
   {
      RARCH_WARN("[KTX2] Failed to open %s for writing.\n", out_path);
      return false;
   }

   /* Compute layout. */
   level_index_offset = 12 + KTX2_HEADER_SIZE;
   dfd_offset         = level_index_offset + KTX2_LEVEL_INDEX_SIZE;

   dfd_len = ktx2_emit_dfd_for(p->vk_format, dfd_buf, sizeof dfd_buf);
   if (dfd_len == 0)
   {
      RARCH_WARN("[KTX2] No DFD entry for VkFormat %d; writing empty DFD.\n", (int)p->vk_format);
      dfd_len = ktx2_emit_empty_dfd(dfd_buf);
   }

   kvd_offset    = dfd_offset + dfd_len;
   kvd_len       = 0;  /* filled in Task 5 */
   image_offset  = ktx2_align_up(kvd_offset + kvd_len, KTX2_IMAGE_ALIGNMENT);

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
   hdr.dfd_byte_offset         = (uint32_t)dfd_offset;
   hdr.dfd_byte_length         = (uint32_t)dfd_len;
   hdr.kvd_byte_offset         = (uint32_t)(kvd_len ? kvd_offset : 0);
   hdr.kvd_byte_length         = (uint32_t)kvd_len;
   hdr.sgd_byte_offset         = 0;
   hdr.sgd_byte_length         = 0;

   if (fwrite(ktx2_identifier, 1, 12, f) != 12) goto io_fail;
   if (fwrite(&hdr, 1, KTX2_HEADER_SIZE, f) != KTX2_HEADER_SIZE) goto io_fail;

   /* Level index (single entry). */
   uint64_t level_off = image_offset;
   uint64_t level_len = (uint64_t)p->pixels_size;
   uint64_t level_unc = level_len;
   if (fwrite(&level_off, 8, 1, f) != 1) goto io_fail;
   if (fwrite(&level_len, 8, 1, f) != 1) goto io_fail;
   if (fwrite(&level_unc, 8, 1, f) != 1) goto io_fail;

   /* DFD. */
   if (fwrite(dfd_buf, 1, dfd_len, f) != dfd_len) goto io_fail;

   /* (KVD section: Task 5.) */

   /* Pad to image alignment. */
   while ((uint64_t)ftell(f) < image_offset)
   {
      uint8_t pad = 0;
      if (fwrite(&pad, 1, 1, f) != 1) goto io_fail;
   }

   /* Image data: Task 6 — for now, just append the pixel bytes so the
    * test sees a non-zero level length matching pixels_size. */
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
```

- [ ] **Step 4: Run, verify it passes** — the new test plus the prior two.

- [ ] **Step 5: Commit**

```bash
git add gfx/drivers/ktx2_writer.c tests/ktx2_writer_test.c
git commit -m "ktx2_writer: level index + DFD per VkFormat"
```

---

### Task 5: KVD section + RAretroarch JSON

**Files:**
- Modify: `gfx/drivers/ktx2_writer.c`
- Modify: `tests/ktx2_writer_test.c`

- [ ] **Step 1: Add the failing test**

```c
static void test_kvd_section(void)
{
   const char *path = "/tmp/ktx2_test_kvd.ktx2";
   uint8_t pixels[4 * 4 * 4];
   memset(pixels, 0x11, sizeof pixels);

   const char *orient = "rd";  /* + implicit \0 */
   const char *meta   = "{\"pass\":3}";
   ktx2_kv_pair_t kvs[2] = {
      { "KTXorientation", orient, 3 },   /* include trailing \0 */
      { "RAretroarch",    meta,   strlen(meta) + 1 }
   };

   ktx2_write_params_t p = {
      .vk_format     = VK_FORMAT_R8G8B8A8_UNORM,
      .width         = 4,
      .height        = 4,
      .pixels        = pixels,
      .pixels_size   = sizeof pixels,
      .kv_pairs      = kvs,
      .kv_pair_count = 2,
   };
   CHECK(ktx2_write_file(path, &p));

   FILE *f = fopen(path, "rb");
   CHECK(f != NULL); if (!f) return;

   /* Skim header — pull KVD offset + length. */
   fseek(f, 12 + 11 * 4, SEEK_SET);   /* skip to kvd_byte_offset field */
   uint32_t kvd_off, kvd_len;
   CHECK(fread(&kvd_off, 4, 1, f) == 1);
   CHECK(fread(&kvd_len, 4, 1, f) == 1);
   CHECK(kvd_off > 0);
   CHECK(kvd_len > 0);

   fseek(f, (long)kvd_off, SEEK_SET);
   /* First entry: KTXorientation = "rd\0". */
   uint32_t e1_len;
   CHECK(fread(&e1_len, 4, 1, f) == 1);
   /* key "KTXorientation" (14) + \0 (1) + value "rd\0" (3) = 18 */
   CHECK(e1_len == 18);
   char buf1[18];
   CHECK(fread(buf1, 1, 18, f) == 18);
   CHECK(memcmp(buf1, "KTXorientation\0rd\0", 18) == 0);

   /* Padding to 4 bytes. e1_len=18 → pad 2 bytes. */
   uint8_t pad[2];
   CHECK(fread(pad, 1, 2, f) == 2);
   CHECK(pad[0] == 0 && pad[1] == 0);

   /* Second entry: RAretroarch. */
   uint32_t e2_len;
   CHECK(fread(&e2_len, 4, 1, f) == 1);
   /* key "RAretroarch" (11) + \0 (1) + value "{\"pass\":3}\0" (11) = 23 */
   CHECK(e2_len == 23);

   fclose(f);
   remove(path);
}
```

Add the call to `main`.

- [ ] **Step 2: Run, verify it fails** (KVD offset is currently 0).

- [ ] **Step 3: Implement**

Add a helper near the other static helpers in `gfx/drivers/ktx2_writer.c`:

```c
/* Returns total bytes a KVD section would occupy for the given pairs.
 * Each entry is uint32 keyAndValueByteLength + key + \0 + value + 4B pad. */
static size_t ktx2_kvd_total_size(const ktx2_kv_pair_t *pairs, size_t n)
{
   size_t i, total = 0;
   for (i = 0; i < n; i++)
   {
      size_t key_len = strlen(pairs[i].key) + 1;
      size_t kv_len  = key_len + pairs[i].value_len;
      total += 4 + kv_len;
      /* pad to 4-byte alignment between entries (and after last). */
      total = (total + 3u) & ~3u;
   }
   return total;
}

static bool ktx2_write_kvd(FILE *f, const ktx2_kv_pair_t *pairs, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
   {
      size_t key_len = strlen(pairs[i].key) + 1;
      uint32_t kv_len = (uint32_t)(key_len + pairs[i].value_len);
      if (fwrite(&kv_len, 4, 1, f) != 1) return false;
      if (fwrite(pairs[i].key, 1, key_len, f) != key_len) return false;
      if (pairs[i].value_len > 0)
         if (fwrite(pairs[i].value, 1, pairs[i].value_len, f) != pairs[i].value_len) return false;
      /* Pad. */
      uint32_t pos = 4 + (uint32_t)kv_len;
      while (pos & 3u)
      {
         uint8_t z = 0;
         if (fwrite(&z, 1, 1, f) != 1) return false;
         pos++;
      }
   }
   return true;
}
```

Modify the body of `ktx2_write_file` to compute KVD size and write it. Update these lines:

```c
kvd_offset    = dfd_offset + dfd_len;
kvd_len       = ktx2_kvd_total_size(p->kv_pairs, p->kv_pair_count);
image_offset  = ktx2_align_up(kvd_offset + kvd_len, KTX2_IMAGE_ALIGNMENT);
```

And after the DFD write, before the alignment padding loop:

```c
   /* KVD section. */
   if (kvd_len > 0)
      if (!ktx2_write_kvd(f, p->kv_pairs, p->kv_pair_count)) goto io_fail;
```

- [ ] **Step 4: Run, verify it passes**

- [ ] **Step 5: Commit**

```bash
git add gfx/drivers/ktx2_writer.c tests/ktx2_writer_test.c
git commit -m "ktx2_writer: KVD section + 4-byte padding"
```

---

### Task 6: Image data alignment + final correctness test

**Files:**
- Modify: `tests/ktx2_writer_test.c`

The writer already writes pixel bytes after alignment padding (Task 4). Verify image offset is 16-aligned and the bytes round-trip.

- [ ] **Step 1: Add the failing test**

```c
static void test_image_data_roundtrip(void)
{
   const char *path = "/tmp/ktx2_test_image.ktx2";
   uint8_t pixels[8 * 8 * 8];           /* 16x16 R16G16B16A16_SFLOAT-sized buffer */
   size_t i;
   for (i = 0; i < sizeof pixels; i++)
      pixels[i] = (uint8_t)(i * 31u);

   ktx2_write_params_t p = {
      .vk_format     = VK_FORMAT_R16G16B16A16_SFLOAT,
      .width         = 8,
      .height        = 8,
      .pixels        = pixels,
      .pixels_size   = sizeof pixels,
   };
   CHECK(ktx2_write_file(path, &p));

   FILE *f = fopen(path, "rb");
   CHECK(f != NULL); if (!f) return;

   /* Read level index entry to find image offset + length. */
   fseek(f, 12 + KTX2_HEADER_SIZE, SEEK_SET);
   uint64_t lvl_off, lvl_len, lvl_unc;
   CHECK(fread(&lvl_off, 8, 1, f) == 1);
   CHECK(fread(&lvl_len, 8, 1, f) == 1);
   CHECK(fread(&lvl_unc, 8, 1, f) == 1);
   CHECK(lvl_off % 16 == 0);
   CHECK(lvl_len == sizeof pixels);

   uint8_t round[sizeof pixels];
   fseek(f, (long)lvl_off, SEEK_SET);
   CHECK(fread(round, 1, sizeof pixels, f) == sizeof pixels);
   CHECK(memcmp(round, pixels, sizeof pixels) == 0);

   fclose(f);
   remove(path);
}
```

Note: `KTX2_HEADER_SIZE` isn't exposed in the header. Either add it to the public header or inline the value `80` into the test. Using the value:

Change `12 + KTX2_HEADER_SIZE` to `12 + 80`.

Add the call to `main`.

- [ ] **Step 2: Run, verify it passes** — the writer already handles this correctly.

- [ ] **Step 3: Commit**

```bash
git add tests/ktx2_writer_test.c
git commit -m "ktx2_writer: image data alignment regression test"
```

---

### Task 7: Makefile test target + Vulkan-block build hook

**Files:**
- Modify: `Makefile.common`

- [ ] **Step 1: Add the writer to the Vulkan OBJ list**

In `Makefile.common`, the `HAVE_VULKAN` block at line ~1726 currently contains:

```make
   OBJ += gfx/drivers/vulkan.o \
          gfx/common/vulkan_common.o \
          $(LIBRETRO_COMM_DIR)/vulkan/vulkan_symbol_wrapper.o
```

Add `gfx/drivers/ktx2_writer.o`:

```make
   OBJ += gfx/drivers/vulkan.o \
          gfx/common/vulkan_common.o \
          gfx/drivers/ktx2_writer.o \
          $(LIBRETRO_COMM_DIR)/vulkan/vulkan_symbol_wrapper.o
```

- [ ] **Step 2: Add a Makefile target for the test**

At the end of `Makefile.common`, add:

```make
.PHONY: ktx2_writer_test
ktx2_writer_test:
	$(Q)$(CC) -std=c99 -Wall -Wextra -O0 -g \
	   -I . -I deps/Vulkan-Headers/include \
	   tests/ktx2_writer_test.c gfx/drivers/ktx2_writer.c \
	   -o /tmp/ktx2_writer_test
	$(Q)/tmp/ktx2_writer_test
```

If `Makefile.common` already provides `$(Q)`, use it; otherwise drop it.

- [ ] **Step 3: Verify the main RetroArch build still passes**

Run from the repo root:

```
./configure --disable-qt
make -j10 ktx2_writer_test
make -j10
```

Expected: `all tests passed` from the test target, and the main `make` completes (it now compiles `ktx2_writer.o` into the binary even though nothing yet calls it — should be a clean unused-symbol situation).

- [ ] **Step 4: Commit**

```bash
git add Makefile.common
git commit -m "build: compile ktx2_writer under HAVE_VULKAN + add test target"
```

---

## Phase 2 — Filter Chain Accessors (Task 8)

### Task 8: Read-only accessors on `vulkan_filter_chain`

**Files:**
- Modify: `gfx/drivers_shader/shader_vulkan.h`
- Modify: `gfx/drivers_shader/shader_vulkan.cpp`

No TDD step — accessors are read-only getters that have no behavior to test except via the integration in Phase 4. We verify by checking the build compiles.

- [ ] **Step 1: Add prototypes to the public C API header**

In `gfx/drivers_shader/shader_vulkan.h`, find the existing `vulkan_filter_chain_*` extern-"C" prototype block (after the struct typedefs, around the existing `vulkan_filter_chain_set_input_texture` / `vulkan_filter_chain_build_offscreen_passes` lines). Append:

```c
/* Read-only accessors for offline tooling (e.g. pass-dump debug feature).
 * All return safe defaults when chain is NULL. */
unsigned       vulkan_filter_chain_get_pass_count(vulkan_filter_chain_t *chain);
VkImage        vulkan_filter_chain_get_pass_image(vulkan_filter_chain_t *chain, unsigned i);
VkFormat       vulkan_filter_chain_get_pass_format(vulkan_filter_chain_t *chain, unsigned i);
VkExtent2D     vulkan_filter_chain_get_pass_extent(vulkan_filter_chain_t *chain, unsigned i);
const char    *vulkan_filter_chain_get_pass_name(vulkan_filter_chain_t *chain, unsigned i);
VkImage        vulkan_filter_chain_get_original_image(vulkan_filter_chain_t *chain);
VkFormat       vulkan_filter_chain_get_original_format(vulkan_filter_chain_t *chain);
VkExtent2D     vulkan_filter_chain_get_original_extent(vulkan_filter_chain_t *chain);
VkImageLayout  vulkan_filter_chain_get_original_layout(vulkan_filter_chain_t *chain);
```

- [ ] **Step 2: Add implementations**

In `gfx/drivers_shader/shader_vulkan.cpp`, locate the existing C-API block at the bottom of the file (search for `vulkan_filter_chain_build_viewport_pass`'s extern-"C" wrapper around line 4380 — it's outside the `vulkan_filter_chain` struct, in the C-linkage block). Append at the end of that block (before the closing `}` of `extern "C"` if present, otherwise just after the existing wrappers):

```cpp
unsigned vulkan_filter_chain_get_pass_count(vulkan_filter_chain_t *chain)
{
   if (!chain)
      return 0;
   return (unsigned)chain->passes.size();
}

VkImage vulkan_filter_chain_get_pass_image(vulkan_filter_chain_t *chain, unsigned i)
{
   if (!chain || i >= chain->passes.size())
      return VK_NULL_HANDLE;
   return chain->passes[i]->get_framebuffer().get_image();
}

VkFormat vulkan_filter_chain_get_pass_format(vulkan_filter_chain_t *chain, unsigned i)
{
   if (!chain || i >= chain->passes.size())
      return VK_FORMAT_UNDEFINED;
   return chain->passes[i]->get_framebuffer().get_format();
}

VkExtent2D vulkan_filter_chain_get_pass_extent(vulkan_filter_chain_t *chain, unsigned i)
{
   VkExtent2D e = { 0, 0 };
   if (!chain || i >= chain->passes.size())
      return e;
   auto sz = chain->passes[i]->get_framebuffer().get_size();
   e.width  = sz.width;
   e.height = sz.height;
   return e;
}

const char *vulkan_filter_chain_get_pass_name(vulkan_filter_chain_t *chain, unsigned i)
{
   if (!chain || i >= chain->passes.size())
      return "";
   video_shader *vs = chain->get_shader_preset();
   if (!vs || i >= vs->passes)
      return "";
   if (vs->pass[i].alias[0])
      return vs->pass[i].alias;
   /* Fall back to shader basename without extension. */
   return vs->pass[i].source.path;  /* full path; caller is expected to basename + strip ext */
}

VkImage vulkan_filter_chain_get_original_image(vulkan_filter_chain_t *chain)
{
   if (!chain)
      return VK_NULL_HANDLE;
   return chain->input_texture.image;
}

VkFormat vulkan_filter_chain_get_original_format(vulkan_filter_chain_t *chain)
{
   if (!chain)
      return VK_FORMAT_UNDEFINED;
   return chain->input_texture.format;
}

VkExtent2D vulkan_filter_chain_get_original_extent(vulkan_filter_chain_t *chain)
{
   VkExtent2D e = { 0, 0 };
   if (!chain)
      return e;
   e.width  = chain->input_texture.width;
   e.height = chain->input_texture.height;
   return e;
}

VkImageLayout vulkan_filter_chain_get_original_layout(vulkan_filter_chain_t *chain)
{
   if (!chain)
      return VK_IMAGE_LAYOUT_UNDEFINED;
   return chain->input_texture.layout;
}
```

The `pass_name` accessor returns the raw `source.path` as fallback. The dump module is responsible for basename + extension stripping + character sanitization (Task 13).

The `chain->input_texture` and `chain->passes` are private members of a struct (not a class), so they're effectively public — but verify by reading lines 753–840 of `shader_vulkan.cpp`. If `passes` is declared `private:` in newer code, change the accessors to `chain->get_pass_count()` style — and add a `passes` accessor on the struct first. As of this plan's writing (commit `b69dce59aa`), the struct is C-style with no access modifier, so direct access works.

- [ ] **Step 3: Build and verify**

```
make -j10
```

Expected: clean build, no warnings beyond pre-existing ones.

- [ ] **Step 4: Commit**

```bash
git add gfx/drivers_shader/shader_vulkan.h gfx/drivers_shader/shader_vulkan.cpp
git commit -m "shader_vulkan: read-only accessors for pass dump tooling"
```

---

## Phase 3 — Pass Dump Module (Tasks 9–13)

### Task 9: Module skeleton + arm allocation

**Files:**
- Create: `gfx/drivers/vulkan_pass_dump.h`
- Create: `gfx/drivers/vulkan_pass_dump.c`
- Modify: `Makefile.common`

- [ ] **Step 1: Write the public header**

```c
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
} vulkan_pass_dump_ctx_t;

vulkan_pass_dump_t *vulkan_pass_dump_arm(
      const vulkan_pass_dump_ctx_t *ctx,
      vulkan_filter_chain_t *chain,
      const char *screenshot_dir,
      const char *core_short_name,
      const char *preset_path);

void vulkan_pass_dump_record_offscreen(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      vulkan_filter_chain_t *chain);

void vulkan_pass_dump_record_final(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
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
```

- [ ] **Step 2: Write the module skeleton (arm + free + active + stubs)**

```c
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

   if (!in || !*in)
   {
      snprintf(out, out_size, "unnamed");
      return;
   }

   /* Strip leading directory components. */
   base = strrchr(in, '/');
   if (!base) base = strrchr(in, '\\');
   if (base) base++; else base = in;

   /* Find last dot to strip extension. */
   dot = strrchr(base, '.');

   for (size_t i = 0; base[i] && (!dot || &base[i] < dot) && j + 1 < out_size && j < 32; i++)
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

/* Allocate one host-visible staging buffer sized for the image.
 * Returns true on success. */
static bool alloc_staging(const vulkan_pass_dump_ctx_t *ctx,
      VkDeviceSize size, VkBuffer *out_buf, VkDeviceMemory *out_mem, void **out_map)
{
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
   bci.size        = size;
   bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

   if (vkCreateBuffer(ctx->device, &bci, NULL, out_buf) != VK_SUCCESS)
      return false;

   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(ctx->device, *out_buf, &req);

   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   mai.allocationSize = req.size;
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
   /* Last pass is the "final" pass; we count it separately later. */
   if (offscreen >= 1)
      offscreen -= 1;

   if (vulkan_filter_chain_get_original_image(chain) == VK_NULL_HANDLE)
   {
      RARCH_WARN("[Pass Dump] No Original image bound on chain.\n");
      return NULL;
   }

   vulkan_pass_dump_t *d = (vulkan_pass_dump_t*)calloc(1, sizeof(*d));
   if (!d)
      return NULL;
   d->ctx              = *ctx;
   d->offscreen_count  = offscreen;
   d->num_images       = 1 + offscreen + 1;     /* Original + offscreen + final */
   d->frame_count      = ctx->frame_count;
   format_iso_time(d->capture_time_iso, sizeof d->capture_time_iso);

   /* Build output directory path. */
   char timestamp[32];
   format_filename_timestamp(timestamp, sizeof timestamp);
   char subdir[256];
   snprintf(subdir, sizeof subdir, "%s_%s",
         (core_short_name && *core_short_name) ? core_short_name : "nocore",
         timestamp);
   char base[1024];
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
      p->src_image    = vulkan_filter_chain_get_original_image(chain);
      p->format       = vulkan_filter_chain_get_original_format(chain);
      p->extent       = vulkan_filter_chain_get_original_extent(chain);
      p->prev_layout  = vulkan_filter_chain_get_original_layout(chain);
      p->pass_index   = -1;

      size_t bpt = ktx2_bytes_per_texel(p->format);
      if (bpt == 0)
      {
         RARCH_WARN("[Pass Dump] Unknown Original VkFormat %d.\n", (int)p->format);
         /* Best-effort: assume 4 bytes/texel so blit succeeds. */
         bpt = 4;
      }
      p->staging_size = (VkDeviceSize)p->extent.width * p->extent.height * bpt;
      if (!alloc_staging(ctx, p->staging_size, &p->staging, &p->memory, &p->mapped))
      {
         RARCH_WARN("[Pass Dump] Original staging alloc failed.\n");
         goto fail;
      }

      p->out_filename = (char*)malloc(32);
      snprintf(p->out_filename, 32, "00_original.ktx2");
      strlcpy(p->display_name, "original", sizeof p->display_name);
   }

   /* Slots 1..offscreen_count = offscreen passes. */
   for (i = 0; i < offscreen; i++)
   {
      pass_image_t *p = &d->images[1 + i];
      p->src_image    = vulkan_filter_chain_get_pass_image(chain, i);
      p->format       = vulkan_filter_chain_get_pass_format(chain, i);
      p->extent       = vulkan_filter_chain_get_pass_extent(chain, i);
      p->prev_layout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  /* chain invariant */
      p->pass_index   = (int)i;

      size_t bpt = ktx2_bytes_per_texel(p->format);
      if (bpt == 0) bpt = 8;   /* worst-case 64-bit; over-allocates safely */
      p->staging_size = (VkDeviceSize)p->extent.width * p->extent.height * bpt;
      if (!alloc_staging(ctx, p->staging_size, &p->staging, &p->memory, &p->mapped))
      {
         RARCH_WARN("[Pass Dump] Pass %u staging alloc failed.\n", i);
         goto fail;
      }

      const char *raw = vulkan_filter_chain_get_pass_name(chain, i);
      sanitize_pass_name(raw, p->display_name, sizeof p->display_name);
      p->out_filename = (char*)malloc(96);
      snprintf(p->out_filename, 96, "%02u_pass%02u_%s.ktx2", 1u + i, i, p->display_name);
   }

   /* Slot num_images-1 = final (filled at record_final time once we know
    * the swapchain image/format/extent).  Staging buffer allocated lazily
    * because the swapchain image extent may not be known yet here in the
    * future, but in current code it is — the driver tells us in
    * vulkan_pass_dump_record_final. */

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

/* Stubs filled in subsequent tasks. */
void vulkan_pass_dump_record_offscreen(
      vulkan_pass_dump_t *dump, VkCommandBuffer cmd, vulkan_filter_chain_t *chain)
{
   (void)dump; (void)cmd; (void)chain;
}

void vulkan_pass_dump_record_final(
      vulkan_pass_dump_t *dump, VkCommandBuffer cmd, VkImage img, VkFormat fmt, VkExtent2D ext)
{
   (void)dump; (void)cmd; (void)img; (void)fmt; (void)ext;
}

void vulkan_pass_dump_flush(vulkan_pass_dump_t *dump)
{
   /* For Task 9, just free. Real implementation lands in Task 12. */
   vulkan_pass_dump_free(dump);
}
```

- [ ] **Step 3: Add to Makefile.common**

In the same `HAVE_VULKAN` block where `ktx2_writer.o` was added:

```make
   OBJ += gfx/drivers/vulkan.o \
          gfx/common/vulkan_common.o \
          gfx/drivers/ktx2_writer.o \
          gfx/drivers/vulkan_pass_dump.o \
          $(LIBRETRO_COMM_DIR)/vulkan/vulkan_symbol_wrapper.o
```

- [ ] **Step 4: Build**

```
make -j10
```

Expected: clean build. `vulkan_pass_dump.o` is built but unused (no caller yet) — that's fine, no link-time warning since it's a library archive.

- [ ] **Step 5: Commit**

```bash
git add gfx/drivers/vulkan_pass_dump.h gfx/drivers/vulkan_pass_dump.c Makefile.common
git commit -m "vulkan_pass_dump: module skeleton + arm with staging allocation"
```

---

### Task 10: Record offscreen pass blits

**Files:**
- Modify: `gfx/drivers/vulkan_pass_dump.c`

- [ ] **Step 1: Implement `vulkan_pass_dump_record_offscreen`**

Replace the stub in `gfx/drivers/vulkan_pass_dump.c`:

```c
static void record_one_image_to_staging(
      VkCommandBuffer cmd,
      const pass_image_t *p,
      VkImageLayout src_layout_in,
      VkImageLayout dst_layout_out,
      VkImageAspectFlags aspect)
{
   /* Transition: src_layout_in -> TRANSFER_SRC_OPTIMAL */
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
   b.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
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
   VkBufferImageCopy r = {0};
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
   b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
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

   if (!dump || dump->aborted || dump->offscreen_recorded || !chain)
      return;

   /* Sanity: pass count must match what we armed with. */
   unsigned now_offscreen = vulkan_filter_chain_get_pass_count(chain);
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
```

- [ ] **Step 2: Build and verify no compile errors**

```
make -j10
```

- [ ] **Step 3: Commit**

```bash
git add gfx/drivers/vulkan_pass_dump.c
git commit -m "vulkan_pass_dump: record offscreen pass + Original blits"
```

---

### Task 11: Record final pass (swapchain) blit

**Files:**
- Modify: `gfx/drivers/vulkan_pass_dump.c`

- [ ] **Step 1: Implement `vulkan_pass_dump_record_final`**

Replace the stub:

```c
void vulkan_pass_dump_record_final(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      VkImage swapchain_image,
      VkFormat swapchain_format,
      VkExtent2D swapchain_extent)
{
   pass_image_t *p;

   if (!dump || dump->aborted || dump->final_recorded || !dump->offscreen_recorded)
      return;

   p = &dump->images[dump->num_images - 1];

   /* Lazy alloc — sized to swapchain. */
   size_t bpt = ktx2_bytes_per_texel(swapchain_format);
   if (bpt == 0) bpt = 8;
   p->src_image    = swapchain_image;
   p->format       = swapchain_format;
   p->extent       = swapchain_extent;
   p->prev_layout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   p->pass_index   = (int)dump->offscreen_count;     /* zero-indexed; final is the last pass */
   p->staging_size = (VkDeviceSize)swapchain_extent.width * swapchain_extent.height * bpt;

   if (!alloc_staging(&dump->ctx, p->staging_size,
            &p->staging, &p->memory, &p->mapped))
   {
      RARCH_WARN("[Pass Dump] Final-pass staging alloc failed; aborting.\n");
      dump->aborted = true;
      return;
   }

   /* Compose filename — needs preset's last pass name. The chain passed
    * into arm() owned the name list, but we no longer have it here.
    * Instead we name it based on the offscreen_count slot (i.e. last
    * pass index = offscreen_count, since num_passes = offscreen_count+1). */
   p->out_filename = (char*)malloc(96);
   snprintf(p->out_filename, 96, "%02u_pass%02u_final.ktx2",
         dump->num_images - 1u, dump->offscreen_count);

   record_one_image_to_staging(cmd, p,
         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         VK_IMAGE_ASPECT_COLOR_BIT);
   p->recorded = true;

   /* Record swapchain format name for manifest. */
   snprintf(dump->swapchain_format_name, sizeof dump->swapchain_format_name,
         "VkFormat(%d)", (int)swapchain_format);

   dump->final_recorded = true;
}
```

The format-name string uses `VkFormat(N)` as a fallback. The manifest section in Task 12 has the canonical name table; we'll update this call site once that table exists.

- [ ] **Step 2: Build and verify**

```
make -j10
```

- [ ] **Step 3: Commit**

```bash
git add gfx/drivers/vulkan_pass_dump.c
git commit -m "vulkan_pass_dump: record final swapchain pass blit"
```

---

### Task 12: Flush — wait, write KTX2 + manifest

**Files:**
- Modify: `gfx/drivers/vulkan_pass_dump.c`

- [ ] **Step 1: Add VkFormat→name helper**

At file scope in `vulkan_pass_dump.c`:

```c
static const char *vk_format_name(VkFormat f)
{
   switch (f)
   {
      case VK_FORMAT_R8G8B8A8_UNORM:           return "VK_FORMAT_R8G8B8A8_UNORM";
      case VK_FORMAT_R8G8B8A8_SRGB:            return "VK_FORMAT_R8G8B8A8_SRGB";
      case VK_FORMAT_B8G8R8A8_UNORM:           return "VK_FORMAT_B8G8R8A8_UNORM";
      case VK_FORMAT_B8G8R8A8_SRGB:            return "VK_FORMAT_B8G8R8A8_SRGB";
      case VK_FORMAT_R16G16B16A16_UNORM:       return "VK_FORMAT_R16G16B16A16_UNORM";
      case VK_FORMAT_R16G16B16A16_SFLOAT:      return "VK_FORMAT_R16G16B16A16_SFLOAT";
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32:  return "VK_FORMAT_B10G11R11_UFLOAT_PACK32";
      default:                                 return "VkFormat(unknown)";
   }
}
```

Update the final-pass recorder to use it — replace the `snprintf(dump->swapchain_format_name, ...)` line in `vulkan_pass_dump_record_final`:

```c
   strlcpy(dump->swapchain_format_name, vk_format_name(swapchain_format),
         sizeof dump->swapchain_format_name);
```

- [ ] **Step 2: Implement `vulkan_pass_dump_flush`**

Replace the stub:

```c
static void compose_kvd_for(const vulkan_pass_dump_t *d, const pass_image_t *p,
      char *json_out, size_t json_size)
{
   snprintf(json_out, json_size,
         "{\"pass\":%d,\"name\":\"%s\","
         "\"input_extent\":[%u,%u],\"output_extent\":[%u,%u],"
         "\"frame_index\":%llu,\"capture_time\":\"%s\"}",
         p->pass_index,
         p->display_name[0] ? p->display_name : "unnamed",
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

   fill_pathname_join_special(path, d->out_dir, p->out_filename, sizeof path);

   compose_kvd_for(d, p, kvd_json, sizeof kvd_json);

   kvs[0].key       = "KTXorientation";
   kvs[0].value     = "rd";
   kvs[0].value_len = 3;   /* include trailing \0 */
   kvs[1].key       = "RAretroarch";
   kvs[1].value     = kvd_json;
   kvs[1].value_len = strlen(kvd_json) + 1;

   ktx2_write_params_t wp = {
      .vk_format     = p->format,
      .width         = p->extent.width,
      .height        = p->extent.height,
      .pixels        = p->mapped,
      .pixels_size   = (size_t)p->staging_size,
      .kv_pairs      = kvs,
      .kv_pair_count = 2,
   };
   return ktx2_write_file(path, &wp);
}

static void write_manifest(const vulkan_pass_dump_t *d)
{
   char path[2048];
   FILE *f;
   unsigned i;

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
         "  \"preset_path\": \"%s\",\n"
         "  \"num_passes\": %u,\n"
         "  \"swapchain_format\": \"%s\",\n"
         "  \"frame_index\": %llu,\n"
         "  \"capture_time\": \"%s\",\n"
         "  \"files\": [\n",
         d->preset_path[0] ? d->preset_path : "",
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
   if (!d) return;

   if (d->aborted || !d->offscreen_recorded)
   {
      RARCH_WARN("[Pass Dump] Discarding aborted or never-recorded dump.\n");
      vulkan_pass_dump_free(d);
      return;
   }

   /* HOST_COHERENT memory, but call invalidate for portability. */
   VkMappedMemoryRange ranges[64];
   unsigned n_ranges = 0;
   for (i = 0; i < d->num_images && n_ranges < 64; i++)
   {
      if (!d->images[i].memory) continue;
      VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
      r.memory = d->images[i].memory;
      r.offset = 0;
      r.size   = VK_WHOLE_SIZE;
      ranges[n_ranges++] = r;
   }
   if (n_ranges > 0)
      vkInvalidateMappedMemoryRanges(d->ctx.device, n_ranges, ranges);

   /* Write files in slot order: Original first, then passes, then final. */
   for (i = 0; i < d->num_images; i++)
   {
      const pass_image_t *p = &d->images[i];
      if (!p->recorded || !p->mapped) continue;
      if (!write_one_ktx2(d, p))
         RARCH_WARN("[Pass Dump] Failed to write %s\n", p->out_filename);
   }

   write_manifest(d);

   RARCH_LOG("[Pass Dump] Wrote %u files to %s\n", d->num_images, d->out_dir);

   vulkan_pass_dump_free(d);
}
```

- [ ] **Step 3: Build**

```
make -j10
```

- [ ] **Step 4: Commit**

```bash
git add gfx/drivers/vulkan_pass_dump.c
git commit -m "vulkan_pass_dump: flush writes KTX2 + manifest.json"
```

---

### Task 13: Pass-name resolution improvement

`vulkan_pass_dump_record_final` currently names the final file `<idx>_pass<idx>_final.ktx2` without the pass alias. To get the alias, the dump module needs the chain when recording the final pass. Adjust the API.

**Files:**
- Modify: `gfx/drivers/vulkan_pass_dump.h`
- Modify: `gfx/drivers/vulkan_pass_dump.c`
- Modify: callers (none yet — wiring lands in Task 14, but update the signature now to avoid churn).

- [ ] **Step 1: Adjust the header signature**

In `gfx/drivers/vulkan_pass_dump.h`, change:

```c
void vulkan_pass_dump_record_final(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      VkImage swapchain_image,
      VkFormat swapchain_format,
      VkExtent2D swapchain_extent);
```

to:

```c
void vulkan_pass_dump_record_final(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      vulkan_filter_chain_t *chain,
      VkImage swapchain_image,
      VkFormat swapchain_format,
      VkExtent2D swapchain_extent);
```

- [ ] **Step 2: Use the chain to fetch the final pass name**

In `gfx/drivers/vulkan_pass_dump.c`, update `vulkan_pass_dump_record_final`'s signature and the filename composition:

```c
void vulkan_pass_dump_record_final(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      vulkan_filter_chain_t *chain,
      VkImage swapchain_image,
      VkFormat swapchain_format,
      VkExtent2D swapchain_extent)
{
   /* ... unchanged setup ... */

   /* Replace the filename block: */
   {
      const char *raw = chain
         ? vulkan_filter_chain_get_pass_name(chain, dump->offscreen_count)
         : "";
      sanitize_pass_name(raw, p->display_name, sizeof p->display_name);
      p->out_filename = (char*)malloc(96);
      snprintf(p->out_filename, 96, "%02u_pass%02u_%s_final.ktx2",
            dump->num_images - 1u, dump->offscreen_count, p->display_name);
   }

   /* ... rest unchanged ... */
}
```

- [ ] **Step 3: Build**

```
make -j10
```

- [ ] **Step 4: Commit**

```bash
git add gfx/drivers/vulkan_pass_dump.h gfx/drivers/vulkan_pass_dump.c
git commit -m "vulkan_pass_dump: include chain in record_final for pass naming"
```

---

## Phase 4 — Driver and Input Integration (Tasks 14–17)

### Task 14: Wire arm/record/flush into `vulkan.c`

**Files:**
- Modify: `gfx/drivers/vulkan.c`

- [ ] **Step 1: Add the include and the new vk_t field**

Near the top of `gfx/drivers/vulkan.c` (in the includes block):

```c
#include "vulkan_pass_dump.h"
```

In the `vk_t` struct definition (search for `struct vk { ... };` or `typedef struct vk vk_t;` and find the struct body), add at the end of the struct's fields (before the closing brace):

```c
   /* Slang pass dump (debug, Vulkan-only). NULL when not armed/in-flight. */
   vulkan_pass_dump_t *pass_dump;
   bool                pass_dump_arm_pending;   /* set by hotkey, consumed at top of vulkan_frame */
   bool                pass_dump_unsupported_logged;
```

Find the `vk_t` deinit / destroy function (typically `vulkan_free`) and add cleanup before the existing teardown:

```c
   if (vk->pass_dump)
   {
      vulkan_pass_dump_free(vk->pass_dump);
      vk->pass_dump = NULL;
   }
```

- [ ] **Step 2: Add the public dump-trigger API**

Add a forward declaration with the other vulkan-internal functions at the top of `vulkan.c`:

```c
void vulkan_dump_slang_passes_request(void *data);
```

Implement it (above `video_driver_t video_vulkan = ...`):

```c
void vulkan_dump_slang_passes_request(void *data)
{
   vk_t *vk = (vk_t*)data;
   if (!vk)
      return;
   if (vk->pass_dump)
   {
      RARCH_WARN("[Pass Dump] A previous dump is still in flight; ignoring request.\n");
      return;
   }
   vk->pass_dump_arm_pending = true;
}
```

- [ ] **Step 3: Wire arm at the top of `vulkan_frame`**

In `vulkan_frame()` (the main per-frame entry point — search for `static bool vulkan_frame(`), shortly after the chain has been pumped with `set_input_texture` and `set_frame_count` (i.e. *before* `vulkan_filter_chain_build_offscreen_passes` is called at line 6708), insert:

```c
   /* Arm pass dump if requested this frame. */
   if (vk->pass_dump_arm_pending)
   {
      vk->pass_dump_arm_pending = false;
      runloop_state_t *rls = runloop_state_get_ptr();   /* for core name */
      settings_t *settings = config_get_ptr();
      const char *core_name = (rls && rls->system.info.library_name)
         ? rls->system.info.library_name
         : "nocore";
      const char *preset    = settings->paths.path_shader;

      vulkan_pass_dump_ctx_t ctx = {0};
      ctx.device      = vk->context->device;
      ctx.gpu         = vk->context->gpu;
      ctx.mem_props   = &vk->context->memory_properties;
      ctx.frame_count = video_driver_frame_count_get();   /* or whatever local frame ctr */

      vk->pass_dump = vulkan_pass_dump_arm(&ctx, vk->filter_chain,
            settings->paths.directory_screenshot, core_name, preset);
   }
```

The exact runloop-state / settings / frame-counter accessors may differ — verify by grepping. Replace `runloop_state_get_ptr()->system.info.library_name` with whatever the file already uses (search for `library_name`) and `video_driver_frame_count_get()` with whatever provides `frame_count` (search for `frame_count`).

- [ ] **Step 4: Wire record_offscreen after build_offscreen_passes**

At line 6708 region of `gfx/drivers/vulkan.c`, after the existing call:

```c
   vulkan_filter_chain_build_offscreen_passes(
         vk->filter_chain, vk->cmd, &vp);
```

add:

```c
   if (vk->pass_dump)
      vulkan_pass_dump_record_offscreen(vk->pass_dump, vk->cmd, vk->filter_chain);
```

- [ ] **Step 5: Wire record_final after build_viewport_pass**

At line ~6780 region, after the existing call:

```c
      vulkan_filter_chain_build_viewport_pass(vk->filter_chain,
            vk->cmd, &vp, vk->mvp);
```

add:

```c
   if (vk->pass_dump)
      vulkan_pass_dump_record_final(vk->pass_dump, vk->cmd, vk->filter_chain,
            vk->backbuffer->image,
            vk->context->swapchain_format,
            (VkExtent2D){ vk->vp.full_width, vk->vp.full_height });
```

`vk->backbuffer` is a `struct vk_image *` pointing at the active entry of `vk->backbuffers[]` (see `gfx/drivers/vulkan.c:228`). `vk->context->swapchain_format` is the swapchain `VkFormat` (see `gfx/common/vulkan_common.h:209`). The viewport-extent struct may need to come from `vk->vp` (full width/height); confirm by grepping `vp.full_width` near line 6780 of vulkan.c.

- [ ] **Step 6: Wire flush at the top of the next vulkan_frame**

Near the very start of `vulkan_frame`, immediately after `vk` is dereferenced and basic null-checks done:

```c
   if (vk->pass_dump)
   {
      /* Wait on the prior frame's fence so HOST_COHERENT staging is ready. */
      vkQueueWaitIdle(vk->context->queue);
      vulkan_pass_dump_flush(vk->pass_dump);
      vk->pass_dump = NULL;
   }
```

`vkQueueWaitIdle` is heavy but only fires the frame after a dump — acceptable for a debug feature. If the existing per-frame fence is exposed in scope (`vk->context->fences[i]`), prefer waiting on that specific fence instead. Implementer should pick whichever is cleaner in the actual code.

- [ ] **Step 7: Build**

```
make -j10
```

- [ ] **Step 8: Commit**

```bash
git add gfx/drivers/vulkan.c
git commit -m "vulkan: wire slang pass dump arm/record/flush into vulkan_frame"
```

---

### Task 15: video_driver dispatch

**Files:**
- Modify: `gfx/video_driver.h`
- Modify: `gfx/video_driver.c`

- [ ] **Step 1: Add prototype**

In `gfx/video_driver.h`, near the other `video_driver_*` prototypes:

```c
/* Slang-shader pass dump (Vulkan only). Logs a notice once per session
 * if the active driver is not Vulkan. */
void video_driver_dump_slang_passes(void);
```

- [ ] **Step 2: Add implementation**

In `gfx/video_driver.c`, append (near other meta-action implementations such as `video_driver_take_screenshot`):

```c
extern video_driver_t video_vulkan;
extern void vulkan_dump_slang_passes_request(void *data);   /* defined in gfx/drivers/vulkan.c */

void video_driver_dump_slang_passes(void)
{
   static bool already_warned = false;
   video_driver_state_t *vs   = video_state_get_ptr();

   if (!vs || !vs->current_video || !vs->data)
   {
      RARCH_WARN("[Pass Dump] No active video driver.\n");
      return;
   }

#if defined(HAVE_VULKAN)
   if (vs->current_video == &video_vulkan)
   {
      vulkan_dump_slang_passes_request(vs->data);
      return;
   }
#endif

   if (!already_warned)
   {
      already_warned = true;
      RARCH_WARN("[Pass Dump] Only supported on the Vulkan video driver (current: %s).\n",
            vs->current_video->ident ? vs->current_video->ident : "(unknown)");
   }
}
```

`video_state_get_ptr()` is declared at `gfx/video_driver.h:1090` and returns a `video_driver_state_t *` whose `current_video` (driver vtable) and `data` (driver-specific state, here `vk_t *`) fields are used above.

- [ ] **Step 3: Build**

```
make -j10
```

- [ ] **Step 4: Commit**

```bash
git add gfx/video_driver.h gfx/video_driver.c
git commit -m "video_driver: add dump_slang_passes dispatch (Vulkan-only)"
```

---

### Task 16: Input action + labels

**Files:**
- Modify: `input/input_defines.h`
- Modify: `configuration.c`
- Modify: `intl/msg_hash_lbl.h`
- Modify: `intl/msg_hash_us.h`
- Modify: `msg_hash.h`
- Modify: `menu/menu_displaylist.c`

- [ ] **Step 1: Add the enum value**

In `input/input_defines.h`, in the same enum block as `RARCH_SCREENSHOT` (around line 172), add the new entry immediately after `RARCH_SCREENSHOT`:

```c
   RARCH_SCREENSHOT,
   RARCH_DUMP_SLANG_PASSES,
```

Adding right after `RARCH_SCREENSHOT` (rather than appending at the tail of the enum) keeps the new value adjacent to its semantic neighbor. Any addition shifts the values of everything that follows it, which may invalidate keybinds saved by older builds — there is no perfectly compatible insert position, so consistency-of-grouping wins.

- [ ] **Step 2: Add to default keybinds**

In `configuration.c` (around line 405), find:

```c
   DECLARE_META_BIND(2, screenshot,            RARCH_SCREENSHOT,             MENU_ENUM_LABEL_VALUE_INPUT_META_SCREENSHOT),
```

Immediately after it, add:

```c
   DECLARE_META_BIND(2, dump_slang_passes,     RARCH_DUMP_SLANG_PASSES,      MENU_ENUM_LABEL_VALUE_INPUT_META_DUMP_SLANG_PASSES),
```

The `2` argument is the meta-bind group; match the surrounding rows. The second argument (`dump_slang_passes`) becomes the cfg-file key (`input_dump_slang_passes`); the third is the runtime action enum from Step 1; the fourth is the menu-label enum added in Steps 4–5 below.

- [ ] **Step 3: Add label hash entry**

In `intl/msg_hash_lbl.h`, in the `MSG_HASH_LABEL_LIST` (or equivalent), find the row for `MENU_ENUM_LABEL_INPUT_META_SCREENSHOT` and add immediately after:

```c
MSG_HASH(MENU_ENUM_LABEL_INPUT_META_DUMP_SLANG_PASSES,
   "input_meta_dump_slang_passes")
```

- [ ] **Step 4: Add English strings**

In `intl/msg_hash_us.h`, find `MENU_ENUM_LABEL_VALUE_INPUT_META_SCREENSHOT` and add after it:

```c
MSG_HASH(MENU_ENUM_LABEL_VALUE_INPUT_META_DUMP_SLANG_PASSES,
   "Dump Slang Shader Passes")
MSG_HASH(MENU_ENUM_SUBLABEL_INPUT_META_DUMP_SLANG_PASSES,
   "Write each slang shader pass's output texture to disk as KTX2 files. "
   "Vulkan video driver only.")
```

- [ ] **Step 5: Add the corresponding enum constant**

In `msg_hash.h` (where `MENU_ENUM_LABEL_*` and `MENU_ENUM_LABEL_VALUE_*` are declared), search for `MENU_ENUM_LABEL_INPUT_META_SCREENSHOT` and `MENU_ENUM_LABEL_VALUE_INPUT_META_SCREENSHOT`. Add:

```c
   MENU_ENUM_LABEL_INPUT_META_DUMP_SLANG_PASSES,
   MENU_ENUM_LABEL_VALUE_INPUT_META_DUMP_SLANG_PASSES,
   MENU_ENUM_SUBLABEL_INPUT_META_DUMP_SLANG_PASSES,
```

— each immediately after its `_SCREENSHOT` analog, preserving enum order discipline.

- [ ] **Step 6: Add to hotkeys menu**

In `menu/menu_displaylist.c`, search for `MENU_ENUM_LABEL_INPUT_META_SCREENSHOT` (in the function that populates the hotkeys submenu). Add immediately after the screenshot entry:

```c
   { MENU_ENUM_LABEL_INPUT_META_DUMP_SLANG_PASSES, PARSE_ONLY_BIND, false },
```

Match the surrounding struct shape — what's shown above is a sketch; copy the exact macro the screenshot row uses.

- [ ] **Step 7: Build**

```
make -j10
```

- [ ] **Step 8: Commit**

```bash
git add input/input_defines.h configuration.c \
        intl/msg_hash_lbl.h intl/msg_hash_us.h msg_hash.h \
        menu/menu_displaylist.c
git commit -m "input: add RARCH_DUMP_SLANG_PASSES meta hotkey (unbound by default)"
```

---

### Task 17: Runloop poll + command dispatch

**Files:**
- Modify: `command.h`
- Modify: `retroarch.c`
- Modify: `runloop.c`

The screenshot hotkey uses `HOTKEY_CHECK(RARCH_SCREENSHOT, CMD_EVENT_TAKE_SCREENSHOT, true, NULL)` which expands to a press-edge detector that calls `command_event(CMD_EVENT_TAKE_SCREENSHOT, NULL)` once per press. The handler lives in `retroarch.c`'s `command_event()` switch. Mirror that exactly.

- [ ] **Step 1: Add the command enum**

In `command.h` near line 82 (where `CMD_EVENT_TAKE_SCREENSHOT` is declared), add immediately after it:

```c
   CMD_EVENT_TAKE_SCREENSHOT,
   CMD_EVENT_DUMP_SLANG_PASSES,
```

- [ ] **Step 2: Add the case handler**

In `retroarch.c` near line 3755 (the existing `case CMD_EVENT_TAKE_SCREENSHOT:` block), add immediately after that case's closing `break;`:

```c
      case CMD_EVENT_DUMP_SLANG_PASSES:
         video_driver_dump_slang_passes();
         break;
```

Add an include at the top of `retroarch.c` if not already present:

```c
#include "gfx/video_driver.h"
```

(Most likely already pulled in transitively — verify with `grep -n video_driver retroarch.c | head`.)

- [ ] **Step 3: Add the HOTKEY_CHECK call**

In `runloop.c` at line 6654, find:

```c
   HOTKEY_CHECK(RARCH_SCREENSHOT, CMD_EVENT_TAKE_SCREENSHOT, true, NULL);
```

Immediately after it, add:

```c
   HOTKEY_CHECK(RARCH_DUMP_SLANG_PASSES, CMD_EVENT_DUMP_SLANG_PASSES, true, NULL);
```

`HOTKEY_CHECK` (defined at `runloop.c:5596`) is a macro that pulls the bit from `current_bits` and dispatches via `command_event` on press-edge. No additional state plumbing needed.

- [ ] **Step 4: Build**

```
make -j10
```

- [ ] **Step 5: Commit**

```bash
git add command.h retroarch.c runloop.c
git commit -m "command: dispatch CMD_EVENT_DUMP_SLANG_PASSES from hotkey"
```

---

## Phase 5 — Verification (Task 18)

### Task 18: Manual integration test

**Pre-requisites:**
- A slang preset with at least 2 passes (e.g., `crt-geom.slangp` from the slang-shaders repo).
- A small ROM and a libretro core (any).
- `ktxinfo` (from `libktx`) installed for verification: `brew install ktx-tools` on macOS.

- [ ] **Step 1: Configure for Vulkan**

In your `retroarch.cfg` (or via the menu before running):

```
video_driver = "vulkan"
video_shader = "/path/to/crt-geom.slangp"
video_shader_enable = "true"
```

- [ ] **Step 2: Bind the hotkey**

Launch RetroArch, go to Settings → Input → Hotkeys → "Dump Slang Shader Passes", bind it to (e.g.) F11.

- [ ] **Step 3: Boot a core + content**

Load a core, load a small ROM. Confirm the shader chain is active (visible CRT effect on screen).

- [ ] **Step 4: Trigger**

Press F11. Within ~1 second a log line should appear:

```
[Pass Dump] Wrote N files to <screenshot_dir>/slang_dump/<core>_<timestamp>/
```

- [ ] **Step 5: Inspect the output**

```
cd <screenshot_dir>/slang_dump/<core>_<timestamp>/
ls -la
ktxinfo 00_original.ktx2
ktxinfo 01_pass00_*.ktx2
ktxinfo 02_pass01_*_final.ktx2
cat manifest.json | python -m json.tool
```

Verify:
- File count = `num_passes + 1` plus `manifest.json`.
- `ktxinfo` reports the expected VkFormat for each (R8G8B8A8_UNORM for Original on most cores; whatever `video_shader_pass.fbo.scale_x/y * scale_type` works out to per pass).
- Per-pass extent matches the preset's scale factors (e.g., 2x upscale → 2x width/height versus the preceding pass).
- `00_original.ktx2` ≠ `01_pass00_*.ktx2` (byte-compare; sanity that the staging buffers weren't aliased):
  ```
  cmp 00_original.ktx2 01_pass00_*.ktx2 && echo "ALIASING BUG" || echo "ok"
  ```
- `manifest.json` `num_passes` matches the preset's pass count.

- [ ] **Step 6: Open in RenderDoc**

`renderdoccmd thumb 01_pass00_*.ktx2 /tmp/thumb.png` (or open the .ktx2 directly in the RenderDoc UI). Should display a recognizable per-pass intermediate.

- [ ] **Step 7: Negative test — no preset**

In RetroArch, disable the shader (`video_shader_enable = false`). Reload. Press F11. Expected log:

```
[Pass Dump] No active slang preset.
```

No files produced.

- [ ] **Step 8: Negative test — wrong driver**

Switch `video_driver = "gl"` (or another non-Vulkan driver). Reload. Press F11. Expected log:

```
[Pass Dump] Only supported on the Vulkan video driver (current: gl).
```

- [ ] **Step 9: Negative test — repeat-press**

With everything armed, mash F11 twice in quick succession. The second press, while the first dump is still in flight, should produce:

```
[Pass Dump] A previous dump is still in flight; ignoring request.
```

- [ ] **Step 10: Document the result in a short verification note**

Write a 5–10 line note to `docs/superpowers/specs/2026-05-27-slang-pass-dump-verified.md` recording: RetroArch git SHA tested, preset used, core used, file count and total size produced, any deviations from the spec found.

- [ ] **Step 11: Commit the verification note (only — no code changes in this task)**

```bash
git add docs/superpowers/specs/2026-05-27-slang-pass-dump-verified.md
git commit -m "docs: verify slang pass dump on real preset"
```

---

## Self-Review Summary

This plan covers every spec section:

| Spec section | Plan task(s) |
|---|---|
| Goals / Non-Goals | Implicit — implementation order respects them |
| Decisions (Vulkan-only, output + Original, hotkey, KTX2, screenshot_dir layout) | Threaded through Tasks 1–17 |
| Architecture (two-layer, three modules) | Tasks 1–7 (writer), Task 8 (chain), Tasks 9–13 (dump module) |
| Files touched | Tasks 7, 8, 14, 15, 16, 17 |
| Chain accessors | Task 8 |
| Dump module API | Tasks 9–13 |
| Data flow (arm → Original → offscreen → final → submit → flush) | Tasks 9, 10, 11, 12, 14 |
| Synchronization (barriers, layout restore, HOST_COHERENT) | Task 10 |
| KTX 2.0 file structure | Tasks 1–6 |
| VkFormat→DFD coverage | Task 4 |
| RAretroarch KVD JSON | Task 12 (compose_kvd_for) |
| On-disk layout + manifest | Task 12 (write_manifest), Task 9 (dir creation) |
| Hotkey wiring + unbound default | Tasks 16, 17 |
| Edge cases — no preset / wrong driver / in-flight / mismatched pass count / disk error | Tasks 9, 10, 15, 14 |
| Testing — unit | Tasks 1–6 |
| Testing — manual recipe | Task 18 |

Open questions: none.
