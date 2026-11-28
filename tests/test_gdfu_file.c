#include "gdfu_file.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gdfu_frame.h"

#define ZIP_CAPACITY 70000u
#define ZIP_ENTRY_CAPACITY 4u

struct zip_entry {
  const char *name;
  const uint8_t *data;
  size_t length;
  uint16_t method;
  uint32_t local_offset;
  uint32_t central_offset;
};

struct zip_builder {
  uint8_t bytes[ZIP_CAPACITY];
  size_t length;
  struct zip_entry entries[ZIP_ENTRY_CAPACITY];
  size_t entry_count;
  uint32_t eocd_offset;
};

static int failures;
static int out_of_range_reads;

#define CHECK(condition) \
  do { \
    if (!(condition)) { \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++failures; \
    } \
  } while (0)

static void put16(struct zip_builder *zip, uint16_t value)
{
  zip->bytes[zip->length++] = (uint8_t)value;
  zip->bytes[zip->length++] = (uint8_t)(value >> 8u);
}

static void put32(struct zip_builder *zip, uint32_t value)
{
  put16(zip, (uint16_t)value);
  put16(zip, (uint16_t)(value >> 16u));
}

static void set16(struct zip_builder *zip, size_t offset, uint16_t value)
{
  zip->bytes[offset] = (uint8_t)value;
  zip->bytes[offset + 1u] = (uint8_t)(value >> 8u);
}

static void set32(struct zip_builder *zip, size_t offset, uint32_t value)
{
  set16(zip, offset, (uint16_t)value);
  set16(zip, offset + 2u, (uint16_t)(value >> 16u));
}

static void add_entry(struct zip_builder *zip, const char *name, const uint8_t *data,
                      size_t length, uint16_t method)
{
  struct zip_entry *entry = &zip->entries[zip->entry_count++];
  size_t name_length = strlen(name);

  entry->name = name;
  entry->data = data;
  entry->length = length;
  entry->method = method;
  entry->local_offset = (uint32_t)zip->length;
  put32(zip, 0x04034b50u);
  put16(zip, 20u);
  put16(zip, 0u);
  put16(zip, method);
  put16(zip, 0u);
  put16(zip, 0u);
  put32(zip, 0u);
  put32(zip, (uint32_t)length);
  put32(zip, (uint32_t)length);
  put16(zip, (uint16_t)name_length);
  put16(zip, 0u);
  memcpy(&zip->bytes[zip->length], name, name_length);
  zip->length += name_length;
  memcpy(&zip->bytes[zip->length], data, length);
  zip->length += length;
}

static void finish_zip(struct zip_builder *zip, size_t comment_length)
{
  uint32_t directory_offset = (uint32_t)zip->length;
  size_t index;

  for (index = 0u; index < zip->entry_count; ++index) {
    struct zip_entry *entry = &zip->entries[index];
    size_t name_length = strlen(entry->name);
    entry->central_offset = (uint32_t)zip->length;
    put32(zip, 0x02014b50u);
    put16(zip, 20u);
    put16(zip, 20u);
    put16(zip, 0u);
    put16(zip, entry->method);
    put16(zip, 0u);
    put16(zip, 0u);
    put32(zip, 0u);
    put32(zip, (uint32_t)entry->length);
    put32(zip, (uint32_t)entry->length);
    put16(zip, (uint16_t)name_length);
    put16(zip, 0u);
    put16(zip, 0u);
    put16(zip, 0u);
    put16(zip, 0u);
    put32(zip, 0u);
    put32(zip, entry->local_offset);
    memcpy(&zip->bytes[zip->length], entry->name, name_length);
    zip->length += name_length;
  }
  uint32_t directory_size = (uint32_t)(zip->length - directory_offset);
  zip->eocd_offset = (uint32_t)zip->length;
  put32(zip, 0x06054b50u);
  put16(zip, 0u);
  put16(zip, 0u);
  put16(zip, (uint16_t)zip->entry_count);
  put16(zip, (uint16_t)zip->entry_count);
  put32(zip, directory_size);
  put32(zip, directory_offset);
  put16(zip, (uint16_t)comment_length);
  memset(&zip->bytes[zip->length], 'c', comment_length);
  zip->length += comment_length;
}

