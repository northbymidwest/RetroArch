# Slang Shader Pass Dump — Design

## Summary

Add a debug feature to RetroArch that dumps every texture produced by the
slang shader chain for a single frame, on hotkey press. Each pass's color
output and the raw emulator frame ("Original") are written as separate
KTX 2.0 files. Vulkan backend only.

## Goals

- Single-frame, on-demand capture triggered by a user-bound hotkey.
- Per-pass output of the slang chain, lossless, in the pass's native
  `VkFormat` (no tonemap, no sRGB conversion, no quantization).
- One KTX 2.0 file per image so existing tooling (`ktxinfo`, RenderDoc,
  `libktx`) reads them with no custom loader.
- Minimal disruption to `gfx/drivers_shader/shader_vulkan.cpp`: the slang
  chain only exposes read-only accessors; orchestration lives in the
  driver.
- No measurable perf cost on frames where the dump is not armed.

## Non-Goals

- Multi-frame / video capture.
- Dumping LUTs, Feedback, History, or PassN aliases (those are derivable
  from the pass outputs we already capture).
- Coverage for the Metal, GL3, D3D11, or D3D12 backends. The input
  action is plumbed at the runloop level so other drivers can implement
  the same hook later without a public API change.
- Any form of supercompression or block compression in the KTX2 output.

## Scope decisions (recorded from brainstorming)

| Question | Decision |
|---|---|
| Backend coverage | Vulkan only |
| What to capture | Each pass's color output + the Original emulator frame |
| Trigger | Hotkey (new input action, unbound by default) |
| File format | KTX 2.0, no supercompression, one file per image |
| Output path | `<screenshot_directory>/slang_dump/<core>_<timestamp>/` |

## Architecture

Two layers:

1. **Slang chain (`gfx/drivers_shader/shader_vulkan.{cpp,h}`)** exposes
   read-only accessors so an external consumer can inspect the per-pass
   render targets. No behavior change to the rendering path.
2. **Driver (`gfx/drivers/vulkan.c`)** drives the capture: when armed,
   it inserts blit-to-buffer commands into the same command buffer the
   chain is recording into, then writes the staging buffers to disk
   after the frame's fence signals.

A new module `gfx/drivers/vulkan_pass_dump.{c,h}` owns: arm/disarm state,
per-pass staging-buffer allocation, the KTX 2.0 writer, manifest
emission, and filesystem I/O. Approximate size: 400 LOC.

### Files touched

| File | Change |
|---|---|
| `gfx/drivers_shader/shader_vulkan.h` | New accessor prototypes |
| `gfx/drivers_shader/shader_vulkan.cpp` | Accessor implementations |
| `gfx/drivers/vulkan_pass_dump.h` | **New** — public API of dump module |
| `gfx/drivers/vulkan_pass_dump.c` | **New** — arm/record/flush + KTX2 writer |
| `gfx/drivers/vulkan.c` | Wire arm-trigger + record/flush calls into `vulkan_frame()` |
| `input/input_defines.h` | New `RARCH_DUMP_SLANG_PASSES` action |
| `input/input_driver.{c,h}` | Default keybind table entry (unbound) |
| `intl/msg_hash_us.h` + lbl tables | Menu/label strings |
| `runloop.c` | Poll the new action; call `video_driver_dump_slang_passes()` |
| `gfx/video_driver.{c,h}` | New `video_driver_dump_slang_passes()` dispatch |
| `Makefile.common` | Compile the new files when `HAVE_VULKAN` is set |
| `tests/vulkan_pass_dump_test.c` | **New** — KTX2 writer unit test (opt-in) |

### Chain accessors (read-only)

