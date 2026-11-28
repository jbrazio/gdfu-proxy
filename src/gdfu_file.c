#include "gdfu_file.h"

#include <string.h>

bool gdfu_file_name_valid(const char *name, size_t len) {
  static const char ext[] = ".zip";
  if (name == NULL || len < sizeof(ext) || len > GDFU_FILE_MAX_NAME || name[0] == '.') {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  for (size_t i = 0; i < sizeof(ext) - 1; ++i) {
    char c = name[len - (sizeof(ext) - 1) + i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != ext[i]) return false;
  }
  return true;
}

bool gdfu_file_fits(uint32_t total, uint32_t used, uint32_t size) {
  uint64_t need = ((uint64_t)size + GDFU_FILE_BLOCK - 1u) / GDFU_FILE_BLOCK * GDFU_FILE_BLOCK +
                  (uint64_t)GDFU_FILE_RESERVE_BLOCKS * GDFU_FILE_BLOCK;
  return size > 0u && used <= total && need <= (uint64_t)(total - used);
}

uint32_t gdfu_file_crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= buf[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

void gdfu_upload_init(gdfu_upload_t *u) {
  u->active = false;
  u->size = 0;
  u->expected_crc = 0;
  u->received = 0;
  u->running_crc = 0xFFFFFFFFu;
}

gdfu_file_err_t gdfu_upload_begin(gdfu_upload_t *u, uint32_t size, uint32_t crc32) {
  if (u->active) return GDFU_FILE_ERR_BUSY;
  if (size == 0u) return GDFU_FILE_ERR_COMMAND;
  gdfu_upload_init(u);
  u->active = true;
  u->size = size;
  u->expected_crc = crc32;
  return GDFU_FILE_OK;
}

gdfu_file_err_t gdfu_upload_chunk(gdfu_upload_t *u, uint32_t offset, const uint8_t *data,
                                  size_t len) {
  if (!u->active) return GDFU_FILE_ERR_NO_UPLOAD;
  if (len == 0u || len > GDFU_FILE_MAX_CHUNK) return GDFU_FILE_ERR_COMMAND;
  if (offset != u->received) return GDFU_FILE_ERR_OFFSET;
  if (len > u->size - u->received) return GDFU_FILE_ERR_OFFSET;
  u->running_crc = gdfu_file_crc32_update(u->running_crc, data, len);
  u->received += (uint32_t)len;
  return GDFU_FILE_OK;
}

gdfu_file_err_t gdfu_upload_finish(gdfu_upload_t *u) {
  if (!u->active) return GDFU_FILE_ERR_NO_UPLOAD;
  if (u->received != u->size) return GDFU_FILE_ERR_INCOMPLETE;
  if ((u->running_crc ^ 0xFFFFFFFFu) != u->expected_crc) return GDFU_FILE_ERR_CRC;
  return GDFU_FILE_OK;
}

#define GDFU_ZIP_EOCD_SIZE 22u
#define GDFU_ZIP_MAX_COMMENT 65535u
#define GDFU_ZIP_EOCD_SEARCH (GDFU_ZIP_EOCD_SIZE + GDFU_ZIP_MAX_COMMENT)
#define GDFU_ZIP_SCAN_CHUNK 128u
#define GDFU_ZIP_RECORD_CHUNK 64u
#define GDFU_ZIP_NAME_SIZE 64u
#define GDFU_JSON_KEY_SIZE 32u
#define GDFU_JSON_MAX_DEPTH 16u

#define GDFU_ZIP_LOCAL_SIGNATURE 0x04034b50u
#define GDFU_ZIP_CENTRAL_SIGNATURE 0x02014b50u
#define GDFU_ZIP_EOCD_SIGNATURE 0x06054b50u

struct gdfu_zip_entry {
    uint32_t local_offset;
    uint32_t data_offset;
    uint32_t size;
    uint32_t data_end;
    uint8_t name[GDFU_ZIP_NAME_SIZE];
    uint16_t name_length;
};

struct gdfu_json_stream {
    gdfu_file_reader_t reader;
    void *context;
    uint32_t offset;
    uint32_t size;
    uint32_t position;
    uint8_t buffer[GDFU_ZIP_RECORD_CHUNK];
    size_t buffer_length;
    uint32_t buffer_offset;
    bool failed;
    uint8_t values_found;
    char dat_file[GDFU_ZIP_NAME_SIZE];
    char bin_file[GDFU_ZIP_NAME_SIZE];
    uint32_t device_type;
    uint32_t device_revision;
    uint32_t firmware_crc16;
};

static uint16_t gdfu_crc16_update(uint16_t crc, const uint8_t *data, size_t length)
{
    size_t index;

    for (index = 0u; index < length; ++index) {
        crc = (uint16_t)(((crc >> 8u) & 0xffu) | ((crc << 8u) & 0xffffu));
        crc ^= data[index];
        crc ^= (uint16_t)((crc & 0xffu) >> 4u);
        crc ^= (uint16_t)(((uint32_t)crc << 8u << 4u) & 0xffffu);
        crc ^= (uint16_t)((((uint32_t)(crc & 0xffu) << 4u) << 1u) & 0xffffu);
    }
    return crc;
}

uint16_t gdfu_crc16_nordic(const uint8_t *data, size_t length)
{
    return gdfu_crc16_update(0xffffu, data, length);
}

static uint16_t gdfu_get16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static uint32_t gdfu_get32(const uint8_t *data)
{
    return (uint32_t)gdfu_get16(data) | ((uint32_t)gdfu_get16(data + 2u) << 16u);
}

static bool gdfu_range(uint32_t file_size, uint32_t offset, uint32_t length)
{
    return offset <= file_size && length <= file_size - offset;
}

static bool gdfu_read(gdfu_file_reader_t reader, void *context, uint32_t file_size,
                      uint32_t offset, uint8_t *buffer, size_t length)
{
    return length <= UINT32_MAX && gdfu_range(file_size, offset, (uint32_t)length) &&
           reader(context, offset, buffer, length);
}

static bool gdfu_zip_find_eocd(gdfu_file_reader_t reader, void *context, uint32_t file_size,
                               uint32_t *eocd_offset, uint8_t eocd[GDFU_ZIP_EOCD_SIZE])
{
    uint8_t buffer[GDFU_ZIP_SCAN_CHUNK];
    uint32_t lower = file_size > GDFU_ZIP_EOCD_SEARCH ? file_size - GDFU_ZIP_EOCD_SEARCH : 0u;
    uint32_t cursor = file_size;

    while (cursor > lower) {
        uint32_t start = cursor - lower > GDFU_ZIP_SCAN_CHUNK ? cursor - GDFU_ZIP_SCAN_CHUNK : lower;
        size_t length = (size_t)(cursor - start);
        size_t index;

        if (!gdfu_read(reader, context, file_size, start, buffer, length)) {
            return false;
        }
        if (length >= 4u) {
            index = length - 4u;
            for (;;) {
                uint32_t candidate;
                if (gdfu_get32(buffer + index) == GDFU_ZIP_EOCD_SIGNATURE) {
                    candidate = start + (uint32_t)index;
                    /* A signature inside a ZIP comment is not the EOCD; skip
                     * any candidate whose record would run past EOF and keep
                     * scanning toward older offsets. */
                    if (candidate > file_size - GDFU_ZIP_EOCD_SIZE) {
                        if (index == 0u) {
                            break;
                        }
                        --index;
                        continue;
                    }
                    if (!gdfu_read(reader, context, file_size, candidate, eocd,
                                   GDFU_ZIP_EOCD_SIZE)) {
                        return false;
                    }
                    if (gdfu_get16(eocd + 20u) ==
                        file_size - candidate - GDFU_ZIP_EOCD_SIZE) {
                        *eocd_offset = candidate;
                        return true;
                    }
                }
                if (index == 0u) {
                    break;
                }
                --index;
            }
        }
        if (start == lower) {
            break;
        }
        cursor = start + 3u;
    }
    return false;
}

static bool gdfu_name_equals(const uint8_t *name, uint16_t length, const char *expected)
{
    size_t expected_length = strlen(expected);

    return length == expected_length && memcmp(name, expected, expected_length) == 0;
}

static bool gdfu_zip_read_local(gdfu_file_reader_t reader, void *context, uint32_t file_size,
                                uint32_t directory_offset, struct gdfu_zip_entry *entry,
                                uint16_t method, uint16_t flags, uint32_t compressed_size,
                                uint32_t uncompressed_size)
{
    uint8_t local[30];
    uint8_t local_name[GDFU_ZIP_NAME_SIZE];
    uint32_t header_end;
    uint32_t data_offset;
    uint32_t data_end;
    uint16_t name_length;
    uint16_t extra_length;

    if (!gdfu_read(reader, context, file_size, entry->local_offset, local, sizeof(local)) ||
        gdfu_get32(local) != GDFU_ZIP_LOCAL_SIGNATURE || gdfu_get16(local + 6u) != flags ||
        gdfu_get16(local + 8u) != method || gdfu_get32(local + 18u) != compressed_size ||
        gdfu_get32(local + 22u) != uncompressed_size) {
        return false;
    }
    name_length = gdfu_get16(local + 26u);
    extra_length = gdfu_get16(local + 28u);
    if (name_length != entry->name_length || name_length >= sizeof(local_name) ||
        !gdfu_range(file_size, entry->local_offset + 30u, name_length) ||
        !gdfu_read(reader, context, file_size, entry->local_offset + 30u, local_name,
                   name_length) ||
        memcmp(local_name, entry->name, name_length) != 0 ||
        !gdfu_range(file_size, entry->local_offset + 30u + name_length, extra_length)) {
        return false;
    }
    header_end = entry->local_offset + 30u + name_length + extra_length;
    if (header_end < entry->local_offset || !gdfu_range(file_size, header_end, compressed_size)) {
        return false;
    }
    data_offset = header_end;
    data_end = data_offset + compressed_size;
    if (data_end < data_offset || data_end > directory_offset) {
        return false;
    }
    entry->data_offset = data_offset;
    entry->size = compressed_size;
    entry->data_end = data_end;
    return true;
}

static bool gdfu_json_get(struct gdfu_json_stream *stream, uint8_t *value)
{
    if (stream->position == stream->size) {
        return false;
    }
    if (stream->buffer_length == 0u || stream->position < stream->buffer_offset ||
        stream->position - stream->buffer_offset >= stream->buffer_length) {
        size_t length = stream->size - stream->position;
        if (length > sizeof(stream->buffer)) {
            length = sizeof(stream->buffer);
        }
        if (!stream->reader(stream->context, stream->offset + stream->position, stream->buffer,
                            length)) {
            stream->failed = true;
            return false;
        }
        stream->buffer_offset = stream->position;
        stream->buffer_length = length;
    }
    *value = stream->buffer[stream->position - stream->buffer_offset];
    ++stream->position;
    return true;
}

static bool gdfu_json_peek(struct gdfu_json_stream *stream, uint8_t *value)
{
    uint32_t position = stream->position;

    if (!gdfu_json_get(stream, value)) {
        return false;
    }
    stream->position = position;
    return true;
}

static bool gdfu_json_skip_space(struct gdfu_json_stream *stream, uint8_t *value)
{
    do {
        if (!gdfu_json_get(stream, value)) {
            return false;
        }
    } while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n');
    return true;
}

static bool gdfu_json_at_end(struct gdfu_json_stream *stream)
{
    uint8_t value;

    while (gdfu_json_get(stream, &value)) {
        if (!(value == ' ' || value == '\t' || value == '\r' || value == '\n')) {
            return false;
        }
    }
    return !stream->failed;
}

static bool gdfu_json_hex(uint8_t value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static bool gdfu_json_string(struct gdfu_json_stream *stream, char *text, size_t text_size)
{
    uint8_t value;
    size_t length = 0u;

    if (!gdfu_json_skip_space(stream, &value) || value != '"') {
        return false;
    }
    while (gdfu_json_get(stream, &value)) {
        if (value == '"') {
            if (text != NULL) {
                /* Keys feed strcmp; always terminate, and never hand back a
                 * truncated or escape-containing string that could alias a
                 * profile field name. */
                text[length < text_size ? length : text_size - 1u] = '\0';
                if (length >= text_size) {
                    return false;
                }
            }
            return true;
        }
        if (value < 0x20u) {
            return false;
        }
        if (value == '\\') {
            if (!gdfu_json_get(stream, &value) ||
                !(value == '"' || value == '\\' || value == '/' || value == 'b' ||
                  value == 'f' || value == 'n' || value == 'r' || value == 't' || value == 'u')) {
                return false;
            }
            if (value == 'u') {
                size_t index;
                for (index = 0u; index < 4u; ++index) {
                    if (!gdfu_json_get(stream, &value) || !gdfu_json_hex(value)) {
                        return false;
                    }
                }
            }
            if (text != NULL) {
                return false; /* escaped keys are not profile names; reject outright */
            }
        } else if (text != NULL && length + 1u < text_size) {
            text[length++] = (char)value;
        } else if (text != NULL) {
            length = text_size;
        }
    }
    return false;
}

static bool gdfu_json_number(struct gdfu_json_stream *stream, uint32_t *number)
{
    uint8_t value;
    uint32_t result = 0u;
    bool digit = false;

    if (!gdfu_json_skip_space(stream, &value)) {
        return false;
    }
    if (value == '-') {
        return false;
    }
    while (value >= '0' && value <= '9') {
        uint32_t digit_value = (uint32_t)(value - '0');
        if (result > (UINT32_MAX - digit_value) / 10u) {
            return false;
        }
        result = result * 10u + digit_value;
        digit = true;
        if (!gdfu_json_peek(stream, &value)) {
            break;
        }
        if (value < '0' || value > '9') {
            break;
        }
        (void)gdfu_json_get(stream, &value);
    }
    if (!digit) {
        return false;
    }
    *number = result;
    return true;
}

/* ponytail: ignored numbers are only token-checked (digits . e E + -), not full JSON grammar.
 * Profile fields use gdfu_json_number, which is strict. */
static bool gdfu_json_skip_number(struct gdfu_json_stream *stream)
{
    uint8_t value;
    bool digit = false;

    while (gdfu_json_peek(stream, &value) &&
           ((value >= '0' && value <= '9') || value == '-' || value == '+' || value == '.' ||
            value == 'e' || value == 'E')) {
        digit = digit || (value >= '0' && value <= '9');
        (void)gdfu_json_get(stream, &value);
    }
    return digit && !stream->failed;
}

static bool gdfu_json_literal(struct gdfu_json_stream *stream, uint8_t first)
{
    const char *tail;
    uint8_t value;

    if (first == 't') {
        tail = "rue";
    } else if (first == 'f') {
        tail = "alse";
    } else if (first == 'n') {
        tail = "ull";
    } else {
        return false;
    }
    while (*tail != '\0') {
        if (!gdfu_json_get(stream, &value) || value != (uint8_t)*tail++) {
            return false;
        }
    }
    return true;
}

static bool gdfu_json_value(struct gdfu_json_stream *stream, unsigned depth, unsigned path);

#define GDFU_JSON_PATH_UNKNOWN 0xFFFFFFFFu

static bool gdfu_json_array(struct gdfu_json_stream *stream, unsigned depth)
{
    uint8_t value;

    if (!gdfu_json_skip_space(stream, &value) || value != '[') {
        return false;
    }
    if (!gdfu_json_skip_space(stream, &value)) {
        return false;
    }
    if (value == ']') {
        return true;
    }
    --stream->position;
    for (;;) {
        if (!gdfu_json_value(stream, depth + 1u, GDFU_JSON_PATH_UNKNOWN) ||
            !gdfu_json_skip_space(stream, &value)) {
            return false;
        }
        if (value == ']') {
            return true;
        }
        if (value != ',') {
            return false;
        }
    }
}

static unsigned gdfu_json_child_path(unsigned path, const char *key)
{
    /* GDFU_JSON_PATH_UNKNOWN must not equal a real path: a "manifest" key
     * inside an unrelated subtree (or array) must not open the profile. */
    if (path == 0u && strcmp(key, "manifest") == 0) {
        return 1u;
    }
    if (path == 1u && strcmp(key, "application") == 0) {
        return 2u;
    }
    if (path == 2u && strcmp(key, "init_packet_data") == 0) {
        return 3u;
    }
    return GDFU_JSON_PATH_UNKNOWN;
}

/* Returns 0 when key is not a profile field, 1 when consumed, -1 when invalid or duplicate. */
static int gdfu_json_record_number(struct gdfu_json_stream *stream, const char *key)
{
    uint32_t value;
    uint8_t bit;

    if (strcmp(key, "device_type") == 0) {
        bit = 1u;
    } else if (strcmp(key, "device_revision") == 0) {
        bit = 2u;
    } else if (strcmp(key, "firmware_crc16") == 0) {
        bit = 4u;
    } else {
        return 0;
    }
    if ((stream->values_found & bit) != 0u || !gdfu_json_number(stream, &value)) {
        return -1;
    }
    stream->values_found |= bit;
    if (bit == 1u) {
        stream->device_type = value;
    } else if (bit == 2u) {
        stream->device_revision = value;
    } else {
        stream->firmware_crc16 = value;
    }
    return 1;
}

/* Same contract as gdfu_json_record_number, for the application file names. */
static int gdfu_json_record_name(struct gdfu_json_stream *stream, const char *key)
{
    uint8_t bit;
    char *name;

    if (strcmp(key, "dat_file") == 0) {
        bit = 8u;
        name = stream->dat_file;
    } else if (strcmp(key, "bin_file") == 0) {
        bit = 16u;
        name = stream->bin_file;
    } else {
        return 0;
    }
    if ((stream->values_found & bit) != 0u ||
        !gdfu_json_string(stream, name, GDFU_ZIP_NAME_SIZE)) {
        return -1;
    }
    stream->values_found |= bit;
    return 1;
}

static bool gdfu_json_object(struct gdfu_json_stream *stream, unsigned depth, unsigned path)
{
    uint8_t value;
    char key[GDFU_JSON_KEY_SIZE];

    if (!gdfu_json_skip_space(stream, &value) || value != '{') {
        return false;
    }
    if (!gdfu_json_skip_space(stream, &value)) {
        return false;
    }
    if (value == '}') {
        return true;
    }
    --stream->position;
    for (;;) {
        int recorded = 0;
        if (!gdfu_json_string(stream, key, sizeof(key)) || !gdfu_json_skip_space(stream, &value) ||
            value != ':' || !gdfu_json_skip_space(stream, &value)) {
            return false;
        }
        --stream->position;
        if (path == 3u) {
            recorded = gdfu_json_record_number(stream, key);
            if (recorded < 0) {
                return false;
            }
        } else if (path == 2u) {
            recorded = gdfu_json_record_name(stream, key);
            if (recorded < 0) {
                return false;
            }
        } else if (path == GDFU_JSON_PATH_UNKNOWN &&
                   (stream->values_found & 7u) != 0u) {
            /* Profile fields must come from one init_packet_data object, not
             * be assembled across several unrelated ones. */
            return false;
        }
        if (recorded == 0 &&
            !gdfu_json_value(stream, depth + 1u, gdfu_json_child_path(path, key))) {
            return false;
        }
        if (!gdfu_json_skip_space(stream, &value)) {
            return false;
        }
        if (value == '}') {
            return true;
        }
        if (value != ',') {
            return false;
        }
    }
}

static bool gdfu_json_value(struct gdfu_json_stream *stream, unsigned depth, unsigned path)
{
    uint8_t value;

    if (depth >= GDFU_JSON_MAX_DEPTH || !gdfu_json_skip_space(stream, &value)) {
        return false;
    }
    --stream->position;
    if (value == '{') {
        return gdfu_json_object(stream, depth, path);
    }
    if (value == '[') {
        return gdfu_json_array(stream, depth);
    }
    if (value == '"') {
        return gdfu_json_string(stream, NULL, 0u);
    }
    if ((value >= '0' && value <= '9') || value == '-') {
        return gdfu_json_skip_number(stream);
    }
    (void)gdfu_json_get(stream, &value);
    return gdfu_json_literal(stream, value);
}

/* On success returns the manifest's file names and firmware_crc16. */
static bool gdfu_validate_manifest(gdfu_file_reader_t reader, void *context,
                                   const struct gdfu_zip_entry *manifest, char *dat_file,
                                   char *bin_file, uint16_t *crc16)
{
    struct gdfu_json_stream stream;
    if (manifest->size == 0u) {
        return false;
    }
    memset(&stream, 0, sizeof(stream));
    stream.reader = reader;
    stream.context = context;
    stream.offset = manifest->data_offset;
    stream.size = manifest->size;
    if (!gdfu_json_value(&stream, 0u, 0u) || !gdfu_json_at_end(&stream) ||
        stream.values_found != 31u || stream.device_type != GDFU_FILE_DEVICE_TYPE ||
        stream.device_revision != GDFU_FILE_DEVICE_REVISION || stream.firmware_crc16 > UINT16_MAX) {
        return false;
    }
    memcpy(dat_file, stream.dat_file, GDFU_ZIP_NAME_SIZE);
    memcpy(bin_file, stream.bin_file, GDFU_ZIP_NAME_SIZE);
    *crc16 = (uint16_t)stream.firmware_crc16;
    return true;
}

static bool gdfu_crc16_entry(gdfu_file_reader_t reader, void *context, uint32_t file_size,
                             const struct gdfu_zip_entry *entry, uint16_t *crc16)
{
    uint8_t buffer[GDFU_ZIP_RECORD_CHUNK];
    uint32_t offset = entry->data_offset;
    uint32_t remaining = entry->size;
    uint16_t crc = 0xffffu;

    while (remaining != 0u) {
        size_t length = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        if (!gdfu_read(reader, context, file_size, offset, buffer, length)) {
            return false;
        }
        crc = gdfu_crc16_update(crc, buffer, length);
        offset += (uint32_t)length;
        remaining -= (uint32_t)length;
    }
    *crc16 = crc;
    return true;
}

gdfu_file_err_t gdfu_zip_validate_reader(gdfu_file_reader_t reader, void *context,
                                         uint32_t file_size)
{
    uint8_t eocd[GDFU_ZIP_EOCD_SIZE];
    uint8_t central[46];
    struct gdfu_zip_entry entries[3];
    char dat_file[GDFU_ZIP_NAME_SIZE];
    char bin_file[GDFU_ZIP_NAME_SIZE];
    uint32_t eocd_offset;
    uint32_t directory_offset;
    uint32_t directory_size;
    uint32_t directory_end;
    uint32_t cursor;
    uint16_t count;
    size_t index;
    size_t manifest_index = 3u;
    size_t dat_index = 3u;
    size_t bin_index = 3u;
    uint16_t crc16;
    uint16_t manifest_crc16;

    if (reader == NULL || !gdfu_zip_find_eocd(reader, context, file_size, &eocd_offset, eocd) ||
        gdfu_get16(eocd + 4u) != 0u || gdfu_get16(eocd + 6u) != 0u ||
        gdfu_get16(eocd + 8u) != 3u || gdfu_get16(eocd + 10u) != 3u) {
        return GDFU_FILE_PACKAGE;
    }
    directory_size = gdfu_get32(eocd + 12u);
    directory_offset = gdfu_get32(eocd + 16u);
    if (!gdfu_range(file_size, directory_offset, directory_size) ||
        directory_offset > eocd_offset || directory_size != eocd_offset - directory_offset) {
        return GDFU_FILE_PACKAGE;
    }
    directory_end = eocd_offset;
    cursor = directory_offset;
    count = gdfu_get16(eocd + 10u);
    memset(entries, 0, sizeof(entries));

    for (index = 0u; index < count; ++index) {
        uint16_t flags;
        uint16_t method;
        uint16_t name_length;
        uint16_t extra_length;
        uint16_t comment_length;
        uint32_t compressed_size;
        uint32_t uncompressed_size;
        uint32_t record_size;
        struct gdfu_zip_entry *entry = &entries[index];

        if (!gdfu_read(reader, context, file_size, cursor, central, sizeof(central)) ||
            gdfu_get32(central) != GDFU_ZIP_CENTRAL_SIGNATURE) {
            return GDFU_FILE_PACKAGE;
        }
        flags = gdfu_get16(central + 8u);
        method = gdfu_get16(central + 10u);
        compressed_size = gdfu_get32(central + 20u);
        uncompressed_size = gdfu_get32(central + 24u);
        name_length = gdfu_get16(central + 28u);
        extra_length = gdfu_get16(central + 30u);
        comment_length = gdfu_get16(central + 32u);
        if (name_length >= GDFU_ZIP_NAME_SIZE || flags != 0u || method != 0u ||
            compressed_size != uncompressed_size || gdfu_get16(central + 34u) != 0u ||
            !gdfu_range(directory_end, cursor, 46u) ||
            !gdfu_range(directory_end, cursor + 46u, name_length) ||
            !gdfu_range(directory_end, cursor + 46u + name_length, extra_length) ||
            !gdfu_range(directory_end, cursor + 46u + name_length + extra_length, comment_length)) {
            return GDFU_FILE_PACKAGE;
        }
        record_size = 46u + name_length + extra_length + comment_length;
        if (record_size < 46u || !gdfu_range(directory_end, cursor, record_size)) {
            return GDFU_FILE_PACKAGE;
        }
        if (name_length == 0u ||
            !gdfu_read(reader, context, file_size, cursor + 46u, entry->name, name_length)) {
            return GDFU_FILE_PACKAGE;
        }
        if (gdfu_name_equals(entry->name, name_length, "manifest.json")) {
            if (manifest_index != 3u) {
                return GDFU_FILE_PACKAGE;
            }
            manifest_index = index;
        }
        entry->local_offset = gdfu_get32(central + 42u);
        entry->name_length = name_length;
        if (!gdfu_zip_read_local(reader, context, file_size, directory_offset, entry, method, flags,
                                 compressed_size, uncompressed_size)) {
            return GDFU_FILE_PACKAGE;
        }
        cursor += record_size;
    }
    /* The manifest names the other two entries; they must match exactly, one each. */
    if (cursor != directory_end || manifest_index == 3u ||
        !gdfu_validate_manifest(reader, context, &entries[manifest_index], dat_file, bin_file,
                                &manifest_crc16) ||
        dat_file[0] == '\0' || bin_file[0] == '\0' || strcmp(dat_file, bin_file) == 0 ||
        strcmp(dat_file, "manifest.json") == 0 || strcmp(bin_file, "manifest.json") == 0) {
        return GDFU_FILE_PACKAGE;
    }
    for (index = 0u; index < 3u; ++index) {
        if (index == manifest_index) {
            continue;
        }
        if (gdfu_name_equals(entries[index].name, entries[index].name_length, dat_file) &&
            dat_index == 3u) {
            dat_index = index;
        } else if (gdfu_name_equals(entries[index].name, entries[index].name_length, bin_file) &&
                   bin_index == 3u) {
            bin_index = index;
        } else {
            return GDFU_FILE_PACKAGE;
        }
    }
    if (dat_index == 3u || bin_index == 3u || entries[dat_index].size == 0u ||
        entries[bin_index].size == 0u ||
        !gdfu_crc16_entry(reader, context, file_size, &entries[bin_index], &crc16) ||
        crc16 != manifest_crc16) {
        return GDFU_FILE_PACKAGE;
    }
    return GDFU_FILE_OK;
}

gdfu_promote_result_t gdfu_file_promote(const gdfu_fs_ops_t *fs, void *ctx,
                                        bool destination_exists)
{
    /* A leftover backup is the only surviving copy of some earlier file until
     * recovery has resolved it. Renaming over it (littlefs rename replaces an
     * existing destination) could destroy that copy, so refuse until then. */
    if (fs->exists != NULL && fs->exists(ctx, GDFU_PATH_BACKUP)) {
        return GDFU_PROMOTE_STUCK;
    }
    if (!destination_exists) {
        return fs->rename(ctx, GDFU_PATH_TEMP, GDFU_PATH_FINAL) ? GDFU_PROMOTE_DONE
                                                                 : GDFU_PROMOTE_RESTORED;
    }
    /* The old file moves aside first, so a failure at any later point still
     * leaves one complete copy reachable. */
    if (!fs->rename(ctx, GDFU_PATH_FINAL, GDFU_PATH_BACKUP)) {
        return GDFU_PROMOTE_RESTORED; /* nothing moved: the original is untouched */
    }
    if (fs->rename(ctx, GDFU_PATH_TEMP, GDFU_PATH_FINAL)) {
        (void)fs->remove(ctx, GDFU_PATH_BACKUP);
        return GDFU_PROMOTE_DONE;
    }
    if (fs->rename(ctx, GDFU_PATH_BACKUP, GDFU_PATH_FINAL)) {
        return GDFU_PROMOTE_RESTORED;
    }
    return GDFU_PROMOTE_STUCK; /* never remove: the backup is the only copy */
}

bool gdfu_file_recover(const gdfu_fs_ops_t *fs, void *ctx, bool backup_exists,
                       bool final_exists)
{
    if (!backup_exists) {
        return true;
    }
    if (final_exists) {
        return fs->remove(ctx, GDFU_PATH_BACKUP);
    }
    return fs->rename(ctx, GDFU_PATH_BACKUP, GDFU_PATH_FINAL);
}
