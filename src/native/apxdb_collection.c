#include "apxdb_collection.h"
#include "apxdb_json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

typedef struct {
  char* id;
  char* json;
  uint8_t* blob;
  size_t blob_size;
} apxdb_collection_document_t;

typedef struct {
  char* key;
  char** doc_ids;
  size_t doc_count;
  size_t doc_capacity;
} apxdb_index_entry_t;

typedef struct {
  apxdb_index_definition_t definition;
  apxdb_index_entry_t* entries;
  size_t entry_count;
  size_t entry_capacity;
} apxdb_index_data_t;

struct apxdb_collection_t {
  const apxdb_schema_t* schema;
  apxdb_collection_document_t* documents;
  size_t document_count;
  size_t document_capacity;
  apxdb_index_definition_t* indexes;
  size_t index_count;
  size_t index_capacity;
  apxdb_index_data_t* index_data;
  size_t index_data_count;
  size_t index_data_capacity;
};

struct apxdb_query_t {
  const char* collection_name;
  const char* query_json;
};

struct apxdb_transaction_t {
  bool active;
};

static apxdb_collection_t** g_collections = NULL;
static size_t g_collection_count = 0;
static size_t g_collection_capacity = 0;
static atomic_int_fast32_t g_collection_counter = ATOMIC_VAR_INIT(0);
static apxdb_transaction_t* g_active_transaction = NULL;
static const size_t kGpuQueryMinDocs = 128;
static const uint32_t kCollectionFileMagic = 0x41505843u; // 'APXC'
static const uint32_t kCollectionFileVersion = 1;

static bool grow_collections(void) {
  if (g_collection_count >= g_collection_capacity) {
    size_t next_capacity = g_collection_capacity == 0 ? 8 : g_collection_capacity * 2;
    apxdb_collection_t** next = (apxdb_collection_t**)realloc(g_collections, next_capacity * sizeof(apxdb_collection_t*));
    if (!next) {
      return false;
    }
    g_collections = next;
    g_collection_capacity = next_capacity;
  }
  return true;
}

static apxdb_collection_t* find_collection(const char* name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = 0; i < g_collection_count; ++i) {
    if (strcmp(g_collections[i]->schema->collection_name, name) == 0) {
      return g_collections[i];
    }
  }
  return NULL;
}

static char* allocate_string_copy(const char* text) {
  if (!text) {
    return NULL;
  }
  size_t length = strlen(text);
  char* result = (char*)malloc(length + 1);
  if (!result) {
    return NULL;
  }
  memcpy(result, text, length + 1);
  return result;
}

typedef struct {
  uint32_t offset;
  uint32_t size;
  uint8_t is_null;
  uint8_t reserved[3];
} apxdb_document_field_entry_t;

static bool parse_numeric_value(const apxdb_json_value_t* value, double* out_value);
static bool compare_numeric_operator(double doc_value, const char* op, double threshold);
static bool serialize_document_blob(const apxdb_schema_t* schema, const apxdb_json_value_t* document, uint8_t** out_blob, size_t* out_blob_size);
static bool compute_document_blob_size(const apxdb_schema_t* schema, const apxdb_json_value_t* document, size_t* out_size);
static bool compute_field_payload_size(const apxdb_schema_field_t* field, const apxdb_json_value_t* value, size_t* out_size);
static bool serialize_field_payload(const apxdb_schema_field_t* field, const apxdb_json_value_t* value, uint8_t* buffer, size_t buffer_size, size_t* out_written);
extern bool run_gpu_query_int(const int32_t* values, const uint32_t* valid_mask, size_t count, int op, int32_t threshold, uint8_t* out_mask);
static bool index_document(apxdb_collection_t* collection, size_t doc_index, const apxdb_json_value_t* document);
static bool ensure_directory_exists(const char* path);
static bool format_collection_file_path(const char* directory_path, const char* collection_name, char* out_path, size_t out_path_size);
static bool save_collection_to_file(apxdb_collection_t* collection, const char* file_path);
static bool load_collection_from_file(apxdb_collection_t* collection, const char* file_path);
static bool write_uint32_to_file(FILE* file, uint32_t value);
static bool write_uint64_to_file(FILE* file, uint64_t value);
static bool read_uint32_from_file(FILE* file, uint32_t* out_value);
static bool read_uint64_from_file(FILE* file, uint64_t* out_value);
static bool write_bytes_to_file(FILE* file, const void* buffer, size_t size);
static bool read_bytes_from_file(FILE* file, void* buffer, size_t size);
static void update_document_counter_from_id(const char* id);
static bool parse_integer_value(const apxdb_json_value_t* value, int64_t* out_value) {
  if (!value || !out_value) {
    return false;
  }
  if (value->type == APXDB_JSON_NUMBER && value->u.number.is_integer) {
    *out_value = value->u.number.integer_value;
    return true;
  }
  if (value->type == APXDB_JSON_STRING) {
    char* endptr = NULL;
    errno = 0;
    long long parsed = strtoll(value->u.string_value, &endptr, 10);
    if (endptr == value->u.string_value || *endptr != '\0' || errno == ERANGE) {
      return false;
    }
    *out_value = (int64_t)parsed;
    return true;
  }
  return false;
}

static bool compute_document_blob_size(const apxdb_schema_t* schema, const apxdb_json_value_t* document, size_t* out_size) {
  if (!schema || !document || !out_size) {
    return false;
  }
  size_t total_payload = 0;
  for (size_t i = 0; i < schema->field_count; ++i) {
    const apxdb_schema_field_t* field = &schema->fields[i];
    const apxdb_json_value_t* value = apxdb_json_object_get(document, field->name);
    size_t field_size = 0;
    if (!value) {
      apxdb_json_value_t missing = {.type = APXDB_JSON_NULL};
      if (!compute_field_payload_size(field, &missing, &field_size)) {
        return false;
      }
    } else {
      if (!compute_field_payload_size(field, value, &field_size)) {
        return false;
      }
    }
    total_payload += field_size;
  }
  size_t header_size = sizeof(uint32_t) * 3 + schema->field_count * sizeof(apxdb_document_field_entry_t);
  *out_size = header_size + total_payload;
  return true;
}

static bool compute_field_payload_size(const apxdb_schema_field_t* field, const apxdb_json_value_t* value, size_t* out_size) {
  if (!field || !out_size) {
    return false;
  }
  *out_size = 0;
  if (!value || value->type == APXDB_JSON_NULL) {
    if (!field->nullable) {
      return false;
    }
    *out_size = 0;
    return true;
  }

  switch (field->type) {
    case APXDB_TYPE_BYTE: {
      int64_t integer;
      if (!parse_integer_value(value, &integer) || integer < 0 || integer > UINT8_MAX) {
        return false;
      }
      *out_size = 1;
      return true;
    }
    case APXDB_TYPE_SHORT: {
      int64_t integer;
      if (!parse_integer_value(value, &integer) || integer < INT32_MIN || integer > INT32_MAX) {
        return false;
      }
      *out_size = sizeof(int32_t);
      return true;
    }
    case APXDB_TYPE_INT: {
      int64_t integer;
      if (!parse_integer_value(value, &integer)) {
        return false;
      }
      *out_size = sizeof(int64_t);
      return true;
    }
    case APXDB_TYPE_FLOAT:
      if (value->type != APXDB_JSON_NUMBER) {
        return false;
      }
      *out_size = sizeof(float);
      return true;
    case APXDB_TYPE_DOUBLE:
      if (value->type != APXDB_JSON_NUMBER) {
        return false;
      }
      *out_size = sizeof(double);
      return true;
    case APXDB_TYPE_BOOL:
      if (value->type != APXDB_JSON_BOOL) {
        return false;
      }
      *out_size = 1;
      return true;
    case APXDB_TYPE_STRING:
      if (value->type != APXDB_JSON_STRING) {
        return false;
      }
      *out_size = sizeof(uint32_t) + strlen(value->u.string_value);
      return true;
    case APXDB_TYPE_DATETIME: {
      double conversion;
      if (!parse_numeric_value(value, &conversion)) {
        return false;
      }
      *out_size = sizeof(int64_t);
      return true;
    }
    case APXDB_TYPE_ENUM: {
      int64_t integer;
      if (!parse_integer_value(value, &integer)) {
        return false;
      }
      *out_size = sizeof(int64_t);
      return true;
    }
    case APXDB_TYPE_EMBEDDED: {
      if (value->type != APXDB_JSON_OBJECT || !field->embedded_schema) {
        return false;
      }
      size_t nested_size = 0;
      if (!compute_document_blob_size(field->embedded_schema, value, &nested_size)) {
        return false;
      }
      *out_size = sizeof(uint32_t) + nested_size;
      return true;
    }
    case APXDB_TYPE_LIST: {
      if (value->type != APXDB_JSON_ARRAY) {
        return false;
      }
      size_t count = value->u.array.count;
      size_t total = sizeof(uint32_t);
      apxdb_schema_field_t element_field = *field;
      element_field.type = field->list_element_type;
      element_field.embedded_schema = field->list_element_schema;
      for (size_t i = 0; i < count; ++i) {
        size_t element_size = 0;
        if (!compute_field_payload_size(&element_field, &value->u.array.values[i], &element_size)) {
          return false;
        }
        if (element_field.type == APXDB_TYPE_STRING || element_field.type == APXDB_TYPE_EMBEDDED) {
          total += sizeof(uint32_t) + element_size;
        } else {
          total += element_size;
        }
      }
      *out_size = total;
      return true;
    }
    default:
      return false;
  }
}