```c
unsigned    vulkan_filter_chain_get_pass_count(vulkan_filter_chain_t *chain);
VkImage     vulkan_filter_chain_get_pass_image(vulkan_filter_chain_t *chain, unsigned i);
VkFormat    vulkan_filter_chain_get_pass_format(vulkan_filter_chain_t *chain, unsigned i);
VkExtent2D  vulkan_filter_chain_get_pass_extent(vulkan_filter_chain_t *chain, unsigned i);
const char *vulkan_filter_chain_get_pass_name(vulkan_filter_chain_t *chain, unsigned i);
VkImage       vulkan_filter_chain_get_original_image(vulkan_filter_chain_t *chain);
VkFormat      vulkan_filter_chain_get_original_format(vulkan_filter_chain_t *chain);
VkExtent2D    vulkan_filter_chain_get_original_extent(vulkan_filter_chain_t *chain);
VkImageLayout vulkan_filter_chain_get_original_layout(vulkan_filter_chain_t *chain);

/* Per-pass framebuffer color attachments end build_offscreen_passes in
 * SHADER_READ_ONLY_OPTIMAL by chain construction, so no per-pass layout
 * accessor is needed — the dump module hard-codes that as the
 * restore-target after each blit. */
```

These read directly from `passes[i]->framebuffer->image` and the
chain-stored original-input texture. No state changes, no synchronization.

Pass `i` covers all offscreen passes (`0 .. num_passes-2`); the final
pass (`num_passes-1`) renders to the swapchain image, which the driver
already owns, so it is captured separately and not exposed through
`get_pass_image()`.

### Dump module public API

```c
/* gfx/drivers/vulkan_pass_dump.h */

typedef struct vulkan_pass_dump vulkan_pass_dump_t;

/* Allocate state + staging buffers from chain metadata + driver context.
 * Returns NULL and logs on failure (no preset, allocation, etc.). */
vulkan_pass_dump_t *vulkan_pass_dump_arm(
      vk_t *vk,
      vulkan_filter_chain_t *chain,
      const char *screenshot_dir,
      const char *core_name);

/* Record per-pass and Original blits onto `cmd`. Call after the chain's
 * build_offscreen_passes returns and before build_viewport_pass starts. */
void vulkan_pass_dump_record_offscreen(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd);

/* Record swapchain-image blit onto `cmd`. Call after build_viewport_pass
 * returns and before the present transition. */
void vulkan_pass_dump_record_final(
      vulkan_pass_dump_t *dump,
      VkCommandBuffer cmd,
      VkImage swapchain_image,
      VkFormat swapchain_format,
      VkExtent2D swapchain_extent);

/* Wait on fence, map buffers, write KTX2 files, free state. Idempotent
 * after first call. Driver calls this from the *next* vulkan_frame() once
 * the prior frame's fence has signalled. */
void vulkan_pass_dump_flush(vulkan_pass_dump_t *dump);

/* True if a dump is currently armed/in-flight; used to gate hotkey
 * re-press and to gate record_* calls cheaply on non-armed frames. */
bool vulkan_pass_dump_active(const vulkan_pass_dump_t *dump);
```

The driver stores a single `vulkan_pass_dump_t *` on `vk_t` (NULL when
not armed). All `record_*` and `flush` calls early-out if NULL.

## Data flow — one captured frame

Trigger frame (the hotkey was pressed between frame N−1 and N):

1. **Arm** at the top of `vulkan_frame()`:
   - Runloop reports `RARCH_DUMP_SLANG_PASSES` was just pressed.
   - Driver calls `vulkan_pass_dump_arm(vk, chain, dir, core)`.
   - Dump module reads pass count/format/extent + original
     format/extent from the chain. Allocates one host-visible
     `VkBuffer` (`VK_BUFFER_USAGE_TRANSFER_DST_BIT`,
     `HOST_VISIBLE | HOST_COHERENT`) per pass plus one for Original
     plus one for the final swapchain image. Buffer size:
     `extent.width * extent.height * bytes_per_texel(format)`.
     `vkCmdCopyImageToBuffer` with `bufferRowLength = 0` packs rows
     tight — no per-row padding to strip on the CPU side.
   - Records the manifest fields (preset path, num passes, swapchain
     format, video driver name, RetroArch git SHA, frame index, ISO
     timestamp) for the writer.