static void make_package(struct zip_builder *zip, const char *manifest,
                         const char *dat_name, const char *dat, const char *bin_name,
                         const char *bin, size_t comment_length)
{
  memset(zip, 0, sizeof(*zip));
  add_entry(zip, "manifest.json", (const uint8_t *)manifest, strlen(manifest), 0u);
  add_entry(zip, dat_name, (const uint8_t *)dat, strlen(dat), 0u);
  add_entry(zip, bin_name, (const uint8_t *)bin, strlen(bin), 0u);
  finish_zip(zip, comment_length);
}

static bool memory_reader(void *context, uint32_t offset, uint8_t *buffer, size_t length)
{
  const struct zip_builder *zip = context;
  if ((uint64_t)offset + length > zip->length) {
    ++out_of_range_reads;
    return false;
  }
  memcpy(buffer, &zip->bytes[offset], length);
  return true;
}

static gdfu_file_err_t validate(const struct zip_builder *zip)
{
  return gdfu_zip_validate_reader(memory_reader, (void *)zip, (uint32_t)zip->length);
}

#define APP_NAMES "\"bin_file\":\"firmware.bin\",\"dat_file\":\"firmware.dat\","

static const char manifest_ok[] =
    "{\"manifest\":{\"application\":{" APP_NAMES "\"init_packet_data\":{\"device_type\":82,"
    "\"device_revision\":65535,\"firmware_crc16\":43514}}}}";

static void manifest_with(char *out, const char *dat_name, const char *bin_name)
{
  sprintf(out,
          "{\"manifest\":{\"application\":{\"bin_file\":\"%s\",\"dat_file\":\"%s\","
          "\"init_packet_data\":{\"device_type\":82,\"device_revision\":65535,"
          "\"firmware_crc16\":43514}}}}",
          bin_name, dat_name);
}

static void test_package_policy(void)
{
  CHECK(GDFU_FILE_DEVICE_TYPE == 82u);
  CHECK(GDFU_FILE_DEVICE_REVISION == 65535u);
  CHECK(GDFU_FILE_ERR_PACKAGE == 12);
}

static void test_nordic_crc16(void)
{
  static const uint8_t input[] = "123456789";
  CHECK(gdfu_crc16_nordic(input, sizeof(input) - 1u) == 0x29b1u);
  CHECK(gdfu_crc16_nordic(NULL, 0u) == 0xffffu);
}

static void test_accepts_exact_stored_package_with_bounded_comment(void)
{
  struct zip_builder zip;
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 65535u);
  CHECK(validate(&zip) == GDFU_FILE_OK);
}