static bool serialize_field_payload(const apxdb_schema_field_t* field, const apxdb_json_value_t* value, uint8_t* buffer, size_t buffer_size, size_t* out_written) {
  if (!field || !buffer || !out_written) {
    return false;
  }
  *out_written = 0;
  if (!value || value->type == APXDB_JSON_NULL) {
    return field->nullable;
  }
  uint8_t* cursor = buffer;
  switch (field->type) {
    case APXDB_TYPE_BYTE: {
      int64_t integer;
      if (!parse_integer_value(value, &integer) || integer < 0 || integer > UINT8_MAX) {
        return false;
      }
      if (buffer_size < 1) {
        return false;
      }
      *cursor = (uint8_t)integer;
      *out_written = 1;
      return true;
    }
    case APXDB_TYPE_SHORT: {
      int64_t integer;
      if (!parse_integer_value(value, &integer) || integer < INT32_MIN || integer > INT32_MAX) {
        return false;
      }
      if (buffer_size < sizeof(int32_t)) {
        return false;
      }
      int32_t stored = (int32_t)integer;
      memcpy(cursor, &stored, sizeof(int32_t));
      *out_written = sizeof(int32_t);
      return true;
    }
    case APXDB_TYPE_INT: {
      int64_t integer;
      if (!parse_integer_value(value, &integer)) {
        return false;
      }
      if (buffer_size < sizeof(int64_t)) {
        return false;
      }
      memcpy(cursor, &integer, sizeof(int64_t));
      *out_written = sizeof(int64_t);
      return true;
    }
    case APXDB_TYPE_FLOAT:
      if (value->type != APXDB_JSON_NUMBER || buffer_size < sizeof(float)) {
        return false;
      }
      {
        float stored = (float)value->u.number.double_value;
        memcpy(cursor, &stored, sizeof(float));
        *out_written = sizeof(float);
      }
      return true;
    case APXDB_TYPE_DOUBLE:
      if (value->type != APXDB_JSON_NUMBER || buffer_size < sizeof(double)) {
        return false;
      }
      memcpy(cursor, &value->u.number.double_value, sizeof(double));
      *out_written = sizeof(double);
      return true;
    case APXDB_TYPE_BOOL:
      if (value->type != APXDB_JSON_BOOL || buffer_size < 1) {
        return false;
      }
      *cursor = value->u.bool_value ? 1 : 0;
      *out_written = 1;
      return true;
    case APXDB_TYPE_STRING:
      if (value->type != APXDB_JSON_STRING) {
        return false;
      }
      {
        size_t length = strlen(value->u.string_value);
        if (buffer_size < sizeof(uint32_t) + length) {
          return false;
        }
        uint32_t length_u32 = (uint32_t)length;
        memcpy(cursor, &length_u32, sizeof(uint32_t));
        cursor += sizeof(uint32_t);
        memcpy(cursor, value->u.string_value, length);
        *out_written = sizeof(uint32_t) + length;
      }
      return true;
    case APXDB_TYPE_DATETIME: {
      double number;
      if (!parse_numeric_value(value, &number) || buffer_size < sizeof(int64_t)) {
        return false;
      }
      int64_t stored = (int64_t)number;
      memcpy(cursor, &stored, sizeof(int64_t));
      *out_written = sizeof(int64_t);
      return true;
    }
    case APXDB_TYPE_ENUM: {
      int64_t integer;
      if (!parse_integer_value(value, &integer) || buffer_size < sizeof(int64_t)) {
        return false;
      }
      memcpy(cursor, &integer, sizeof(int64_t));
      *out_written = sizeof(int64_t);
      return true;
    }
    case APXDB_TYPE_EMBEDDED: {
      if (value->type != APXDB_JSON_OBJECT || !field->embedded_schema) {
        return false;
      }
      uint8_t* nested_blob = NULL;
      size_t nested_size = 0;
      if (!serialize_document_blob(field->embedded_schema, value, &nested_blob, &nested_size)) {
        return false;
      }
      if (buffer_size < sizeof(uint32_t) + nested_size) {
        free(nested_blob);
        return false;
      }
      uint32_t nested_u32 = (uint32_t)nested_size;
      memcpy(cursor, &nested_u32, sizeof(uint32_t));
      cursor += sizeof(uint32_t);
      memcpy(cursor, nested_blob, nested_size);
      *out_written = sizeof(uint32_t) + nested_size;
      free(nested_blob);
      return true;
    }
    case APXDB_TYPE_LIST: {
      if (value->type != APXDB_JSON_ARRAY) {
        return false;
      }
      size_t count = value->u.array.count;
      size_t remaining = buffer_size;
      if (remaining < sizeof(uint32_t)) {
        return false;
      }
      uint32_t count_u32 = (uint32_t)count;
      memcpy(cursor, &count_u32, sizeof(uint32_t));
      cursor += sizeof(uint32_t);
      remaining -= sizeof(uint32_t);
      apxdb_schema_field_t element_field = *field;
      element_field.type = field->list_element_type;
      element_field.embedded_schema = field->list_element_schema;
      for (size_t i = 0; i < count; ++i) {
        size_t element_size = 0;
        if (!compute_field_payload_size(&element_field, &value->u.array.values[i], &element_size)) {
          return false;
        }
        if (element_field.type == APXDB_TYPE_STRING || element_field.type == APXDB_TYPE_EMBEDDED) {
          if (remaining < sizeof(uint32_t) + element_size) {
            return false;
          }
          uint32_t element_size_u32 = (uint32_t)element_size;
          memcpy(cursor, &element_size_u32, sizeof(uint32_t));
          cursor += sizeof(uint32_t);
          remaining -= sizeof(uint32_t);
          if (!serialize_field_payload(&element_field, &value->u.array.values[i], cursor, remaining, &element_size)) {
            return false;
          }
          cursor += element_size;
          remaining -= element_size;
        } else {
          if (remaining < element_size) {
            return false;
          }
          if (!serialize_field_payload(&element_field, &value->u.array.values[i], cursor, remaining, &element_size)) {
            return false;
          }
          cursor += element_size;
          remaining -= element_size;
        }
      }
      *out_written = buffer_size - remaining;
      return true;
    }
    default:
      return false;
  }
}

static bool serialize_document_blob(const apxdb_schema_t* schema, const apxdb_json_value_t* document, uint8_t** out_blob, size_t* out_blob_size) {
  if (!schema || !document || !out_blob || !out_blob_size) {
    return false;
  }
  size_t payload_size = 0;
  if (!compute_document_blob_size(schema, document, &payload_size)) {
    return false;
  }
  size_t header_size = sizeof(uint32_t) * 3 + schema->field_count * sizeof(apxdb_document_field_entry_t);
  size_t blob_size = header_size + payload_size;
  uint8_t* blob = (uint8_t*)malloc(blob_size);
  if (!blob) {
    return false;
  }
  uint8_t* cursor = blob;
  uint32_t magic = 0x41584244u; // 'AXBD'
  uint32_t version = 1;
  uint32_t field_count = (uint32_t)schema->field_count;
  memcpy(cursor, &magic, sizeof(uint32_t));
  cursor += sizeof(uint32_t);
  memcpy(cursor, &version, sizeof(uint32_t));
  cursor += sizeof(uint32_t);
  memcpy(cursor, &field_count, sizeof(uint32_t));
  cursor += sizeof(uint32_t);

  uint8_t* payload_cursor = blob + header_size;
  for (size_t i = 0; i < schema->field_count; ++i) {
    const apxdb_schema_field_t* field = &schema->fields[i];
    const apxdb_json_value_t* value = apxdb_json_object_get(document, field->name);
    if (!value) {
      apxdb_json_value_t missing = {.type = APXDB_JSON_NULL};
      value = &missing;
    }
    apxdb_document_field_entry_t entry = {0};
    size_t written = 0;
    if (value->type != APXDB_JSON_NULL) {
      if (!serialize_field_payload(field, value, payload_cursor, blob_size - (payload_cursor - blob), &written)) {
        free(blob);
        return false;
      }
      entry.offset = (uint32_t)(payload_cursor - blob);
      entry.size = (uint32_t)written;
      entry.is_null = 0;
      payload_cursor += written;
    } else {
      entry.offset = 0;
      entry.size = 0;
      entry.is_null = 1;
    }
    memcpy(cursor, &entry, sizeof(apxdb_document_field_entry_t));
    cursor += sizeof(apxdb_document_field_entry_t);
  }

  *out_blob = blob;
  *out_blob_size = blob_size;
  return true;
}

static size_t scalar_type_size(apxdb_type_t type) {
  switch (type) {
    case APXDB_TYPE_BYTE:
      return 1;
    case APXDB_TYPE_SHORT:
      return sizeof(int32_t);
    case APXDB_TYPE_INT:
    case APXDB_TYPE_DATETIME:
    case APXDB_TYPE_ENUM:
      return sizeof(int64_t);
    case APXDB_TYPE_FLOAT:
      return sizeof(float);
    case APXDB_TYPE_DOUBLE:
      return sizeof(double);
    case APXDB_TYPE_BOOL:
      return 1;
    default:
      return 0;
  }
}

static bool get_schema_field_index(const apxdb_schema_t* schema, const char* field_name, size_t* out_index) {
  if (!schema || !field_name || !out_index) {
    return false;
  }
  for (size_t i = 0; i < schema->field_count; ++i) {
    if (strcmp(schema->fields[i].name, field_name) == 0) {
      *out_index = i;
      return true;
    }
  }
  return false;
}

static const apxdb_document_field_entry_t* get_blob_field_entry(const uint8_t* blob, size_t blob_size, size_t field_index) {
  if (!blob || blob_size < sizeof(uint32_t) * 3) {
    return NULL;
  }
  uint32_t field_count;
  memcpy(&field_count, blob + 8, sizeof(uint32_t));
  size_t entries_size = field_count * sizeof(apxdb_document_field_entry_t);
  if (field_index >= field_count || blob_size < sizeof(uint32_t) * 3 + entries_size) {
    return NULL;
  }
  return (const apxdb_document_field_entry_t*)(blob + sizeof(uint32_t) * 3 + field_index * sizeof(apxdb_document_field_entry_t));
}

static bool get_blob_field_payload(const apxdb_schema_t* schema, const uint8_t* blob, size_t blob_size, const char* path, const apxdb_schema_field_t** out_field, const uint8_t** out_payload, size_t* out_payload_size) {
  if (!schema || !blob || !path || !out_field || !out_payload || !out_payload_size) {
    return false;
  }
  char buffer[256];
  size_t path_length = strlen(path);
  if (path_length >= sizeof(buffer)) {
    return false;
  }
  memcpy(buffer, path, path_length + 1);

  const apxdb_schema_t* current_schema = schema;
  const uint8_t* current_blob = blob;
  size_t current_blob_size = blob_size;
  const apxdb_schema_field_t* current_field = NULL;

  char* segment = strtok(buffer, ".");
  while (segment) {
    size_t field_index;
    if (!get_schema_field_index(current_schema, segment, &field_index)) {
      return false;
    }
    const apxdb_document_field_entry_t* entry = get_blob_field_entry(current_blob, current_blob_size, field_index);
    if (!entry || entry->is_null) {
      return false;
    }
    const uint8_t* payload = current_blob + entry->offset;
    size_t payload_size = entry->size;
    char* next_segment = strtok(NULL, ".");
    current_field = &current_schema->fields[field_index];
    if (!next_segment) {
      *out_field = current_field;
      *out_payload = payload;
      *out_payload_size = payload_size;
      return true;
    }
    if (current_field->type == APXDB_TYPE_EMBEDDED && current_field->embedded_schema) {
      if (payload_size < sizeof(uint32_t)) {
        return false;
      }
      uint32_t nested_size;
      memcpy(&nested_size, payload, sizeof(uint32_t));
      if (payload_size < sizeof(uint32_t) + nested_size) {
        return false;
      }
      current_blob = payload + sizeof(uint32_t);
      current_blob_size = nested_size;
      current_schema = current_field->embedded_schema;
      segment = next_segment;
      continue;
    }
    return false;
  }
  return false;
}

