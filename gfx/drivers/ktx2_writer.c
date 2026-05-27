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
   (void)fmt;
   return 0;  /* filled in Task 2 */
}

bool ktx2_write_file(const char *out_path, const ktx2_write_params_t *p)
{
   (void)out_path; (void)p;
   return false;  /* filled in subsequent tasks */
}