2. **Original capture.** With the chain command buffer open, dump
   module emits:
   - Barrier on original image: `<current_layout> → TRANSFER_SRC_OPTIMAL`
     where `<current_layout>` is read from the chain's input-texture
     state. For images uploaded each frame this is typically
     `SHADER_READ_ONLY_OPTIMAL`; for the dynamic/HW-rendered path it
     may be `GENERAL`. The implementer queries the chain for the
     actual layout (small accessor add) rather than hard-coding.
   - `vkCmdCopyImageToBuffer` original → original-staging
   - Barrier: `TRANSFER_SRC_OPTIMAL → <current_layout>` (restored)
3. **`build_offscreen_passes(cmd, vp)`** runs normally. Each offscreen
   pass ends with its color attachment in `SHADER_READ_ONLY_OPTIMAL`
   so subsequent passes can sample it.
4. **`vulkan_pass_dump_record_offscreen(dump, cmd)`.** For each pass
   `i` in `0 .. num_passes-2`:
   - Barrier on pass `i` image: `SHADER_READ_ONLY_OPTIMAL → TRANSFER_SRC_OPTIMAL`
   - `vkCmdCopyImageToBuffer` pass `i` image → staging buffer `i`
   - Barrier back to `SHADER_READ_ONLY_OPTIMAL` so the final pass can
     still sample it.
5. **`build_viewport_pass(cmd, vp, mvp)`** runs normally — final pass
   renders into `vk->chain->backbuffer.image` (or equivalent).
6. **`vulkan_pass_dump_record_final(dump, cmd, swapchain_image, fmt, extent)`.**
   - Barrier on swapchain image: `COLOR_ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL`
   - `vkCmdCopyImageToBuffer` swapchain → final-pass staging buffer
   - Barrier back to `COLOR_ATTACHMENT_OPTIMAL` so the existing
     present transition (`COLOR_ATTACHMENT → PRESENT_SRC`) still works.
7. **Submit + present** unchanged.
8. **Frame N+1 in `vulkan_frame()`.** Before recording the new frame's
   commands, driver calls `vulkan_pass_dump_flush(dump)` (which waits
   on the per-frame fence from frame N if needed). Flush:
   - Creates the output subdirectory.
   - Maps each staging buffer.
   - For each image (Original, then passes 0..N−2, then final), invokes
     the KTX2 writer.
   - Writes `manifest.json`.
   - Unmaps + destroys buffers, frees dump state, sets `vk->pass_dump =
     NULL`.

If the hotkey is pressed while `vk->pass_dump != NULL`, the new arm is
rejected with a `RARCH_WARN` line and the existing in-flight dump
continues to completion.

### Synchronization

- All blits ride on the chain's existing command buffer. No extra
  submits, no `vkDeviceWaitIdle`, no extra fences.
- The host-visible buffers are written by the GPU during the frame and
  read by the CPU after the frame's existing fence signals. Since the
  buffers are `HOST_COHERENT`, no explicit `vkInvalidateMappedMemoryRanges`
  is needed; we still call it once before mapping for portability.
- Layout-restoring barriers after each blit are essential because the
  final pass samples earlier offscreen outputs. Skipping them would
  leave images in `TRANSFER_SRC_OPTIMAL` and break sampling.

## File format

### KTX 2.0 subset

We emit a minimal-but-spec-compliant KTX 2.0 file per image:

```
[ 12 B ] identifier — 0xAB 'K' 'T' 'X' ' ' '2' '0' 0xBB 0x0D 0x0A 0x1A 0x0A
[ 68 B ] header
  uint32 vkFormat                  = pass VkFormat
  uint32 typeSize                  = element size for unpacked formats, else 1
  uint32 pixelWidth, pixelHeight   = extent
  uint32 pixelDepth                = 0
  uint32 layerCount                = 0
  uint32 faceCount                 = 1
  uint32 levelCount                = 1
  uint32 supercompressionScheme    = 0 (NONE)
  uint32 dfdByteOffset             = header_end + level_index_size
  uint32 dfdByteLength
  uint32 kvdByteOffset             = dfd_end
  uint32 kvdByteLength
  uint64 sgdByteOffset             = 0
  uint64 sgdByteLength             = 0
[ 24 B ] level index — single entry
  uint64 byteOffset, byteLength, uncompressedByteLength
[ DFD ] Data Format Descriptor — one descriptor block, generated from
        a VkFormat lookup table covering:
          R8G8B8A8_UNORM, R8G8B8A8_SRGB
          B8G8R8A8_UNORM, B8G8R8A8_SRGB
          R16G16B16A16_UNORM, R16G16B16A16_SFLOAT
          A2B10G10R10_UNORM_PACK32
          B10G11R11_UFLOAT_PACK32
        Unrecognized format → empty DFD (single header block, all
        zeroed) + RARCH_WARN. `ktxinfo` still prints `vkFormat` from
        the main header, so the file is forensically usable.
[ KVD ] key/value data, two entries:
  "KTXorientation"  -> "rd\0"        (rightward, downward — Vulkan convention)
  "RAretroarch"     -> compact JSON  (see below)
[ DATA ] mip-0 image data, height × width × bytes_per_texel bytes,
         rows packed tight (no row padding).
```