static bool blob_payload_to_double(const apxdb_schema_field_t* field, const uint8_t* payload, size_t payload_size, double* out_value) {
  if (!field || !payload || !out_value) {
    return false;
  }
  switch (field->type) {
    case APXDB_TYPE_BYTE:
      if (payload_size < 1) {
        return false;
      }
      *out_value = (double)payload[0];
      return true;
    case APXDB_TYPE_SHORT: {
      if (payload_size < sizeof(int32_t)) {
        return false;
      }
      int32_t value;
      memcpy(&value, payload, sizeof(int32_t));
      *out_value = (double)value;
      return true;
    }
    case APXDB_TYPE_INT:
    case APXDB_TYPE_DATETIME:
    case APXDB_TYPE_ENUM: {
      if (payload_size < sizeof(int64_t)) {
        return false;
      }
      int64_t value;
      memcpy(&value, payload, sizeof(int64_t));
      *out_value = (double)value;
      return true;
    }
    case APXDB_TYPE_FLOAT: {
      if (payload_size < sizeof(float)) {
        return false;
      }
      float value;
      memcpy(&value, payload, sizeof(float));
      *out_value = (double)value;
      return true;
    }
    case APXDB_TYPE_DOUBLE: {
      if (payload_size < sizeof(double)) {
        return false;
      }
      memcpy(out_value, payload, sizeof(double));
      return true;
    }
    default:
      return false;
  }
}

static bool blob_payload_to_bool(const uint8_t* payload, size_t payload_size, bool* out_value) {
  if (!payload || !out_value || payload_size < 1) {
    return false;
  }
  *out_value = payload[0] != 0;
  return true;
}

static char* blob_value_to_string(const apxdb_schema_field_t* field, const uint8_t* payload, size_t payload_size) {
  if (!field || !payload) {
    return NULL;
  }
  char buffer[64];
  switch (field->type) {
    case APXDB_TYPE_BYTE:
      if (payload_size < 1) {
        return NULL;
      }
      snprintf(buffer, sizeof(buffer), "%u", (unsigned)payload[0]);
      return allocate_string_copy(buffer);
    case APXDB_TYPE_SHORT: {
      if (payload_size < sizeof(int32_t)) {
        return NULL;
      }
      int32_t value;
      memcpy(&value, payload, sizeof(int32_t));
      snprintf(buffer, sizeof(buffer), "%d", (int)value);
      return allocate_string_copy(buffer);
    }
    case APXDB_TYPE_INT:
    case APXDB_TYPE_DATETIME:
    case APXDB_TYPE_ENUM: {
      if (payload_size < sizeof(int64_t)) {
        return NULL;
      }
      int64_t value;
      memcpy(&value, payload, sizeof(int64_t));
      snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
      return allocate_string_copy(buffer);
    }
    case APXDB_TYPE_FLOAT: {
      if (payload_size < sizeof(float)) {
        return NULL;
      }
      float value;
      memcpy(&value, payload, sizeof(float));
      snprintf(buffer, sizeof(buffer), "%.*g", 9, value);
      return allocate_string_copy(buffer);
    }
    case APXDB_TYPE_DOUBLE: {
      if (payload_size < sizeof(double)) {
        return NULL;
      }
      double value;
      memcpy(&value, payload, sizeof(double));
      snprintf(buffer, sizeof(buffer), "%.*g", 17, value);
      return allocate_string_copy(buffer);
    }
    case APXDB_TYPE_BOOL: {
      if (payload_size < 1) {
        return NULL;
      }
      return allocate_string_copy(payload[0] ? "true" : "false");
    }
    case APXDB_TYPE_STRING: {
      if (payload_size < sizeof(uint32_t)) {
        return NULL;
      }
      uint32_t length;
      memcpy(&length, payload, sizeof(uint32_t));
      if (payload_size < sizeof(uint32_t) + length) {
        return NULL;
      }
      char* result = (char*)malloc(length + 1);
      if (!result) {
        return NULL;
      }
      memcpy(result, payload + sizeof(uint32_t), length);
      result[length] = '\0';
      return result;
    }
    default:
      return NULL;
  }
}

static bool blob_field_matches_value(const apxdb_schema_field_t* field, const uint8_t* payload, size_t payload_size, const apxdb_json_value_t* query_value);
static bool blob_embedded_object_matches(const apxdb_schema_t* schema, const uint8_t* blob, size_t blob_size, const apxdb_json_value_t* query_value);
static bool blob_list_contains(const apxdb_schema_field_t* field, const uint8_t* payload, size_t payload_size, const apxdb_json_value_t* query_value);

static bool blob_field_matches_value(const apxdb_schema_field_t* field, const uint8_t* payload, size_t payload_size, const apxdb_json_value_t* query_value) {
  if (!field || !payload || !query_value) {
    return false;
  }

  if (field->type == APXDB_TYPE_BYTE || field->type == APXDB_TYPE_SHORT || field->type == APXDB_TYPE_INT || field->type == APXDB_TYPE_FLOAT || field->type == APXDB_TYPE_DOUBLE || field->type == APXDB_TYPE_DATETIME || field->type == APXDB_TYPE_ENUM) {
    double doc_value;
    if (!blob_payload_to_double(field, payload, payload_size, &doc_value)) {
      return false;
    }
    if (query_value->type == APXDB_JSON_OBJECT && query_value->u.object.count == 1) {
      const apxdb_json_member_t* op_member = &query_value->u.object.members[0];
      double threshold;
      if (!parse_numeric_value(op_member->value, &threshold)) {
        return false;
      }
      return compare_numeric_operator(doc_value, op_member->name, threshold);
    }
    double expected;
    if (!parse_numeric_value(query_value, &expected)) {
      return false;
    }
    return doc_value == expected;
  }

  if (field->type == APXDB_TYPE_BOOL) {
    bool doc_bool;
    if (!blob_payload_to_bool(payload, payload_size, &doc_bool)) {
      return false;
    }
    if (query_value->type != APXDB_JSON_BOOL) {
      return false;
    }
    return doc_bool == query_value->u.bool_value;
  }

  if (field->type == APXDB_TYPE_STRING) {
    if (query_value->type != APXDB_JSON_STRING) {
      return false;
    }
    if (payload_size < sizeof(uint32_t)) {
      return false;
    }
    uint32_t length;
    memcpy(&length, payload, sizeof(uint32_t));
    if (payload_size < sizeof(uint32_t) + length) {
      return false;
    }
    return length == strlen(query_value->u.string_value) && memcmp(payload + sizeof(uint32_t), query_value->u.string_value, length) == 0;
  }

  if (field->type == APXDB_TYPE_EMBEDDED) {
    if (query_value->type != APXDB_JSON_OBJECT || !field->embedded_schema) {
      return false;
    }
    if (payload_size < sizeof(uint32_t)) {
      return false;
    }
    uint32_t nested_size;
    memcpy(&nested_size, payload, sizeof(uint32_t));
    if (payload_size < sizeof(uint32_t) + nested_size) {
      return false;
    }
    return blob_embedded_object_matches(field->embedded_schema, payload + sizeof(uint32_t), nested_size, query_value);
  }

  if (field->type == APXDB_TYPE_LIST) {
    return blob_list_contains(field, payload, payload_size, query_value);
  }

  return false;
}

static bool blob_embedded_object_matches(const apxdb_schema_t* schema, const uint8_t* blob, size_t blob_size, const apxdb_json_value_t* query_value) {
  if (!schema || !blob || !query_value || query_value->type != APXDB_JSON_OBJECT) {
    return false;
  }
  for (size_t i = 0; i < query_value->u.object.count; ++i) {
    const apxdb_json_member_t* member = &query_value->u.object.members[i];
    const apxdb_schema_field_t* field = apxdb_schema_find_field(schema, member->name);
    if (!field) {
      return false;
    }
    const uint8_t* payload = NULL;
    size_t payload_size = 0;
    if (!get_blob_field_payload(schema, blob, blob_size, member->name, &field, &payload, &payload_size)) {
      return false;
    }
    if (!blob_field_matches_value(field, payload, payload_size, member->value)) {
      return false;
    }
  }
  return true;
}

static bool blob_list_contains(const apxdb_schema_field_t* field, const uint8_t* payload, size_t payload_size, const apxdb_json_value_t* query_value) {
  if (!field || !payload || !query_value || field->type != APXDB_TYPE_LIST) {
    return false;
  }
  if (payload_size < sizeof(uint32_t)) {
    return false;
  }
  uint32_t count;
  memcpy(&count, payload, sizeof(uint32_t));
  const uint8_t* cursor = payload + sizeof(uint32_t);
  size_t remaining = payload_size - sizeof(uint32_t);
  apxdb_schema_field_t element_field = *field;
  element_field.type = field->list_element_type;
  element_field.embedded_schema = field->list_element_schema;

  for (uint32_t i = 0; i < count; ++i) {
    size_t element_size = 0;
    if (element_field.type == APXDB_TYPE_STRING || element_field.type == APXDB_TYPE_EMBEDDED) {
      if (remaining < sizeof(uint32_t)) {
        return false;
      }
      uint32_t element_size_u32;
      memcpy(&element_size_u32, cursor, sizeof(uint32_t));
      cursor += sizeof(uint32_t);
      remaining -= sizeof(uint32_t);
      element_size = element_size_u32;
    } else {
      element_size = scalar_type_size(element_field.type);
      if (remaining < element_size) {
        return false;
      }
    }
    if (remaining < element_size) {
      return false;
    }
    if (blob_field_matches_value(&element_field, cursor, element_size, query_value)) {
      return true;
    }
    cursor += element_size;
    remaining -= element_size;
  }
  return false;
}

static bool ensure_documents_capacity(apxdb_collection_t* collection) {
  if (collection->document_count >= collection->document_capacity) {
    size_t next_capacity = collection->document_capacity == 0 ? 8 : collection->document_capacity * 2;
    apxdb_collection_document_t* next = (apxdb_collection_document_t*)realloc(collection->documents, next_capacity * sizeof(apxdb_collection_document_t));
    if (!next) {
      return false;
    }
    collection->documents = next;
    collection->document_capacity = next_capacity;
  }
  return true;
}

static bool ensure_index_capacity(apxdb_collection_t* collection) {
  if (collection->index_count >= collection->index_capacity) {
    size_t next_capacity = collection->index_capacity == 0 ? 4 : collection->index_capacity * 2;
    apxdb_index_definition_t* next = (apxdb_index_definition_t*)realloc(collection->indexes, next_capacity * sizeof(apxdb_index_definition_t));
    if (!next) {
      return false;
    }
    collection->indexes = next;
    collection->index_capacity = next_capacity;
  }
  return true;
}