static void test_manifest_names_entries(void)
{
  struct zip_builder zip;
  char manifest[512];
  char long_name[65];
  const char *no_dat =
      "{\"manifest\":{\"application\":{\"bin_file\":\"firmware.bin\",\"init_packet_data\":{"
      "\"device_type\":82,\"device_revision\":65535,\"firmware_crc16\":43514}}}}";

  /* accepted */
  manifest_with(manifest, "app.dat", "app.bin");
  make_package(&zip, manifest, "app.dat", "init", "app.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_OK);

  manifest_with(manifest, "zephyr.dat", "zephyr.bin");
  make_package(&zip, manifest, "zephyr.dat", "init", "zephyr.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_OK);

  memset(long_name, 'b', 63u);
  long_name[63] = '\0';
  manifest_with(manifest, "app.dat", long_name);
  make_package(&zip, manifest, "app.dat", "init", long_name, "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_OK);

  memset(&zip, 0, sizeof(zip));
  add_entry(&zip, "firmware.bin", (const uint8_t *)"firmware image", 14u, 0u);
  add_entry(&zip, "manifest.json", (const uint8_t *)manifest_ok, strlen(manifest_ok), 0u);
  add_entry(&zip, "firmware.dat", (const uint8_t *)"init", 4u, 0u);
  finish_zip(&zip, 0u);
  CHECK(validate(&zip) == GDFU_FILE_OK);

  /* rejected */
  manifest_with(manifest, "firmware.dat", "app.bin");
  make_package(&zip, manifest, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  manifest_with(manifest, "app.bin", "app.bin");
  make_package(&zip, manifest, "app.bin", "init", "app.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  manifest_with(manifest, "app.dat", "manifest.json");
  make_package(&zip, manifest, "app.dat", "init", "manifest.json", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  make_package(&zip, no_dat, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  manifest_with(manifest, "app.dat", "app.bin");
  memcpy(strstr(manifest, "\"app.dat\""), "1        ", 9u);
  make_package(&zip, manifest, "app.dat", "init", "app.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* names only at the top level or in init_packet_data are not application names */
  make_package(&zip,
               "{\"bin_file\":\"firmware.bin\",\"dat_file\":\"firmware.dat\",\"manifest\":{"
               "\"application\":{\"init_packet_data\":{\"device_type\":82,"
               "\"device_revision\":65535,\"firmware_crc16\":43514}}}}",
               "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
  make_package(&zip,
               "{\"manifest\":{\"application\":{\"init_packet_data\":{\"bin_file\":"
               "\"firmware.bin\",\"dat_file\":\"firmware.dat\",\"device_type\":82,"
               "\"device_revision\":65535,\"firmware_crc16\":43514}}}}",
               "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* escaped names are rejected */
  manifest_with(manifest, "app\\u002edat", "app.bin");
  make_package(&zip, manifest, "app.dat", "init", "app.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* 64 bytes does not fit */
  memset(long_name, 'b', 64u);
  long_name[64] = '\0';
  manifest_with(manifest, "app.dat", long_name);
  make_package(&zip, manifest, "app.dat", "init", long_name, "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* case matters */
  manifest_with(manifest, "firmware.dat", "Firmware.bin");
  make_package(&zip, manifest, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* duplicate key */
  make_package(&zip,
               "{\"manifest\":{\"application\":{\"bin_file\":\"firmware.bin\","
               "\"bin_file\":\"firmware.bin\",\"dat_file\":\"firmware.dat\","
               "\"init_packet_data\":{\"device_type\":82,\"device_revision\":65535,"
               "\"firmware_crc16\":43514}}}}",
               "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* zip has app.bin twice */
  manifest_with(manifest, "app.bin", "app.bin");
  memset(&zip, 0, sizeof(zip));
  add_entry(&zip, "manifest.json", (const uint8_t *)manifest, strlen(manifest), 0u);
  add_entry(&zip, "app.bin", (const uint8_t *)"init", 4u, 0u);
  add_entry(&zip, "app.bin", (const uint8_t *)"firmware image", 14u, 0u);
  finish_zip(&zip, 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static void test_accepts_nrfutil_manifest_with_ignored_fields(void)
{
  struct zip_builder zip;
  make_package(&zip,
               "{\n \"manifest\": {\n  \"application\": {\n   \"bin_file\": \"firmware.bin\",\n"
               "   \"dat_file\": \"firmware.dat\",\n   \"init_packet_data\": {\n"
               "    \"application_version\": 4294967295,\n    \"device_revision\": 65535,\n"
               "    \"device_type\": 82,\n    \"firmware_crc16\": 43514,\n"
               "    \"softdevice_req\": [182, 183]\n   }\n  },\n  \"dfu_version\": 0.5\n }\n}\n",
               "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_OK);
}

static void test_rejects_bad_profile_and_manifest_fields(void)
{
  struct zip_builder zip;
  char manifest[sizeof(manifest_ok)];

  memcpy(manifest, manifest_ok, sizeof(manifest));
  memcpy(strstr(manifest, "65535"), "65534", 5u);
  make_package(&zip, manifest, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  make_package(&zip,
               "{\"manifest\":{\"application\":{" APP_NAMES "\"init_packet_data\":{\"device_revision\":65535,\"firmware_crc16\":43514}}}}",
               "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  memcpy(manifest, manifest_ok, sizeof(manifest));
  memcpy(strstr(manifest, "82,"), "81,", 3u);
  make_package(&zip, manifest, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  memcpy(manifest, manifest_ok, sizeof(manifest));
  memcpy(strstr(manifest, "43514"), "43515", 5u);
  make_package(&zip, manifest, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  make_package(&zip,
               "{\"manifest\":{\"application\":{" APP_NAMES "\"init_packet_data\":{\"device_type\":82,\"device_type\":82,\"device_revision\":65535,\"firmware_crc16\":43514}}}}",
               "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static void test_rejects_invalid_zip_structure_and_offsets(void)
{
  struct zip_builder zip;
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  set32(&zip, zip.eocd_offset, 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  set32(&zip, zip.eocd_offset + 16u, (uint32_t)zip.length);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  set32(&zip, zip.entries[0].central_offset + 42u, (uint32_t)zip.length);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  set32(&zip, zip.eocd_offset + 12u, 1u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 1u);
  set16(&zip, zip.eocd_offset + 20u, 2u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static void test_rejects_hostile_sizes_names_and_search_window(void)
{
  struct zip_builder zip;

  /* Declared size runs past the file: must fail before any read of that range. */
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  set32(&zip, zip.entries[2].central_offset + 20u, 0xffffffffu);
  set32(&zip, zip.entries[2].central_offset + 24u, 0xffffffffu);
  set32(&zip, zip.entries[2].local_offset + 18u, 0xffffffffu);
  set32(&zip, zip.entries[2].local_offset + 22u, 0xffffffffu);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* Local header disagrees with the central directory about the name. */
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  zip.bytes[zip.entries[1].local_offset + 30u] = 'X';
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* Central name length points past the directory. */
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  set16(&zip, zip.entries[2].central_offset + 28u, 15u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* Local extra length pushes data past the central directory. */
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  set16(&zip, zip.entries[2].local_offset + 28u, 500u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* Encrypted/data-descriptor flag is outside the accepted profile. */
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  set16(&zip, zip.entries[0].central_offset + 8u, 8u);
  set16(&zip, zip.entries[0].local_offset + 6u, 8u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* Truncated archive and tiny inputs have no EOCD. */
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  zip.length = zip.eocd_offset + 10u;
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
  zip.length = 3u;
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
  zip.length = 0u;
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* An EOCD buried deeper than 22 + 65535 bytes from the end is never searched for. */
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  memset(&zip.bytes[zip.length], 0, 65536u);
  zip.length += 65536u;
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static void test_rejects_empty_bin_even_when_manifest_crc_matches_empty(void)
{
  struct zip_builder zip;
  /* CRC16 of zero bytes is the 0xffff seed, so only the explicit empty check can reject this. */
  make_package(&zip,
               "{\"manifest\":{\"application\":{" APP_NAMES "\"init_packet_data\":{\"device_type\":82,"
               "\"device_revision\":65535,\"firmware_crc16\":65535}}}}",
               "firmware.dat", "init", "firmware.bin", "", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static void test_rejects_data_overlapping_central_directory(void)
{
  struct zip_builder zip;
  /* firmware.dat is the last local record and is not CRC-checked, so only the bound can reject it. */
  memset(&zip, 0, sizeof(zip));
  add_entry(&zip, "manifest.json", (const uint8_t *)manifest_ok, strlen(manifest_ok), 0u);
  add_entry(&zip, "firmware.bin", (const uint8_t *)"firmware image", 14u, 0u);
  add_entry(&zip, "firmware.dat", (const uint8_t *)"init", 4u, 0u);
  finish_zip(&zip, 0u);
  CHECK(validate(&zip) == GDFU_FILE_OK);
  set32(&zip, zip.entries[2].central_offset + 20u, 20u);
  set32(&zip, zip.entries[2].central_offset + 24u, 20u);
  set32(&zip, zip.entries[2].local_offset + 18u, 20u);
  set32(&zip, zip.entries[2].local_offset + 22u, 20u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static void test_rejects_stray_bytes_after_last_directory_record(void)
{
  struct zip_builder zip;
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  memmove(&zip.bytes[zip.eocd_offset + 4u], &zip.bytes[zip.eocd_offset], 22u);
  memset(&zip.bytes[zip.eocd_offset], 0, 4u);
  zip.eocd_offset += 4u;
  zip.length += 4u;
  set32(&zip, zip.eocd_offset + 12u, 187u - 12u + 4u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static void test_rejects_nonexact_entries_and_methods(void)
{
  struct zip_builder zip;
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  set16(&zip, zip.entries[0].central_offset + 10u, 8u);
  set16(&zip, zip.entries[0].local_offset + 8u, 8u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  memset(&zip, 0, sizeof(zip));
  add_entry(&zip, "manifest.json", (const uint8_t *)manifest_ok, strlen(manifest_ok), 0u);
  add_entry(&zip, "firmware.dat", (const uint8_t *)"init", 4u, 0u);
  finish_zip(&zip, 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  memset(&zip, 0, sizeof(zip));
  add_entry(&zip, "manifest.json", (const uint8_t *)manifest_ok, strlen(manifest_ok), 0u);
  add_entry(&zip, "firmware.dat", (const uint8_t *)"init", 4u, 0u);
  add_entry(&zip, "firmware.bin", (const uint8_t *)"firmware image", 14u, 0u);
  add_entry(&zip, "firmware.bin", (const uint8_t *)"firmware image", 14u, 0u);
  finish_zip(&zip, 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  memset(&zip, 0, sizeof(zip));
  add_entry(&zip, "manifest.json", (const uint8_t *)manifest_ok, strlen(manifest_ok), 0u);
  add_entry(&zip, "firmware.dat", (const uint8_t *)"init", 4u, 0u);
  add_entry(&zip, "firmware.bin", (const uint8_t *)"firmware image", 14u, 0u);
  add_entry(&zip, "extra.bin", (const uint8_t *)"x", 1u, 0u);
  finish_zip(&zip, 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  memset(&zip, 0, sizeof(zip));
  add_entry(&zip, "manifest.json", (const uint8_t *)manifest_ok, strlen(manifest_ok), 0u);
  add_entry(&zip, "firmware.dat", (const uint8_t *)"init", 4u, 0u);
  add_entry(&zip, "dir/firmware.bin", (const uint8_t *)"firmware image", 14u, 0u);
  finish_zip(&zip, 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static void test_rejects_escaped_or_overlong_manifest_keys(void)
{
  struct zip_builder zip;
  const char *escaped[] = {
      /* \u0078 decodes to "x"; the profile object must not be found via it. */
      "{\"manifest\":{\"application\":{" APP_NAMES "\"init_packet_data\":{},\"\\u0078\":{"
      "\"device_type\":82,\"device_revision\":65535,\"firmware_crc16\":43514}}}}",
      /* prefix-only match on an escaped key must not record a field */
      "{\"manifest\":{\"application\":{" APP_NAMES "\"init_packet_data\":{\"device_type\":82,"
      "\"device_revision\\u0041\":65535,\"firmware_crc16\":43514}}}}",
      /* over-long key: must be rejected, not read as a truncated prefix */
      "{\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\":1,\"manifest\":{\"application\":"
      "{" APP_NAMES "\"init_packet_data\":{\"device_type\":82,\"device_revision\":65535,"
      "\"firmware_crc16\":43514}}}}",
  };
  size_t index;

  for (index = 0u; index < sizeof(escaped) / sizeof(escaped[0]); ++index) {
    make_package(&zip, escaped[index], "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
    CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
  }
}

static void test_rejects_manifest_nested_under_unrelated_key(void)
{
  struct zip_builder zip;
  make_package(&zip,
               "{\"x\":{\"manifest\":{\"application\":{" APP_NAMES "\"init_packet_data\":"
               "{\"device_type\":82,\"device_revision\":65535,\"firmware_crc16\":43514}}}}}",
               "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);

  /* fields split across two init_packet_data objects must not combine */
  make_package(&zip,
               "{\"manifest\":{\"application\":{" APP_NAMES "\"init_packet_data\":{\"device_type\":82}},"
               "\"init_packet_data\":{\"device_revision\":65535,\"firmware_crc16\":43514}}}",
               "firmware.dat", "init", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static void test_accepts_comment_containing_eocd_signature(void)
{
  struct zip_builder zip;
  size_t index;
  /* Fake EOCD signature with fewer than 22 bytes left after it: the old scan
   * gave up (read past EOF) instead of continuing to the real record. */
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 12u);
  for (index = 0u; index < 4u; ++index) {
    zip.bytes[zip.eocd_offset + 22u + index] = (uint8_t)(0x06054b50u >> (8u * index));
  }
  CHECK(validate(&zip) == GDFU_FILE_OK);

  /* Same trick with more room: must also still find the real record. */
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "firmware image", 24u);
  for (index = 0u; index < 4u; ++index) {
    zip.bytes[zip.eocd_offset + 22u + index] = (uint8_t)(0x06054b50u >> (8u * index));
  }
  CHECK(validate(&zip) == GDFU_FILE_OK);
}

static void test_rejects_empty_required_data(void)
{
  struct zip_builder zip;
  make_package(&zip, manifest_ok, "firmware.dat", "", "firmware.bin", "firmware image", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
  make_package(&zip, manifest_ok, "firmware.dat", "init", "firmware.bin", "", 0u);
  CHECK(validate(&zip) == GDFU_FILE_PACKAGE);
}

static uint32_t crc_of(const uint8_t *b, size_t n) {
  return gdfu_file_crc32_update(0xFFFFFFFFu, b, n) ^ 0xFFFFFFFFu;
}

static void test_upload_and_name_rules(void)
{
  /* names */
  assert(gdfu_file_name_valid("fw-1.2_a.zip", 12));
  assert(gdfu_file_name_valid("FW.ZIP", 6));
  assert(!gdfu_file_name_valid(".zip", 4));
  assert(!gdfu_file_name_valid(".hidden.zip", 11));
  assert(!gdfu_file_name_valid("fw.bin", 6));
  assert(!gdfu_file_name_valid("a/b.zip", 7));
  assert(!gdfu_file_name_valid("a b.zip", 7));
  assert(!gdfu_file_name_valid("", 0));
  assert(!gdfu_file_name_valid(NULL, 4));
  {
    char longname[80];
    memset(longname, 'a', sizeof(longname));
    memcpy(longname + 59, ".zip", 4);
    assert(gdfu_file_name_valid(longname, 63));
    assert(!gdfu_file_name_valid(longname, 64));
  }

  /* capacity: block rounding + 3 reserve blocks */
  assert(!gdfu_file_fits(1000000, 0, 0));
  assert(gdfu_file_fits(7 * 4096, 0, 4096));      /* 1 + 3 reserve = 4 <= 7 */
  assert(gdfu_file_fits(4 * 4096, 0, 4096));      /* exactly fits */
  assert(!gdfu_file_fits(4 * 4096, 0, 4097));     /* rounds up to 2 blocks */
  assert(!gdfu_file_fits(4 * 4096, 4096, 4096));  /* used counted */
  assert(!gdfu_file_fits(100, 200, 1));           /* used > total */
  assert(!gdfu_file_fits(0xFFFFFFFFu, 0, 0xFFFFFFFFu));

  /* crc32 matches the RF frame CRC */
  {
    const uint8_t v[] = "123456789";
    assert(crc_of(v, 9) == 0xCBF43926u);
    assert(crc_of(v, 9) == gdfu_crc32(v, 9));
  }

  /* upload happy path across two chunks */
  {
    uint8_t data[300];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 7);
    gdfu_upload_t u;
    gdfu_upload_init(&u);
    assert(gdfu_upload_chunk(&u, 0, data, 1) == GDFU_FILE_ERR_NO_UPLOAD);
    assert(gdfu_upload_finish(&u) == GDFU_FILE_ERR_NO_UPLOAD);
    assert(gdfu_upload_begin(&u, 0, 0) == GDFU_FILE_ERR_COMMAND);
    assert(gdfu_upload_begin(&u, sizeof(data), crc_of(data, sizeof(data))) == GDFU_FILE_OK);
    assert(gdfu_upload_begin(&u, 1, 0) == GDFU_FILE_ERR_BUSY);
    assert(gdfu_upload_finish(&u) == GDFU_FILE_ERR_INCOMPLETE);
    assert(gdfu_upload_chunk(&u, 0, data, 160) == GDFU_FILE_OK);
    assert(gdfu_upload_chunk(&u, 0, data, 160) == GDFU_FILE_ERR_OFFSET);   /* replay */
    assert(gdfu_upload_chunk(&u, 200, data, 10) == GDFU_FILE_ERR_OFFSET);  /* gap */
    assert(gdfu_upload_chunk(&u, 160, data + 160, 160) == GDFU_FILE_ERR_OFFSET); /* overrun */
    assert(gdfu_upload_chunk(&u, 160, data + 160, 140) == GDFU_FILE_OK);
    assert(u.received == 300u);
    assert(gdfu_upload_finish(&u) == GDFU_FILE_OK);
  }

  /* CRC mismatch is detected */
  {
    uint8_t data[16] = {1, 2, 3};
    gdfu_upload_t u;
    gdfu_upload_init(&u);
    assert(gdfu_upload_begin(&u, sizeof(data), crc_of(data, sizeof(data)) ^ 1u) == GDFU_FILE_OK);
    assert(gdfu_upload_chunk(&u, 0, data, sizeof(data)) == GDFU_FILE_OK);
    assert(gdfu_upload_finish(&u) == GDFU_FILE_ERR_CRC);
  }

  /* zero-length and oversized chunks rejected */
  {
    uint8_t big[GDFU_FILE_MAX_CHUNK + 1] = {0};
    gdfu_upload_t u;
    gdfu_upload_init(&u);
    assert(gdfu_upload_begin(&u, 100000, 0) == GDFU_FILE_OK);
    assert(gdfu_upload_chunk(&u, 0, big, 0) == GDFU_FILE_ERR_COMMAND);
    assert(gdfu_upload_chunk(&u, 0, big, sizeof(big)) == GDFU_FILE_ERR_COMMAND);
  }
}

/* ---- transactional replacement -------------------------------------------- */

/* Fake filesystem: which of the three paths exist, plus a script of failures. */
struct fake_fs {
  bool has[3];          /* indexed by GDFU_PATH_* */
  int fail_on_call;     /* 1-based rename call number that fails, 0 = none */
  int fail_also_on;     /* second rename call that fails, 0 = none */
  int rename_calls;
  int remove_calls;
  int log[8];           /* from*10+to for each rename */
};

static bool fake_rename(void *ctx, int from, int to)
{
  struct fake_fs *fs = ctx;
  fs->rename_calls++;
  fs->log[fs->rename_calls - 1] = from * 10 + to;
  if (fs->rename_calls == fs->fail_on_call || fs->rename_calls == fs->fail_also_on) {
    return false;
  }
  if (!fs->has[from] || fs->has[to]) {
    return false; /* VFS rename refuses a missing source or an existing target */
  }
  fs->has[from] = false;
  fs->has[to] = true;
  return true;
}

static bool fake_exists(void *ctx, int path)
{
  const struct fake_fs *fs = ctx;
  return fs->has[path];
}

static bool fake_remove(void *ctx, int path)
{
  struct fake_fs *fs = ctx;
  fs->remove_calls++;
  fs->has[path] = false;
  return true;
}

static void test_promote_without_existing_destination(void)
{
  struct fake_fs fs = {.has = {[GDFU_PATH_TEMP] = true}};
  const gdfu_fs_ops_t ops = {fake_rename, fake_remove, fake_exists};

  CHECK(gdfu_file_promote(&ops, &fs, false) == GDFU_PROMOTE_DONE);
  CHECK(fs.has[GDFU_PATH_FINAL] && !fs.has[GDFU_PATH_TEMP] && !fs.has[GDFU_PATH_BACKUP]);
  CHECK(fs.rename_calls == 1 && fs.remove_calls == 0);
}

static void test_promote_replaces_through_backup(void)
{
  struct fake_fs fs = {.has = {[GDFU_PATH_TEMP] = true, [GDFU_PATH_FINAL] = true}};
  const gdfu_fs_ops_t ops = {fake_rename, fake_remove, fake_exists};

  CHECK(gdfu_file_promote(&ops, &fs, true) == GDFU_PROMOTE_DONE);
  CHECK(fs.has[GDFU_PATH_FINAL] && !fs.has[GDFU_PATH_TEMP] && !fs.has[GDFU_PATH_BACKUP]);
  /* order: final->backup, temp->final, then the backup is removed */
  CHECK(fs.log[0] == GDFU_PATH_FINAL * 10 + GDFU_PATH_BACKUP);
  CHECK(fs.log[1] == GDFU_PATH_TEMP * 10 + GDFU_PATH_FINAL);
  CHECK(fs.rename_calls == 2 && fs.remove_calls == 1);
}

static void test_promote_restores_original_when_second_rename_fails(void)
{
  struct fake_fs fs = {.has = {[GDFU_PATH_TEMP] = true, [GDFU_PATH_FINAL] = true},
                       .fail_on_call = 2};
  const gdfu_fs_ops_t ops = {fake_rename, fake_remove, fake_exists};

  CHECK(gdfu_file_promote(&ops, &fs, true) == GDFU_PROMOTE_RESTORED);
  CHECK(fs.has[GDFU_PATH_FINAL]);    /* the original survives */
  CHECK(!fs.has[GDFU_PATH_BACKUP]);
  CHECK(fs.remove_calls == 0);       /* never delete while a copy is at risk */
}

static void test_promote_keeps_backup_when_restore_also_fails(void)
{
  struct fake_fs fs = {.has = {[GDFU_PATH_TEMP] = true, [GDFU_PATH_FINAL] = true},
                       .fail_on_call = 2, .fail_also_on = 3};
  const gdfu_fs_ops_t ops = {fake_rename, fake_remove, fake_exists};

  CHECK(gdfu_file_promote(&ops, &fs, true) == GDFU_PROMOTE_STUCK);
  CHECK(fs.has[GDFU_PATH_BACKUP]);   /* the only surviving copy is kept */
  CHECK(fs.remove_calls == 0);
}

static void test_promote_stops_when_backup_rename_fails(void)
{
  struct fake_fs fs = {.has = {[GDFU_PATH_TEMP] = true, [GDFU_PATH_FINAL] = true},
                       .fail_on_call = 1};
  const gdfu_fs_ops_t ops = {fake_rename, fake_remove, fake_exists};

  CHECK(gdfu_file_promote(&ops, &fs, true) == GDFU_PROMOTE_RESTORED);
  CHECK(fs.has[GDFU_PATH_FINAL] && fs.has[GDFU_PATH_TEMP]);
  CHECK(fs.rename_calls == 1 && fs.remove_calls == 0);
}

static void test_promote_refuses_while_a_stuck_backup_exists(void)
{
  /* A previous replacement got STUCK: the backup is the only copy of some
   * other file. A new promotion must not touch the backup (littlefs rename
   * would overwrite it) and must not remove anything. */
  struct fake_fs fs = {.has = {[GDFU_PATH_TEMP] = true, [GDFU_PATH_BACKUP] = true}};
  const gdfu_fs_ops_t ops = {fake_rename, fake_remove, fake_exists};

  CHECK(gdfu_file_promote(&ops, &fs, false) == GDFU_PROMOTE_STUCK);
  CHECK(fs.has[GDFU_PATH_BACKUP] && fs.has[GDFU_PATH_TEMP] && !fs.has[GDFU_PATH_FINAL]);
  CHECK(fs.rename_calls == 0 && fs.remove_calls == 0);

  struct fake_fs fs2 = {.has = {[GDFU_PATH_TEMP] = true, [GDFU_PATH_BACKUP] = true,
                                [GDFU_PATH_FINAL] = true}};
  CHECK(gdfu_file_promote(&ops, &fs2, true) == GDFU_PROMOTE_STUCK);
  CHECK(fs2.has[GDFU_PATH_BACKUP] && fs2.has[GDFU_PATH_FINAL] && fs2.has[GDFU_PATH_TEMP]);
  CHECK(fs2.rename_calls == 0 && fs2.remove_calls == 0);
}

static void test_recover_after_interrupted_replacement(void)
{
  const gdfu_fs_ops_t ops = {fake_rename, fake_remove, fake_exists};

  /* power lost after final->backup, before temp->final: restore the backup */
  struct fake_fs lost = {.has = {[GDFU_PATH_BACKUP] = true}};
  CHECK(gdfu_file_recover(&ops, &lost, true, false));
  CHECK(lost.has[GDFU_PATH_FINAL] && !lost.has[GDFU_PATH_BACKUP]);

  /* power lost after temp->final, before the backup was removed: drop backup */
  struct fake_fs done = {.has = {[GDFU_PATH_BACKUP] = true, [GDFU_PATH_FINAL] = true}};
  CHECK(gdfu_file_recover(&ops, &done, true, true));
  CHECK(done.has[GDFU_PATH_FINAL] && !done.has[GDFU_PATH_BACKUP]);

  /* nothing pending: no filesystem calls */
  struct fake_fs idle = {.has = {[GDFU_PATH_FINAL] = true}};
  CHECK(gdfu_file_recover(&ops, &idle, false, true));
  CHECK(idle.rename_calls == 0 && idle.remove_calls == 0);
}

int main(void)
{
  test_promote_without_existing_destination();
  test_promote_replaces_through_backup();
  test_promote_restores_original_when_second_rename_fails();
  test_promote_keeps_backup_when_restore_also_fails();
  test_promote_stops_when_backup_rename_fails();
  test_promote_refuses_while_a_stuck_backup_exists();
  test_recover_after_interrupted_replacement();
  test_upload_and_name_rules();
  test_package_policy();
  test_nordic_crc16();
  test_accepts_exact_stored_package_with_bounded_comment();
  test_manifest_names_entries();
  test_accepts_nrfutil_manifest_with_ignored_fields();
  test_rejects_bad_profile_and_manifest_fields();
  test_rejects_invalid_zip_structure_and_offsets();
  test_rejects_hostile_sizes_names_and_search_window();
  test_rejects_empty_bin_even_when_manifest_crc_matches_empty();
  test_rejects_data_overlapping_central_directory();
  test_rejects_stray_bytes_after_last_directory_record();
  test_rejects_nonexact_entries_and_methods();
  test_rejects_empty_required_data();
  test_rejects_escaped_or_overlong_manifest_keys();
  test_rejects_manifest_nested_under_unrelated_key();
  test_accepts_comment_containing_eocd_signature();
  /* Validator must range-check itself; the reader should never see a bad range. */
  CHECK(out_of_range_reads == 0);
  if (failures != 0) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  puts("all GDFU file package tests passed");
  return 0;
}
