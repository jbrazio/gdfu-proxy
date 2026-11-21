#ifndef GDFU_FILE_H
#define GDFU_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure (no I/O) rules for the FILE MANAGEMENT GATT group; see docs/PROTOCOL_BLE.md.
 * main.cpp performs the filesystem work and calls these for every decision,
 * so the rules are testable on the host. */

#define GDFU_FILE_MAX_NAME 63u
#define GDFU_FILE_MAX_CHUNK 160u
/* LittleFS allocates whole 4 KiB blocks and needs spare blocks for metadata. */
#define GDFU_FILE_BLOCK 4096u
#define GDFU_FILE_RESERVE_BLOCKS 3u

/* Package profile shared by the proxy validator and mobile preflight. */
#define GDFU_FILE_DEVICE_TYPE 82u
#define GDFU_FILE_DEVICE_REVISION 65535u
#define GDFU_SUPPORTED_DEVICE_TYPE GDFU_FILE_DEVICE_TYPE
#define GDFU_SUPPORTED_DEVICE_REVISION GDFU_FILE_DEVICE_REVISION
#define GDFU_DEVICE_TYPE GDFU_FILE_DEVICE_TYPE
#define GDFU_DEVICE_REVISION GDFU_FILE_DEVICE_REVISION

#ifdef __cplusplus
extern "C" {
#endif

/* Values are the FILE_STATUS.last_error codes in docs/PROTOCOL_BLE.md. */
typedef enum {
  GDFU_FILE_OK = 0,
  GDFU_FILE_ERR_COMMAND = 1,
  GDFU_FILE_ERR_BUSY = 2,
  GDFU_FILE_ERR_NAME = 3,
  GDFU_FILE_ERR_NO_SPACE = 4,
  GDFU_FILE_ERR_EXISTS = 5,
  GDFU_FILE_ERR_NOT_FOUND = 6,
  GDFU_FILE_ERR_IO = 7,
  GDFU_FILE_ERR_OFFSET = 8,
  GDFU_FILE_ERR_CRC = 9,
  GDFU_FILE_ERR_INCOMPLETE = 10,
  GDFU_FILE_ERR_NO_UPLOAD = 11,
  GDFU_FILE_ERR_PACKAGE = 12
} gdfu_file_err_t;

/* Spellings used by the package validator and its tests. */
#define GDFU_FILE_ERR_OK GDFU_FILE_OK
#define GDFU_FILE_PACKAGE GDFU_FILE_ERR_PACKAGE

typedef struct {
  bool active;
  uint32_t size;
  uint32_t expected_crc;
  uint32_t received;
  uint32_t running_crc; /* raw (non-finalized) CRC register */
} gdfu_upload_t;

/* 1..63 chars of [A-Za-z0-9._-], not starting with '.', ending in ".zip". */
bool gdfu_file_name_valid(const char *name, size_t len);

/* True when a file of `size` bytes can be stored with `used` of `total` bytes
 * already allocated, including block rounding and the metadata reserve. */
bool gdfu_file_fits(uint32_t total, uint32_t used, uint32_t size);

/* IEEE CRC-32 (same polynomial as gdfu_crc32), incrementally. Start from
 * 0xFFFFFFFF; finalize by xor with 0xFFFFFFFF. */
uint32_t gdfu_file_crc32_update(uint32_t crc, const uint8_t *buf, size_t len);

void gdfu_upload_init(gdfu_upload_t *u);
gdfu_file_err_t gdfu_upload_begin(gdfu_upload_t *u, uint32_t size, uint32_t crc32);
/* Accepts only the next in-order chunk; updates received and the CRC. */
gdfu_file_err_t gdfu_upload_chunk(gdfu_upload_t *u, uint32_t offset, const uint8_t *data,
                                  size_t len);
/* Verifies length and CRC; the caller then renames the temporary file. */
gdfu_file_err_t gdfu_upload_finish(gdfu_upload_t *u);

/* ---- transactional replacement ----------------------------------------------
 * Replacing a stored file must never leave a window with no copy of it, and
 * must not need a third copy of the data: only renames and one final remove.
 * The three paths are identified by id so the logic is host-testable. */
enum { GDFU_PATH_TEMP = 0, GDFU_PATH_BACKUP = 1, GDFU_PATH_FINAL = 2 };

typedef struct {
  bool (*rename)(void *ctx, int from, int to);
  bool (*remove)(void *ctx, int path);
  /* Existence probe; NULL is allowed only when no backup can be pending. */
  bool (*exists)(void *ctx, int path);
} gdfu_fs_ops_t;

typedef enum {
  GDFU_PROMOTE_DONE = 0,     /* temp is now the final file */
  GDFU_PROMOTE_RESTORED = 1, /* promotion failed; the original is back in place */
  GDFU_PROMOTE_STUCK = 2     /* restore failed too; the original stays as the backup */
} gdfu_promote_result_t;

/* Move the validated temp file to the final name. When `destination_exists`,
 * the old file is renamed to the backup first and removed only after the new
 * file is in place. */
gdfu_promote_result_t gdfu_file_promote(const gdfu_fs_ops_t *fs, void *ctx,
                                        bool destination_exists);

/* Boot/reset recovery after an interrupted replacement. Restores the backup
 * when the final file is missing; drops a leftover backup when it is present.
 * Returns false only if a needed filesystem operation failed. */
bool gdfu_file_recover(const gdfu_fs_ops_t *fs, void *ctx, bool backup_exists,
                       bool final_exists);

/* Caller supplies bounded reads from the uploaded file. */
typedef bool (*gdfu_file_reader_t)(void *context, uint32_t offset, uint8_t *buffer,
                                   size_t length);

/* Nordic's CRC-16/CCITT-FALSE variant used for firmware.bin. */
uint16_t gdfu_crc16_nordic(const uint8_t *data, size_t length);

/* Validates the exact stored GFSK ZIP profile without extracting it. */
gdfu_file_err_t gdfu_zip_validate_reader(gdfu_file_reader_t reader, void *context,
                                         uint32_t file_size);

#ifdef __cplusplus
}
#endif

#endif