static bool ensure_index_data_capacity(apxdb_collection_t* collection) {
  if (collection->index_count >= collection->index_data_capacity) {
    size_t next_capacity = collection->index_data_capacity == 0 ? 4 : collection->index_data_capacity * 2;
    apxdb_index_data_t* next = (apxdb_index_data_t*)realloc(collection->index_data, next_capacity * sizeof(apxdb_index_data_t));
    if (!next) {
      return false;
    }
    collection->index_data = next;
    collection->index_data_capacity = next_capacity;
  }
  return true;
}

static apxdb_index_data_t* find_index_data(apxdb_collection_t* collection, const char* field_name) {
  if (!collection || !field_name) {
    return NULL;
  }
  for (size_t i = 0; i < collection->index_count; ++i) {
    if (strcmp(collection->index_data[i].definition.field_name, field_name) == 0) {
      return &collection->index_data[i];
    }
  }
  return NULL;
}

static apxdb_index_entry_t* find_index_entry(apxdb_index_data_t* index, const char* key) {
  if (!index || !key) {
    return NULL;
  }
  for (size_t i = 0; i < index->entry_count; ++i) {
    if (strcmp(index->entries[i].key, key) == 0) {
      return &index->entries[i];
    }
  }
  return NULL;
}

static bool ensure_index_entry_capacity(apxdb_index_entry_t* entry) {
  if (entry->doc_count >= entry->doc_capacity) {
    size_t next_capacity = entry->doc_capacity == 0 ? 4 : entry->doc_capacity * 2;
    char** next = (char**)realloc(entry->doc_ids, next_capacity * sizeof(char*));
    if (!next) {
      return false;
    }
    entry->doc_ids = next;
    entry->doc_capacity = next_capacity;
  }
  return true;
}

static bool index_entry_add_doc(apxdb_index_entry_t* entry, const char* doc_id) {
  if (!entry || !doc_id) {
    return false;
  }
  for (size_t i = 0; i < entry->doc_count; ++i) {
    if (strcmp(entry->doc_ids[i], doc_id) == 0) {
      return true;
    }
  }
  if (!ensure_index_entry_capacity(entry)) {
    return false;
  }
  entry->doc_ids[entry->doc_count++] = allocate_string_copy(doc_id);
  return entry->doc_ids[entry->doc_count - 1] != NULL;
}

static void free_index_data(apxdb_index_data_t* index) {
  if (!index) {
    return;
  }
  for (size_t i = 0; i < index->entry_count; ++i) {
    free(index->entries[i].key);
    for (size_t j = 0; j < index->entries[i].doc_count; ++j) {
      free(index->entries[i].doc_ids[j]);
    }
    free(index->entries[i].doc_ids);
  }
  free(index->entries);
}

static void free_collection(apxdb_collection_t* collection) {
  if (!collection) {
    return;
  }
  for (size_t i = 0; i < collection->document_count; ++i) {
    free(collection->documents[i].id);
    free(collection->documents[i].json);
    free(collection->documents[i].blob);
  }
  free(collection->documents);
  free(collection->indexes);
  for (size_t i = 0; i < collection->index_data_count; ++i) {
    free_index_data(&collection->index_data[i]);
  }
  free(collection->index_data);
  free(collection);
}

static apxdb_collection_document_t* find_document_by_id(apxdb_collection_t* collection, const char* id) {
  if (!collection || !id) {
    return NULL;
  }
  for (size_t i = 0; i < collection->document_count; ++i) {
    if (strcmp(collection->documents[i].id, id) == 0) {
      return &collection->documents[i];
    }
  }
  return NULL;
}

static bool has_active_write_txn(void) {
  return g_active_transaction && g_active_transaction->active;
}

static char* build_documents_array_from_ids(apxdb_collection_t* collection, char** doc_ids, size_t doc_id_count) {
  size_t buffer_capacity = 1024;
  char* buffer = (char*)malloc(buffer_capacity);
  if (!buffer) {
    return NULL;
  }
  size_t length = 0;
  buffer[length++] = '[';
  bool first = true;

  for (size_t i = 0; i < doc_id_count; ++i) {
    apxdb_collection_document_t* doc = find_document_by_id(collection, doc_ids[i]);
    if (!doc) {
      continue;
    }
    size_t doc_length = strlen(doc->json);
    size_t needed = length + doc_length + 3;
    if (needed > buffer_capacity) {
      size_t next_capacity = buffer_capacity;
      while (next_capacity < needed) {
        next_capacity *= 2;
      }
      char* next = (char*)realloc(buffer, next_capacity);
      if (!next) {
        free(buffer);
        return NULL;
      }
      buffer = next;
      buffer_capacity = next_capacity;
    }
    if (!first) {
      buffer[length++] = ',';
    }
    memcpy(buffer + length, doc->json, doc_length);
    length += doc_length;
    first = false;
  }

  if (length + 2 > buffer_capacity) {
    char* next = (char*)realloc(buffer, length + 2);
    if (!next) {
      free(buffer);
      return NULL;
    }
    buffer = next;
  }

  buffer[length++] = ']';
  buffer[length] = '\0';
  return buffer;
}

static char* build_documents_array(apxdb_collection_t* collection, const char* query) {
  size_t buffer_capacity = 1024;
  char* buffer = (char*)malloc(buffer_capacity);
  if (!buffer) {
    return NULL;
  }
  size_t length = 0;
  buffer[length++] = '[';

  bool first = true;
  for (size_t i = 0; i < collection->document_count; ++i) {
    const char* doc = collection->documents[i].json;
    if (!query || strstr(doc, query) != NULL) {
      size_t doc_length = strlen(doc);
      size_t needed = length + doc_length + 3;
      if (needed > buffer_capacity) {
        size_t next_capacity = buffer_capacity;
        while (next_capacity < needed) {
          next_capacity *= 2;
        }
        char* next = (char*)realloc(buffer, next_capacity);
        if (!next) {
          free(buffer);
          return NULL;
        }
        buffer = next;
        buffer_capacity = next_capacity;
      }
      if (!first) {
        buffer[length++] = ',';
      }
      memcpy(buffer + length, doc, doc_length);
      length += doc_length;
      first = false;
    }
  }

  if (length + 2 > buffer_capacity) {
    char* next = (char*)realloc(buffer, length + 2);
    if (!next) {
      free(buffer);
      return NULL;
    }
    buffer = next;
  }

  buffer[length++] = ']';
  buffer[length] = '\0';
  return buffer;
}

int32_t apxdb_register_collection(const apxdb_schema_t* schema) {
  if (!schema || !schema->collection_name) {
    return -1;
  }
  if (find_collection(schema->collection_name)) {
    return -1;
  }
  if (!grow_collections()) {
    return -1;
  }
  apxdb_collection_t* collection = (apxdb_collection_t*)calloc(1, sizeof(apxdb_collection_t));
  if (!collection) {
    return -1;
  }
  collection->schema = schema;
  g_collections[g_collection_count++] = collection;
  return 0;
}

int32_t apxdb_add_index(const char* collection_name, const apxdb_index_definition_t* index) {
  if (!collection_name || !index || !index->field_name) {
    return -1;
  }
  apxdb_collection_t* collection = find_collection(collection_name);
  if (!collection) {
    return -1;
  }
  if (!ensure_index_capacity(collection) || !ensure_index_data_capacity(collection)) {
    return -1;
  }
  collection->indexes[collection->index_count] = *index;
  collection->index_data[collection->index_count].definition = *index;
  collection->index_data[collection->index_count].entries = NULL;
  collection->index_data[collection->index_count].entry_count = 0;
  collection->index_data[collection->index_count].entry_capacity = 0;
  collection->index_count += 1;
  collection->index_data_count = collection->index_count;
  return 0;
}

apxdb_transaction_t* apxdb_begin_write_txn(void) {
  if (g_active_transaction && g_active_transaction->active) {
    return NULL;
  }
  apxdb_transaction_t* txn = (apxdb_transaction_t*)malloc(sizeof(apxdb_transaction_t));
  if (!txn) {
    return NULL;
  }
  txn->active = true;
  g_active_transaction = txn;
  return txn;
}

int32_t apxdb_commit_write_txn(apxdb_transaction_t* txn) {
  if (!txn || !txn->active || txn != g_active_transaction) {
    return -1;
  }
  txn->active = false;
  g_active_transaction = NULL;
  return 0;
}

int32_t apxdb_rollback_write_txn(apxdb_transaction_t* txn) {
  if (!txn || !txn->active || txn != g_active_transaction) {
    return -1;
  }
  txn->active = false;
  g_active_transaction = NULL;
  return 0;
}

void apxdb_free_transaction(apxdb_transaction_t* txn) {
  if (!txn) {
    return;
  }
  if (txn == g_active_transaction) {
    g_active_transaction = NULL;
  }
  free(txn);
}

static bool append_document(apxdb_collection_t* collection, const char* id, const char* json, uint8_t* blob, size_t blob_size) {
  if (!collection || !id || !json) {
    free(blob);
    return false;
  }
  if (find_document_by_id(collection, id)) {
    free(blob);
    return false;
  }
  if (!ensure_documents_capacity(collection)) {
    free(blob);
    return false;
  }
  collection->documents[collection->document_count].id = allocate_string_copy(id);
  collection->documents[collection->document_count].json = allocate_string_copy(json);
  collection->documents[collection->document_count].blob = blob;
  collection->documents[collection->document_count].blob_size = blob_size;
  if (!collection->documents[collection->document_count].id || !collection->documents[collection->document_count].json || (!blob && blob_size != 0)) {
    free(collection->documents[collection->document_count].id);
    free(collection->documents[collection->document_count].json);
    free(collection->documents[collection->document_count].blob);
    return false;
  }
  collection->document_count += 1;
  return true;
}

static bool write_uint32_to_file(FILE* file, uint32_t value) {
  return fwrite(&value, 1, sizeof(value), file) == sizeof(value);
}

static bool write_uint64_to_file(FILE* file, uint64_t value) {
  return fwrite(&value, 1, sizeof(value), file) == sizeof(value);
}

static bool read_uint32_from_file(FILE* file, uint32_t* out_value) {
  if (!out_value) {
    return false;
  }
  return fread(out_value, 1, sizeof(*out_value), file) == sizeof(*out_value);
}

static bool read_uint64_from_file(FILE* file, uint64_t* out_value) {
  if (!out_value) {
    return false;
  }
  return fread(out_value, 1, sizeof(*out_value), file) == sizeof(*out_value);
}