All padding requirements from the spec are honored: each section is
4-byte aligned, KVD entries are 4-byte aligned with `\0` pad bytes,
and mip data starts at the alignment dictated by the format
(`max(lcm(texel_block_size, 4), 16)` per spec).

### `RAretroarch` KVD value

```json
{"pass": 3, "name": "crt-pass", "source_shader": "crt-geom.slang",
 "input_extent": [256, 224], "output_extent": [1024, 896],
 "frame_index": 1234, "capture_time": "2026-05-27T15:42:09Z"}
```

For the Original file, `"pass": -1`, `"name": "original"`,
`"source_shader": null`. For the final file, `"pass": num_passes-1`,
`"name": "<...>_final"`.

### On-disk layout

```
<screenshot_directory>/slang_dump/
   <core_short>_<YYYY-MM-DD_HH-MM-SS>/
      00_original.ktx2
      01_pass00_<name>.ktx2
      02_pass01_<name>.ktx2
      ...
      NN_pass<N-1>_<name>_final.ktx2
      manifest.json
```

- `<core_short>` is the loaded core's short name with whitespace
  replaced by `_`. If no core is loaded (menu-only): `"nocore"`.
- Pass `<name>` source: `video_shader_pass.alias` if non-empty, else the
  shader filename without extension. Sanitized to `[A-Za-z0-9_-]`.
- `manifest.json`:
  ```json
  {
    "schema": 1,
    "retroarch_version": "1.22.2",
    "retroarch_sha": "b8f9c7cb0e",
    "video_driver": "vulkan",
    "preset_path": "/path/to/preset.slangp",
    "num_passes": 5,
    "swapchain_format": "VK_FORMAT_B8G8R8A8_UNORM",
    "frame_index": 1234,
    "capture_time": "2026-05-27T15:42:09Z",
    "files": [
      {"path": "00_original.ktx2", "pass": -1, "extent": [256, 224], "vk_format": "VK_FORMAT_R8G8B8A8_UNORM"},
      ...
    ]
  }
  ```

## Hotkey wiring

- New enum value `RARCH_DUMP_SLANG_PASSES` added to the
  meta-action block in `input/input_defines.h`, between existing entries
  so we don't reorder existing values.
- Default bind: **unbound** (`RETROK_UNKNOWN`). Rationale: this is debug
  tooling, not an end-user feature; defaulting to a key would risk
  colliding with users' existing binds.
- Label: `MENU_ENUM_LABEL_VALUE_INPUT_META_DUMP_SLANG_PASSES =
  "Dump Slang Shader Passes"`, with a sublabel
  `MENU_ENUM_SUBLABEL_INPUT_META_DUMP_SLANG_PASSES = "Write each slang
  shader pass's output texture to disk as KTX2 files."`. Added to
  `intl/msg_hash_us.h` only — other locales fall back to English until
  Crowdin picks them up.
- Polled in `runloop.c` alongside `RARCH_SCREENSHOT`. On just-pressed,
  call `video_driver_dump_slang_passes()`.
