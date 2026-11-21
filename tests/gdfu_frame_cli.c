/* Host-only CLI: decode+re-encode a hex GDFU frame, mirroring Python round-trip. */
#include "gdfu_frame.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
  if (argc != 3 || strcmp(argv[1], "roundtrip") != 0)
  {
    fprintf(stderr, "usage: %s roundtrip <hex>\n", argv[0]);
    return 2;
  }

  const char *hex = argv[2];
  size_t hex_len = strlen(hex);
  if (hex_len < 2 || (hex_len % 2) != 0)
  {
    printf("invalid\n");
    return 0;
  }

  uint8_t raw[GDFU_MAX_FRAME_SIZE];
  size_t len = hex_len / 2;
  if (len > sizeof(raw))
  {
    printf("invalid\n");
    return 0;
  }
  for (size_t i = 0; i < len; ++i)
  {
    unsigned byte = 0;
    if (sscanf(hex + 2 * i, "%2x", &byte) != 1)
    {
      printf("invalid\n");
      return 0;
    }
    raw[i] = (uint8_t)byte;
  }

  gdfu_frame_t frame;
  if (!gdfu_frame_decode(raw, len, &frame))
  {
    printf("invalid\n");
    return 0;
  }

  uint8_t out[GDFU_MAX_FRAME_SIZE];
  size_t out_len = 0;
  if (!gdfu_frame_encode(&frame, out, sizeof(out), &out_len))
  {
    printf("invalid\n");
    return 0;
  }

  for (size_t i = 0; i < out_len; ++i)
  {
    printf("%02x", out[i]);
  }
  printf("\n");
  return 0;
}