static bool write_bytes_to_file(FILE* file, const void* buffer, size_t size) {
  if (size == 0) {
    return true;
  }
  return fwrite(buffer, 1, size, file) == size;
}

static bool read_bytes_from_file(FILE* file, void* buffer, size_t size) {
  if (size == 0) {
    return true;
  }
  return fread(buffer, 1, size, file) == size;
}

static bool parse_numeric_query_op(const apxdb_json_value_t* query_value, int* out_op, int32_t* out_threshold) {
  if (!query_value || !out_op || !out_threshold) {
    return false;
  }
  if (query_value->type == APXDB_JSON_OBJECT && query_value->u.object.count == 1) {
    const apxdb_json_member_t* op_member = &query_value->u.object.members[0];
    double threshold_double;
    if (!parse_numeric_value(op_member->value, &threshold_double)) {
      return false;
    }
    int32_t threshold = (int32_t)threshold_double;
    if (strcmp(op_member->name, "eq") == 0) {
      *out_op = 0;
    } else if (strcmp(op_member->name, "gt") == 0) {
      *out_op = 1;
    } else if (strcmp(op_member->name, "gte") == 0) {
      *out_op = 2;
    } else if (strcmp(op_member->name, "lt") == 0) {
      *out_op = 3;
    } else if (strcmp(op_member->name, "lte") == 0) {
      *out_op = 4;
    } else {
      return false;
    }
    *out_threshold = threshold;
    return true;
  }
  double value_double;
  if (!parse_numeric_value(query_value, &value_double)) {
    return false;
  }
  *out_op = 0;
  *out_threshold = (int32_t)value_double;
  return true;
}

static char** gpu_scan_numeric_docs(apxdb_collection_t* collection, const char* field_path, const apxdb_schema_field_t* field, int op_code, int32_t threshold, size_t* out_count) {
  if (!collection || !field_path || !field || !out_count) {
    return NULL;
  }

  size_t count = collection->document_count;
  int32_t* values = (int32_t*)malloc(count * sizeof(int32_t));
  uint32_t* valid_mask = (uint32_t*)malloc(count * sizeof(uint32_t));
  uint8_t* mask = (uint8_t*)malloc(count);
  if (!values || !valid_mask || !mask) {
    free(values);
    free(valid_mask);
    free(mask);
    return NULL;
  }

  for (size_t i = 0; i < count; ++i) {
    const apxdb_schema_field_t* payload_field = NULL;
    const uint8_t* payload = NULL;
    size_t payload_size = 0;
    if (!get_blob_field_payload(collection->schema, collection->documents[i].blob, collection->documents[i].blob_size, field_path, &payload_field, &payload, &payload_size)) {
      valid_mask[i] = 0;
      values[i] = 0;
      continue;
    }
    if (!payload_field || payload_field->type != APXDB_TYPE_INT || payload_size < sizeof(int64_t)) {
      valid_mask[i] = 0;
      values[i] = 0;
      continue;
    }
    int64_t raw;
    memcpy(&raw, payload, sizeof(raw));
    if (raw < INT32_MIN || raw > INT32_MAX) {
      valid_mask[i] = 0;
      values[i] = 0;
      continue;
    }
    valid_mask[i] = 1;
    values[i] = (int32_t)raw;
  }

  if (!run_gpu_query_int(values, valid_mask, count, op_code, threshold, mask)) {
    free(values);
    free(valid_mask);
    free(mask);
    return NULL;
  }

  char** result = NULL;
  size_t result_capacity = 0;
  *out_count = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!mask[i]) {
      continue;
    }
    if (*out_count >= result_capacity) {
      size_t next_capacity = result_capacity == 0 ? 8 : result_capacity * 2;
      char** next = (char**)realloc(result, next_capacity * sizeof(char*));
      if (!next) {
        free(result);
        free(values);
        free(mask);
        return NULL;
      }
      result = next;
      result_capacity = next_capacity;
    }
    result[(*out_count)++] = collection->documents[i].id;
  }

  free(values);
  free(valid_mask);
  free(mask);
  if (*out_count == 0) {
    result = (char**)malloc(0);
  }
  return result;
}

static void update_document_counter_from_id(const char* id) {
  if (!id) {
    return;
  }
  const char* prefix = "doc_";
  size_t prefix_length = strlen(prefix);
  if (strncmp(id, prefix, prefix_length) != 0) {
    return;
  }
  char* endptr = NULL;
  long long value = strtoll(id + prefix_length, &endptr, 10);
  if (endptr == id + prefix_length || *endptr != '\0' || value <= 0 || value > INT32_MAX) {
    return;
  }
  int32_t expected = atomic_load(&g_collection_counter);
  while ((int32_t)value > expected) {
    atomic_compare_exchange_weak(&g_collection_counter, &expected, (int32_t)value);
  }
}

