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