- `video_driver_dump_slang_passes()` (new, in `gfx/video_driver.c`)
  looks up the current driver; if not `&video_vulkan`, logs once
  per session ("Slang pass dump is only supported on the Vulkan video
  driver") and returns. If Vulkan, calls into `vulkan_dump_slang_passes()`
  on the driver, which sets a one-shot flag consumed at the top of the
  next `vulkan_frame()`.

## Edge cases

| Case | Behavior |
|---|---|
| No slang preset active | `arm` fails, logs `[Pass Dump] No active slang preset.` |
| Active driver ≠ Vulkan | `video_driver_dump_slang_passes()` logs once, returns |
| Dump already in flight | Second arm rejected with log line; existing dump continues |
| Pass count/extent changed between arm and record | `record_offscreen` detects mismatch, aborts dump, logs, frees state |
| HDR swapchain (`A2B10G10R10` / `R16G16B16A16_SFLOAT`) | DFD table covers these; raw bytes preserved, no tonemap |
| Unknown VkFormat | Empty DFD + warning; main header still records the format |
| Disk write failure | Per-file error logged; partial dump left as-is |
| Path with spaces/non-ASCII | Uses `fill_pathname_join_special` + `path_mkdir`, same as screenshot path |
| Menu visible during dump | Captures reflect the pure shader chain (menu draws after); intentional |
| `mipmap_input = true` on a pass | Only mip 0 dumped (the pass's own output); mips ≥ 1 are derived |

## Testing

### Unit

`tests/vulkan_pass_dump_test.c` — opt-in target invoked via
`make -f Makefile vulkan_pass_dump_test` (not on the default build).
Tests the KTX 2.0 writer with synthetic inputs:

- Write a 4×4 `R8G8B8A8_UNORM` image with a known byte pattern, parse
  the resulting file byte-by-byte against the spec offsets.
- Write a 16×16 `R16G16B16A16_SFLOAT` image, verify DFD encodes the
  format correctly.
- Write a 1×1 image with an unknown format, verify empty-DFD path and
  warning emission.
- KVD section: write with a known JSON payload, verify alignment
  padding rules.
- If `libktx` headers are detectable at configure time, additionally
  roundtrip each file through `ktxTexture2_CreateFromNamedFile()` and
  assert `vkFormat`/extent match. Otherwise skipped.

### Manual integration

Recipe the implementer runs once the work is complete and reports back
the result of:

1. Load `crt-geom.slangp` (or similar multi-pass preset).
2. Boot a core (any with a small native resolution, e.g. an NES core).
3. Bind the new hotkey via Settings → Input → Hotkeys.
4. Press hotkey. Verify within 1–2 seconds:
   - A new subdirectory appears under
     `<screenshot_dir>/slang_dump/`.
   - File count = `num_passes + 1` (Original + each pass) + `manifest.json`.
   - `ktxinfo 00_original.ktx2` reports the expected core framebuffer
     extent and `R8G8B8A8_UNORM` (or whatever `video_force_format`
     yields).
   - Per-pass extents grow as the preset's scale factors prescribe.
   - Pass 0 output ≠ Pass 1 output ≠ Original (byte-compare; sanity
     that buffers aren't aliased).
   - `RenderDoc` opens each `.ktx2`.
   - `manifest.json` matches the loaded preset's pass count and paths.
5. Negative — re-trigger with no preset; verify warning + no files.
6. Negative — re-trigger on the `gl3` driver; verify "not supported"
   warning + no files.

## Build wiring

In `Makefile.common`, add to the existing Vulkan block:

```make
ifeq ($(HAVE_VULKAN), 1)
   OBJ += gfx/drivers/vulkan_pass_dump.o
endif
```

No new dependencies — the writer is self-contained C using stdio +
existing RetroArch path utilities.

## Open questions

None. All scope/format/trigger questions resolved during brainstorming.

## Implementation order (preview for writing-plans)

1. KTX 2.0 writer + its unit test (no Vulkan deps; testable standalone).
2. Chain accessors in `shader_vulkan.{cpp,h}`.
3. `vulkan_pass_dump.{c,h}` skeleton — arm, allocations, flush stub.
4. Wire `record_offscreen` + `record_final` into `vulkan.c`.
5. Input action + runloop + dispatch.
6. Manifest + filename sanitization.
7. Edge-case handling + log lines.
8. Manual integration test pass on a real preset.