static bool ensure_directory_exists(const char* path) {
  if (!path) {
    return false;
  }
  struct stat st;
  if (stat(path, &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  if (errno != ENOENT) {
    return false;
  }
  return mkdir(path, 0755) == 0;
}

static bool format_collection_file_path(const char* directory_path, const char* collection_name, char* out_path, size_t out_path_size) {
  if (!directory_path || !collection_name || !out_path || out_path_size == 0) {
    return false;
  }
  int written = snprintf(out_path, out_path_size, "%s/%s.apxdb", directory_path, collection_name);
  return written >= 0 && (size_t)written < out_path_size;
}

int32_t apxdb_save_all_collections(const char* directory_path) {
  if (!directory_path) {
    return -1;
  }
  if (!ensure_directory_exists(directory_path)) {
    return -1;
  }
  for (size_t i = 0; i < g_collection_count; ++i) {
    char path[1024];
    if (!format_collection_file_path(directory_path, g_collections[i]->schema->collection_name, path, sizeof(path))) {
      return -1;
    }
    if (!save_collection_to_file(g_collections[i], path)) {
      return -1;
    }
  }
  return 0;
}

int32_t apxdb_load_all_collections(const char* directory_path) {
  if (!directory_path) {
    return -1;
  }
  if (!ensure_directory_exists(directory_path)) {
    return -1;
  }
  for (size_t i = 0; i < g_collection_count; ++i) {
    char path[1024];
    if (!format_collection_file_path(directory_path, g_collections[i]->schema->collection_name, path, sizeof(path))) {
      return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
      if (errno == ENOENT) {
        continue;
      }
      return -1;
    }
    if (!load_collection_from_file(g_collections[i], path)) {
      return -1;
    }
  }
  return 0;
}

static bool save_collection_to_file(apxdb_collection_t* collection, const char* file_path) {
  if (!collection || !file_path) {
    return false;
  }
  char temp_path[1024];
  int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", file_path);
  if (written < 0 || (size_t)written >= sizeof(temp_path)) {
    return false;
  }
  FILE* file = fopen(temp_path, "wb");
  if (!file) {
    return false;
  }

  const char magic[] = "APXDBCLT";
  if (!write_bytes_to_file(file, magic, sizeof(magic) - 1)) {
    fclose(file);
    return false;
  }
  if (!write_uint32_to_file(file, kCollectionFileVersion)) {
    fclose(file);
    return false;
  }

  uint32_t name_length = (uint32_t)strlen(collection->schema->collection_name);
  if (!write_uint32_to_file(file, name_length) || !write_bytes_to_file(file, collection->schema->collection_name, name_length)) {
    fclose(file);
    return false;
  }

  if (!write_uint32_to_file(file, (uint32_t)collection->document_count)) {
    fclose(file);
    return false;
  }

  for (size_t i = 0; i < collection->document_count; ++i) {
    apxdb_collection_document_t* doc = &collection->documents[i];
    uint32_t id_length = (uint32_t)strlen(doc->id);
    uint32_t json_length = (uint32_t)strlen(doc->json);
    if (!write_uint32_to_file(file, id_length) || !write_bytes_to_file(file, doc->id, id_length) ||
        !write_uint32_to_file(file, json_length) || !write_bytes_to_file(file, doc->json, json_length) ||
        !write_uint64_to_file(file, (uint64_t)doc->blob_size) || !write_bytes_to_file(file, doc->blob, doc->blob_size)) {
      fclose(file);
      remove(temp_path);
      return false;
    }
  }

  fclose(file);
  if (rename(temp_path, file_path) != 0) {
    remove(temp_path);
    return false;
  }
  return true;
}

static bool load_collection_from_file(apxdb_collection_t* collection, const char* file_path) {
  if (!collection || !file_path) {
    return false;
  }
  FILE* file = fopen(file_path, "rb");
  if (!file) {
    return false;
  }

  char magic[8];
  if (!read_bytes_from_file(file, magic, sizeof(magic) - 1)) {
    fclose(file);
    return false;
  }
  magic[sizeof(magic) - 1] = '\0';
  if (strcmp(magic, "APXDBCLT") != 0) {
    fclose(file);
    return false;
  }

  uint32_t version;
  if (!read_uint32_from_file(file, &version) || version != 1) {
    fclose(file);
    return false;
  }

  uint32_t name_length;
  if (!read_uint32_from_file(file, &name_length) || name_length == 0) {
    fclose(file);
    return false;
  }
  char* name_buffer = (char*)malloc(name_length + 1);
  if (!name_buffer || !read_bytes_from_file(file, name_buffer, name_length)) {
    free(name_buffer);
    fclose(file);
    return false;
  }
  name_buffer[name_length] = '\0';
  bool matched_name = strcmp(name_buffer, collection->schema->collection_name) == 0;
  free(name_buffer);
  if (!matched_name) {
    fclose(file);
    return false;
  }

  uint32_t document_count;
  if (!read_uint32_from_file(file, &document_count)) {
    fclose(file);
    return false;
  }

  for (uint32_t i = 0; i < document_count; ++i) {
    uint32_t id_length;
    if (!read_uint32_from_file(file, &id_length) || id_length == 0) {
      fclose(file);
      return false;
    }
    char* id = (char*)malloc(id_length + 1);
    if (!id || !read_bytes_from_file(file, id, id_length)) {
      free(id);
      fclose(file);
      return false;
    }
    id[id_length] = '\0';

    uint32_t json_length;
    if (!read_uint32_from_file(file, &json_length)) {
      free(id);
      fclose(file);
      return false;
    }
    char* json = (char*)malloc(json_length + 1);
    if (!json || !read_bytes_from_file(file, json, json_length)) {
      free(id);
      free(json);
      fclose(file);
      return false;
    }
    json[json_length] = '\0';

    uint64_t blob_size;
    if (!read_uint64_from_file(file, &blob_size)) {
      free(id);
      free(json);
      fclose(file);
      return false;
    }
    uint8_t* blob = NULL;
    if (blob_size > 0) {
      blob = (uint8_t*)malloc((size_t)blob_size);
      if (!blob || !read_bytes_from_file(file, blob, (size_t)blob_size)) {
        free(id);
        free(json);
        free(blob);
        fclose(file);
        return false;
      }
    }

    if (!append_document(collection, id, json, blob, (size_t)blob_size)) {
      free(id);
      free(json);
      free(blob);
      fclose(file);
      return false;
    }
    update_document_counter_from_id(id);

    apxdb_json_value_t doc_value;
    if (!apxdb_json_parse(json, &doc_value)) {
      fclose(file);
      return false;
    }
    if (!index_document(collection, collection->document_count - 1, &doc_value)) {
      apxdb_json_free(&doc_value);
      fclose(file);
      return false;
    }
    apxdb_json_free(&doc_value);

    free(id);
    free(json);
  }

  fclose(file);
  return true;
}

static char* json_value_to_string(const apxdb_json_value_t* value) {
  if (!value) {
    return NULL;
  }
  switch (value->type) {
    case APXDB_JSON_STRING:
      return allocate_string_copy(value->u.string_value);
    case APXDB_JSON_NUMBER: {
      char buffer[64];
      if (value->u.number.is_integer) {
        snprintf(buffer, sizeof(buffer), "%lld", (long long)value->u.number.integer_value);
      } else {
        snprintf(buffer, sizeof(buffer), "%.*g", 17, value->u.number.double_value);
      }
      return allocate_string_copy(buffer);
    }
    case APXDB_JSON_BOOL:
      return allocate_string_copy(value->u.bool_value ? "true" : "false");
    case APXDB_JSON_NULL:
      return allocate_string_copy("null");
    default:
      return NULL;
  }
}

static bool parse_numeric_value(const apxdb_json_value_t* value, double* out_value);
static bool compare_numeric_operator(double doc_value, const char* op, double threshold);
static const apxdb_json_value_t* json_path_get(const apxdb_json_value_t* root, const char* path);

static bool json_value_equal(const apxdb_json_value_t* a, const apxdb_json_value_t* b) {
  if (!a || !b || a->type != b->type) {
    return false;
  }
  switch (a->type) {
    case APXDB_JSON_STRING:
      return strcmp(a->u.string_value, b->u.string_value) == 0;
    case APXDB_JSON_NUMBER:
      return a->u.number.double_value == b->u.number.double_value;
    case APXDB_JSON_BOOL:
      return a->u.bool_value == b->u.bool_value;
    case APXDB_JSON_NULL:
      return true;
    default:
      return false;
  }
}

static bool json_value_matches(const apxdb_json_value_t* field_value, const apxdb_json_value_t* query_value) {
  if (!field_value || !query_value) {
    return false;
  }
  if (query_value->type == APXDB_JSON_OBJECT && query_value->u.object.count == 1) {
    const apxdb_json_member_t* op_member = &query_value->u.object.members[0];
    double threshold;
    if (!parse_numeric_value(op_member->value, &threshold)) {
      return false;
    }
    if (field_value->type != APXDB_JSON_NUMBER) {
      return false;
    }
    return compare_numeric_operator(field_value->u.number.double_value, op_member->name, threshold);
  }

  if (field_value->type == APXDB_JSON_ARRAY) {
    for (size_t i = 0; i < field_value->u.array.count; ++i) {
      if (json_value_equal(&field_value->u.array.values[i], query_value)) {
        return true;
      }
    }
    return false;
  }

  return json_value_equal(field_value, query_value);
}

static char** scan_docs_for_member(apxdb_collection_t* collection, const apxdb_json_member_t* member, size_t* out_count) {
  if (!collection || !member || !out_count) {
    return NULL;
  }

  char** result = NULL;
  size_t result_capacity = 0;
  *out_count = 0;

  const apxdb_schema_field_t* query_field = apxdb_schema_find_field_path(collection->schema, member->name);
  if (query_field && query_field->type == APXDB_TYPE_INT && collection->document_count >= kGpuQueryMinDocs) {
    int op_code;
    int32_t threshold;
    if (parse_numeric_query_op(member->value, &op_code, &threshold)) {
      char** gpu_result = gpu_scan_numeric_docs(collection, member->name, query_field, op_code, threshold, out_count);
      if (gpu_result) {
        return gpu_result;
      }
    }
  }

  for (size_t i = 0; i < collection->document_count; ++i) {
    const apxdb_schema_field_t* field = NULL;
    const uint8_t* payload = NULL;
    size_t payload_size = 0;
    if (!get_blob_field_payload(collection->schema, collection->documents[i].blob, collection->documents[i].blob_size, member->name, &field, &payload, &payload_size)) {
      continue;
    }
    if (!blob_field_matches_value(field, payload, payload_size, member->value)) {
      continue;
    }

    if (*out_count >= result_capacity) {
      size_t next_capacity = result_capacity == 0 ? 8 : result_capacity * 2;
      char** next = (char**)realloc(result, next_capacity * sizeof(char*));
      if (!next) {
        free(result);
        return NULL;
      }
      result = next;
      result_capacity = next_capacity;
    }
    result[(*out_count)++] = collection->documents[i].id;
  }
  if (*out_count == 0) {
    result = (char**)malloc(0);
  }
  return result;
}

static bool remove_doc_id_from_entry(apxdb_index_entry_t* entry, const char* doc_id) {
  if (!entry || !doc_id) {
    return false;
  }
  for (size_t i = 0; i < entry->doc_count; ++i) {
    if (strcmp(entry->doc_ids[i], doc_id) == 0) {
      free(entry->doc_ids[i]);
      for (size_t j = i + 1; j < entry->doc_count; ++j) {
        entry->doc_ids[j - 1] = entry->doc_ids[j];
      }
      entry->doc_count -= 1;
      return true;
    }
  }
  return false;
}

static bool add_doc_id_to_entry(apxdb_index_entry_t* entry, const char* doc_id) {
  if (!entry || !doc_id) {
    return false;
  }
  for (size_t i = 0; i < entry->doc_count; ++i) {
    if (strcmp(entry->doc_ids[i], doc_id) == 0) {
      return true;
    }
  }
  if (!ensure_index_entry_capacity(entry)) {
    return false;
  }
  entry->doc_ids[entry->doc_count] = allocate_string_copy(doc_id);
  if (!entry->doc_ids[entry->doc_count]) {
    return false;
  }
  entry->doc_count += 1;
  return true;
}

static char** copy_doc_id_list(char** source, size_t count) {
  char** result = (char**)malloc(count * sizeof(char*));
  if (!result && count != 0) {
    return NULL;
  }
  for (size_t i = 0; i < count; ++i) {
    result[i] = source[i];
  }
  return result;
}

static char** intersect_doc_id_lists(char** a, size_t a_count, char** b, size_t b_count, size_t* out_count) {
  if (!out_count) {
    return NULL;
  }
  *out_count = 0;
  if (!a || a_count == 0 || !b || b_count == 0) {
    return (char**)malloc(0);
  }
  char** result = NULL;
  size_t result_capacity = 0;
  for (size_t i = 0; i < a_count; ++i) {
    for (size_t j = 0; j < b_count; ++j) {
      if (strcmp(a[i], b[j]) == 0) {
        if (*out_count >= result_capacity) {
          size_t next_capacity = result_capacity == 0 ? 8 : result_capacity * 2;
          char** next = (char**)realloc(result, next_capacity * sizeof(char*));
          if (!next) {
            free(result);
            return NULL;
          }
          result = next;
          result_capacity = next_capacity;
        }
        result[(*out_count)++] = a[i];
        break;
      }
    }
  }
  if (*out_count == 0) {
    free(result);
    return (char**)malloc(0);
  }
  return result;
}

static char** union_doc_id_lists(char** a, size_t a_count, char** b, size_t b_count, size_t* out_count) {
  if (!out_count) {
    return NULL;
  }
  *out_count = 0;
  if ((!a || a_count == 0) && (!b || b_count == 0)) {
    return (char**)malloc(0);
  }
  char** result = NULL;
  size_t result_capacity = 0;

  for (size_t i = 0; i < a_count; ++i) {
    bool found = false;
    for (size_t j = 0; j < *out_count; ++j) {
      if (strcmp(result[j], a[i]) == 0) {
        found = true;
        break;
      }
    }
    if (found) {
      continue;
    }
    if (*out_count >= result_capacity) {
      size_t next_capacity = result_capacity == 0 ? 8 : result_capacity * 2;
      char** next = (char**)realloc(result, next_capacity * sizeof(char*));
      if (!next) {
        free(result);
        return NULL;
      }
      result = next;
      result_capacity = next_capacity;
    }
    result[(*out_count)++] = a[i];
  }
  for (size_t i = 0; i < b_count; ++i) {
    bool found = false;
    for (size_t j = 0; j < *out_count; ++j) {
      if (strcmp(result[j], b[i]) == 0) {
        found = true;
        break;
      }
    }
    if (found) {
      continue;
    }
    if (*out_count >= result_capacity) {
      size_t next_capacity = result_capacity == 0 ? 8 : result_capacity * 2;
      char** next = (char**)realloc(result, next_capacity * sizeof(char*));
      if (!next) {
        free(result);
        return NULL;
      }
      result = next;
      result_capacity = next_capacity;
    }
    result[(*out_count)++] = b[i];
  }
  return result;
}

static bool remove_doc_from_indexes(apxdb_collection_t* collection, const char* doc_id) {
  if (!collection || !doc_id) {
    return false;
  }
  for (size_t i = 0; i < collection->index_data_count; ++i) {
    apxdb_index_data_t* index = &collection->index_data[i];
    for (size_t j = 0; j < index->entry_count; ++j) {
      remove_doc_id_from_entry(&index->entries[j], doc_id);
    }
  }
  return true;
}

static bool add_index_key(apxdb_collection_t* collection, size_t doc_index, const char* field_name, const apxdb_schema_field_t* field, const uint8_t* payload, size_t payload_size) {
  if (!collection || !field_name || !field || !payload) {
    return false;
  }
  apxdb_index_data_t* index = find_index_data(collection, field_name);
  if (!index) {
    return false;
  }

  if (index->definition.index_type == APXDB_INDEX_TYPE_HASH_ELEMENTS) {
    if (field->type != APXDB_TYPE_LIST) {
      return false;
    }
    if (payload_size < sizeof(uint32_t)) {
      return false;
    }
    uint32_t count;
    memcpy(&count, payload, sizeof(uint32_t));
    const uint8_t* cursor = payload + sizeof(uint32_t);
    size_t remaining = payload_size - sizeof(uint32_t);

    apxdb_schema_field_t element_field = *field;
    element_field.type = field->list_element_type;
    element_field.embedded_schema = field->list_element_schema;

    for (uint32_t i = 0; i < count; ++i) {
      size_t element_size;
      if (element_field.type == APXDB_TYPE_STRING || element_field.type == APXDB_TYPE_EMBEDDED) {
        if (remaining < sizeof(uint32_t)) {
          return false;
        }
        uint32_t size_u32;
        memcpy(&size_u32, cursor, sizeof(uint32_t));
        cursor += sizeof(uint32_t);
        remaining -= sizeof(uint32_t);
        element_size = size_u32;
      } else {
        element_size = scalar_type_size(element_field.type);
        if (remaining < element_size) {
          return false;
        }
      }
      if (remaining < element_size) {
        return false;
      }
      char* key = blob_value_to_string(&element_field, cursor, element_size);
      if (!key) {
        return false;
      }
      apxdb_index_entry_t* entry = find_index_entry(index, key);
      if (!entry) {
        if (index->entry_count >= index->entry_capacity) {
          size_t next_capacity = index->entry_capacity == 0 ? 4 : index->entry_capacity * 2;
          apxdb_index_entry_t* next = (apxdb_index_entry_t*)realloc(index->entries, next_capacity * sizeof(apxdb_index_entry_t));
          if (!next) {
            free(key);
            return false;
          }
          index->entries = next;
          index->entry_capacity = next_capacity;
        }
        apxdb_index_entry_t* new_entry = &index->entries[index->entry_count++];
        new_entry->key = key;
        new_entry->doc_ids = NULL;
        new_entry->doc_count = 0;
        new_entry->doc_capacity = 0;
        if (!add_doc_id_to_entry(new_entry, collection->documents[doc_index].id)) {
          return false;
        }
      } else {
        if (!add_doc_id_to_entry(entry, collection->documents[doc_index].id)) {
          free(key);
          return false;
        }
        free(key);
      }
      cursor += element_size;
      remaining -= element_size;
    }
    return true;
  }

  char* key = blob_value_to_string(field, payload, payload_size);
  if (!key) {
    return false;
  }
  apxdb_index_entry_t* entry = find_index_entry(index, key);
  if (!entry) {
    if (index->entry_count >= index->entry_capacity) {
      size_t next_capacity = index->entry_capacity == 0 ? 4 : index->entry_capacity * 2;
      apxdb_index_entry_t* next = (apxdb_index_entry_t*)realloc(index->entries, next_capacity * sizeof(apxdb_index_entry_t));
      if (!next) {
        free(key);
        return false;
      }
      index->entries = next;
      index->entry_capacity = next_capacity;
    }
    apxdb_index_entry_t* new_entry = &index->entries[index->entry_count++];
    new_entry->key = key;
    new_entry->doc_ids = NULL;
    new_entry->doc_count = 0;
    new_entry->doc_capacity = 0;
    return add_doc_id_to_entry(new_entry, collection->documents[doc_index].id);
  }
  bool ok = add_doc_id_to_entry(entry, collection->documents[doc_index].id);
  free(key);
  return ok;
}

static const apxdb_json_value_t* json_path_get(const apxdb_json_value_t* value, const char* path) {
  if (!value || !path) {
    return NULL;
  }
  char buffer[256];
  size_t length = strlen(path);
  if (length >= sizeof(buffer)) {
    return NULL;
  }
  memcpy(buffer, path, length + 1);

  const apxdb_json_value_t* current = value;
  char* segment = strtok(buffer, ".");
  while (segment && current) {
    if (current->type == APXDB_JSON_OBJECT) {
      current = apxdb_json_object_get(current, segment);
    } else if (current->type == APXDB_JSON_ARRAY) {
      bool all_digits = true;
      size_t idx = 0;
      for (char* p = segment; *p; ++p) {
        if (!(*p >= '0' && *p <= '9')) {
          all_digits = false;
          break;
        }
        idx = idx * 10 + (*p - '0');
      }
      if (!all_digits) {
        return NULL;
      }
      current = apxdb_json_array_get(current, idx);
    } else {
      return NULL;
    }
    segment = strtok(NULL, ".");
  }
  return current;
}

static bool index_document(apxdb_collection_t* collection, size_t doc_index, const apxdb_json_value_t* document) {
  if (!collection || !document || document->type != APXDB_JSON_OBJECT) {
    return false;
  }
  const uint8_t* blob = collection->documents[doc_index].blob;
  size_t blob_size = collection->documents[doc_index].blob_size;
  for (size_t i = 0; i < collection->index_count; ++i) {
    const apxdb_index_definition_t* index_def = &collection->index_data[i].definition;
    const apxdb_schema_field_t* field = apxdb_schema_find_field(collection->schema, index_def->field_name);
    if (!field) {
      continue;
    }
    const uint8_t* payload = NULL;
    size_t payload_size = 0;
    if (!get_blob_field_payload(collection->schema, blob, blob_size, index_def->field_name, &field, &payload, &payload_size)) {
      continue;
    }
    if (!add_index_key(collection, doc_index, index_def->field_name, field, payload, payload_size)) {
      return false;
    }
  }
  return true;
}

static char* build_document_id(void) {
  int32_t id = atomic_fetch_add(&g_collection_counter, 1) + 1;
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "doc_%d", id);
  return allocate_string_copy(buffer);
}

static bool validate_enum_field(const apxdb_schema_field_t* field, const apxdb_json_value_t* value) {
  if (!field || !value) {
    return false;
  }
  if (value->type == APXDB_JSON_NULL) {
    return field->nullable;
  }
  if (field->enum_value_count == 0) {
    return false;
  }

  switch (field->enum_strategy) {
    case APXDB_ENUM_STRATEGY_NAME:
      if (value->type != APXDB_JSON_STRING) {
        return false;
      }
      for (size_t i = 0; i < field->enum_value_count; ++i) {
        if (strcmp(field->enum_values[i].name, value->u.string_value) == 0) {
          return true;
        }
      }
      return false;
    case APXDB_ENUM_STRATEGY_VALUE:
      if (value->type != APXDB_JSON_NUMBER || !value->u.number.is_integer) {
        return false;
      }
      for (size_t i = 0; i < field->enum_value_count; ++i) {
        if (field->enum_values[i].value == value->u.number.integer_value) {
          return true;
        }
      }
      return false;
    case APXDB_ENUM_STRATEGY_ORDINAL:
    case APXDB_ENUM_STRATEGY_ORDINAL32:
      if (value->type != APXDB_JSON_NUMBER || !value->u.number.is_integer) {
        return false;
      }
      return value->u.number.integer_value >= 0 && (size_t)value->u.number.integer_value < field->enum_value_count;
    default:
      return false;
  }
}

static bool validate_json_value(const apxdb_schema_field_t* field, const apxdb_json_value_t* value);

static bool validate_json_array(const apxdb_schema_field_t* field, const apxdb_json_value_t* value) {
  if (!field || !value) {
    return false;
  }
  if (value->type == APXDB_JSON_NULL) {
    return field->nullable;
  }
  if (value->type != APXDB_JSON_ARRAY) {
    return false;
  }
  for (size_t i = 0; i < value->u.array.count; ++i) {
    apxdb_json_value_t* element = &value->u.array.values[i];
    apxdb_schema_field_t element_field = *field;
    element_field.type = field->list_element_type;
    element_field.embedded_schema = field->list_element_schema;
    if (!validate_json_value(&element_field, element)) {
      return false;
    }
  }
  return true;
}

static bool validate_json_object(const apxdb_schema_t* schema, const apxdb_json_value_t* object) {
  if (!schema || !object || object->type != APXDB_JSON_OBJECT) {
    return false;
  }
  for (size_t i = 0; i < schema->field_count; ++i) {
    const apxdb_schema_field_t* field = &schema->fields[i];
    const apxdb_json_value_t* field_value = apxdb_json_object_get(object, field->name);
    if (!field_value) {
      if (!field->nullable) {
        return false;
      }
      continue;
    }
    if (field_value->type == APXDB_JSON_NULL) {
      if (!field->nullable) {
        return false;
      }
      continue;
    }
    if (!validate_json_value(field, field_value)) {
      return false;
    }
  }
  return true;
}

static bool validate_json_value(const apxdb_schema_field_t* field, const apxdb_json_value_t* value) {
  if (!field || !value) {
    return false;
  }
  if (value->type == APXDB_JSON_NULL) {
    return field->nullable;
  }

  switch (field->type) {
    case APXDB_TYPE_BYTE:
      return value->type == APXDB_JSON_NUMBER && value->u.number.is_integer && value->u.number.integer_value >= 0 && value->u.number.integer_value <= 255;
    case APXDB_TYPE_SHORT:
      return value->type == APXDB_JSON_NUMBER && value->u.number.is_integer && value->u.number.integer_value >= INT32_MIN && value->u.number.integer_value <= INT32_MAX;
    case APXDB_TYPE_INT:
      return value->type == APXDB_JSON_NUMBER && value->u.number.is_integer;
    case APXDB_TYPE_FLOAT:
      return value->type == APXDB_JSON_NUMBER;
    case APXDB_TYPE_DOUBLE:
      return value->type == APXDB_JSON_NUMBER;
    case APXDB_TYPE_BOOL:
      return value->type == APXDB_JSON_BOOL;
    case APXDB_TYPE_STRING:
      return value->type == APXDB_JSON_STRING;
    case APXDB_TYPE_DATETIME:
      return value->type == APXDB_JSON_NUMBER || value->type == APXDB_JSON_STRING;
    case APXDB_TYPE_EMBEDDED:
      return value->type == APXDB_JSON_OBJECT && field->embedded_schema && validate_json_object(field->embedded_schema, value);
    case APXDB_TYPE_ENUM:
      return validate_enum_field(field, value);
    case APXDB_TYPE_LIST:
      return validate_json_array(field, value);
    default:
      return false;
  }
}

static bool validate_document_against_schema(const apxdb_schema_t* schema, const char* json_utf8) {
  if (!schema || !json_utf8) {
    return false;
  }
  apxdb_json_value_t root;
  if (!apxdb_json_parse(json_utf8, &root)) {
    return false;
  }
  bool result = validate_json_object(schema, &root);
  apxdb_json_free(&root);
  return result;
}

const char* apxdb_get_document(const char* collection_name, const char* id) {
  if (!collection_name || !id) {
    return NULL;
  }
  apxdb_collection_t* collection = find_collection(collection_name);
  if (!collection) {
    return NULL;
  }
  apxdb_collection_document_t* doc = find_document_by_id(collection, id);
  if (!doc) {
    return NULL;
  }
  return allocate_string_copy(doc->json);
}

static bool parse_numeric_value(const apxdb_json_value_t* value, double* out_value) {
  if (!value || !out_value) {
    return false;
  }
  if (value->type == APXDB_JSON_NUMBER) {
    *out_value = value->u.number.double_value;
    return true;
  }
  if (value->type == APXDB_JSON_STRING) {
    char* endptr = NULL;
    double result = strtod(value->u.string_value, &endptr);
    if (endptr == value->u.string_value || *endptr != '\0') {
      return false;
    }
    *out_value = result;
    return true;
  }
  return false;
}

static bool compare_numeric_operator(double doc_value, const char* op, double threshold) {
  if (strcmp(op, "gt") == 0) {
    return doc_value > threshold;
  }
  if (strcmp(op, "gte") == 0) {
    return doc_value >= threshold;
  }
  if (strcmp(op, "lt") == 0) {
    return doc_value < threshold;
  }
  if (strcmp(op, "lte") == 0) {
    return doc_value <= threshold;
  }
  if (strcmp(op, "eq") == 0) {
    return doc_value == threshold;
  }
  return false;
}

static char** collect_docs_by_range(apxdb_collection_t* collection, apxdb_index_data_t* index, const char* op, double threshold, size_t* out_count) {
  if (!collection || !index || !op || !out_count) {
    return NULL;
  }

  char** results = NULL;
  size_t result_count = 0;
  size_t result_capacity = 0;

  for (size_t i = 0; i < index->entry_count; ++i) {
    double key_value;
    if (!parse_numeric_value((apxdb_json_value_t*)&(apxdb_json_value_t){.type = APXDB_JSON_STRING, .u.string_value = index->entries[i].key}, &key_value)) {
      continue;
    }
    if (!compare_numeric_operator(key_value, op, threshold)) {
      continue;
    }
    for (size_t j = 0; j < index->entries[i].doc_count; ++j) {
      if (result_count >= result_capacity) {
        size_t next_capacity = result_capacity == 0 ? 8 : result_capacity * 2;
        char** next = (char**)realloc(results, next_capacity * sizeof(char*));
        if (!next) {
          free(results);
          return NULL;
        }
        results = next;
        result_capacity = next_capacity;
      }
      results[result_count++] = index->entries[i].doc_ids[j];
    }
  }

  *out_count = result_count;
  return results;
}

static char** find_docs_for_member(apxdb_collection_t* collection, const apxdb_json_member_t* member, size_t* out_count);

static char** find_docs_for_member(apxdb_collection_t* collection, const apxdb_json_member_t* member, size_t* out_count) {
  if (!collection || !member || !out_count) {
    return NULL;
  }

  apxdb_index_data_t* index = find_index_data(collection, member->name);
  if (index) {
    if (member->value->type == APXDB_JSON_OBJECT) {
      if (index->definition.index_type == APXDB_INDEX_TYPE_VALUE && member->value->u.object.count == 1) {
        const apxdb_json_member_t* op_member = &member->value->u.object.members[0];
        double threshold;
        if (parse_numeric_value(op_member->value, &threshold)) {
          return collect_docs_by_range(collection, index, op_member->name, threshold, out_count);
        }
      }
    } else {
      char* key = json_value_to_string(member->value);
      if (key) {
        apxdb_index_entry_t* entry = find_index_entry(index, key);
        free(key);
        if (entry) {
          *out_count = entry->doc_count;
          return copy_doc_id_list(entry->doc_ids, entry->doc_count);
        }
      }
    }
  }

  return scan_docs_for_member(collection, member, out_count);
}

static char** find_docs_by_query(apxdb_collection_t* collection, const apxdb_json_value_t* query, size_t* out_count, bool* out_owned) {
  if (!collection || !query || !out_count || !out_owned || query->type != APXDB_JSON_OBJECT) {
    return NULL;
  }
  char** result = NULL;
  size_t result_count = 0;
  *out_owned = true;

  for (size_t i = 0; i < query->u.object.count; ++i) {
    const apxdb_json_member_t* member = &query->u.object.members[i];
    char** member_ids = NULL;
    size_t member_count = 0;

    if (strcmp(member->name, "$and") == 0 && member->value->type == APXDB_JSON_ARRAY) {
      size_t child_count = member->value->u.array.count;
      char** child_result = NULL;
      size_t child_result_count = 0;
      for (size_t j = 0; j < child_count; ++j) {
        const apxdb_json_value_t* child_query = &member->value->u.array.values[j];
        bool child_owned = false;
        char** it_ids = find_docs_by_query(collection, child_query, &member_count, &child_owned);
        if (!it_ids) {
          if (child_result) {
            free(child_result);
          }
          free(result);
          return NULL;
        }
        if (!child_result) {
          child_result = it_ids;
          child_result_count = member_count;
        } else {
          size_t next_count = 0;
          char** next = intersect_doc_id_lists(child_result, child_result_count, it_ids, member_count, &next_count);
          free(child_result);
          if (child_owned) {
            free(it_ids);
          }
          if (!next) {
            free(result);
            return NULL;
          }
          child_result = next;
          child_result_count = next_count;
        }
      }
      member_ids = child_result;
      member_count = child_result_count;
    } else if (strcmp(member->name, "$or") == 0 && member->value->type == APXDB_JSON_ARRAY) {
      size_t child_count = member->value->u.array.count;
      char** or_result = NULL;
      size_t or_count = 0;
      for (size_t j = 0; j < child_count; ++j) {
        const apxdb_json_value_t* child_query = &member->value->u.array.values[j];
        bool child_owned = false;
        char** it_ids = find_docs_by_query(collection, child_query, &member_count, &child_owned);
        if (!it_ids) {
          continue;
        }
        if (!or_result) {
          or_result = it_ids;
          or_count = member_count;
        } else {
          size_t next_count = 0;
          char** next = union_doc_id_lists(or_result, or_count, it_ids, member_count, &next_count);
          free(or_result);
          if (child_owned) {
            free(it_ids);
          }
          if (!next) {
            free(result);
            return NULL;
          }
          or_result = next;
          or_count = next_count;
        }
      }
      if (!or_result) {
        member_ids = (char**)malloc(0);
        member_count = 0;
      } else {
        member_ids = or_result;
        member_count = or_count;
      }
    } else {
      member_ids = find_docs_for_member(collection, member, &member_count);
      if (!member_ids) {
        free(result);
        return NULL;
      }
    }

    if (!result) {
      result = member_ids;
      result_count = member_count;
    } else {
      size_t next_count = 0;
      char** next = intersect_doc_id_lists(result, result_count, member_ids, member_count, &next_count);
      free(result);
      free(member_ids);
      if (!next) {
        return NULL;
      }
      result = next;
      result_count = next_count;
    }
  }

  *out_count = result_count;
  return result;
}

const char* apxdb_find_documents(const char* collection_name, const char* query_json) {
  if (!collection_name) {
    return NULL;
  }
  apxdb_collection_t* collection = find_collection(collection_name);
  if (!collection) {
    return NULL;
  }
  if (query_json && *query_json) {
    apxdb_json_value_t query;
    if (apxdb_json_parse(query_json, &query)) {
      size_t doc_count = 0;
      bool owned = false;
      char** doc_ids = find_docs_by_query(collection, &query, &doc_count, &owned);
      apxdb_json_free(&query);
      if (doc_ids || doc_count == 0) {
        char* result = build_documents_array_from_ids(collection, doc_ids, doc_count);
        if (owned && doc_ids) {
          free(doc_ids);
        }
        return result;
      }
    }
  }
  return build_documents_array(collection, query_json);
}

const char* apxdb_put_document(const char* collection_name, const char* json_utf8) {
  if (!collection_name || !json_utf8) {
    return NULL;
  }
  if (!has_active_write_txn()) {
    return NULL;
  }
  apxdb_collection_t* collection = find_collection(collection_name);
  if (!collection) {
    return NULL;
  }
  if (!validate_document_against_schema(collection->schema, json_utf8)) {
    return NULL;
  }
  char* id = build_document_id();
  if (!id) {
    return NULL;
  }

  apxdb_json_value_t doc_value;
  if (!apxdb_json_parse(json_utf8, &doc_value)) {
    free(id);
    return NULL;
  }

  uint8_t* blob = NULL;
  size_t blob_size = 0;
  if (!serialize_document_blob(collection->schema, &doc_value, &blob, &blob_size)) {
    apxdb_json_free(&doc_value);
    free(id);
    return NULL;
  }

  if (!append_document(collection, id, json_utf8, blob, blob_size)) {
    apxdb_json_free(&doc_value);
    free(id);
    return NULL;
  }

  if (!index_document(collection, collection->document_count - 1, &doc_value)) {
    apxdb_json_free(&doc_value);
    remove_doc_from_indexes(collection, id);
    if (collection->document_count > 0) {
      free(collection->documents[collection->document_count - 1].id);
      free(collection->documents[collection->document_count - 1].json);
      free(collection->documents[collection->document_count - 1].blob);
      collection->document_count -= 1;
    }
    free(id);
    return NULL;
  }
  apxdb_json_free(&doc_value);
  return id;
}

int32_t apxdb_delete_document(const char* collection_name, const char* id) {
  if (!collection_name || !id) {
    return -1;
  }
  if (!has_active_write_txn()) {
    return -1;
  }
  apxdb_collection_t* collection = find_collection(collection_name);
  if (!collection) {
    return -1;
  }
  for (size_t i = 0; i < collection->document_count; ++i) {
    if (strcmp(collection->documents[i].id, id) == 0) {
      remove_doc_from_indexes(collection, id);
      free(collection->documents[i].id);
      free(collection->documents[i].json);
      free(collection->documents[i].blob);
      for (size_t j = i + 1; j < collection->document_count; ++j) {
        collection->documents[j - 1] = collection->documents[j];
      }
      collection->document_count -= 1;
      return 0;
    }
  }
  return -1;
}

int32_t apxdb_save_collection(const char* collection_name, const char* file_path) {
  if (!collection_name || !file_path) {
    return -1;
  }
  apxdb_collection_t* collection = find_collection(collection_name);
  if (!collection) {
    return -1;
  }
  return save_collection_to_file(collection, file_path) ? 0 : -1;
}

int32_t apxdb_load_collection(const char* collection_name, const char* file_path) {
  if (!collection_name || !file_path) {
    return -1;
  }
  apxdb_collection_t* collection = find_collection(collection_name);
  if (!collection) {
    return -1;
  }
  return load_collection_from_file(collection, file_path) ? 0 : -1;
}

static void free_collections(void) {
  for (size_t i = 0; i < g_collection_count; ++i) {
    free_collection(g_collections[i]);
  }
  free(g_collections);
  g_collections = NULL;
  g_collection_count = 0;
  g_collection_capacity = 0;
  if (g_active_transaction) {
    free(g_active_transaction);
    g_active_transaction = NULL;
  }
}

void apxdb_collection_unregister_all(void) {
  free_collections();
  atomic_store(&g_collection_counter, 0);
}
