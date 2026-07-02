#ifndef APXDB_NATIVE_H
#define APXDB_NATIVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "apxdb_schema.h"
#include "apxdb_collection.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  APXDB_STATE_CLOSED = 0,
  APXDB_STATE_OPENING,
  APXDB_STATE_OPEN,
  APXDB_STATE_CLOSING,
  APXDB_STATE_FAILED,
} apxdb_state_t;

typedef enum {
  APXDB_GPU_UNAVAILABLE = 0,
  APXDB_GPU_METAL_ACTIVE,
  APXDB_GPU_VULKAN_ACTIVE,
  APXDB_GPU_DISABLED_BY_THRESHOLD,
  APXDB_GPU_INIT_FAILED,
} apxdb_gpu_status_t;

// Debug-only diagnostics API. These helpers expose global runtime state
// for the last observed query, and are not guaranteed to be correct under
// concurrent query execution, in isolates, or in multithreaded scenarios.
//
// In production or future versions, prefer per-query metadata instead of
// shared last-query globals.
// Define APXDB_ENABLE_DIAGNOSTICS to enable runtime recording of this state.

typedef enum {
  APXDB_QUERY_CPU_ONLY = 0,
  APXDB_QUERY_GPU_USED,
  APXDB_QUERY_GPU_SKIPPED_THRESHOLD,
  APXDB_QUERY_GPU_SKIPPED_UNSUPPORTED_TYPE,
  APXDB_QUERY_GPU_SKIPPED_BACKEND_UNAVAILABLE,
} apxdb_query_path_t;

enum {
  APXDB_OK = 0,
  APXDB_OK_GPU_FALLBACK = 10,
  APXDB_ERR_ALREADY_OPEN = 1,
  APXDB_ERR_NOT_OPEN = 2,
  APXDB_ERR_GPU_INIT_FAILED = 3,
  APXDB_ERR_PARTIAL_RECOVERY = 4,
  APXDB_ERR_INVALID_ARGUMENT = 5,
  APXDB_ERR_IO = 6,
  APXDB_ERR_UNKNOWN = 255,
};

int32_t apxdb_initialize();
int32_t apxdb_shutdown();
int32_t apxdb_open(const char* directory_path);
int32_t apxdb_close(void);
int32_t apxdb_gpu_status(void);
void apxdb_set_gpu_enabled(bool enabled);

// Debug-only helpers. Use for single-threaded diagnostics and benchmark
// harnesses only. These values are global and are not thread-safe.
typedef struct {
  uint64_t bytes_uploaded;
  uint64_t bytes_reused;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t result_count;
  uint64_t cpu_prepare_ns;
  uint64_t gpu_exec_ns;
  uint64_t total_ns;
  apxdb_query_path_t path;
} apxdb_query_metrics_t;

typedef enum {
  APXDB_FIELD_INT32 = 0,
  APXDB_FIELD_INT64,
  APXDB_FIELD_BOOL,
  APXDB_FIELD_DOUBLE,
  APXDB_FIELD_STRING,
  APXDB_FIELD_BYTES,
} apxdb_field_type_t;

typedef enum {
  APXDB_STORAGE_FIXED = 0,
  APXDB_STORAGE_VARIABLE,
} apxdb_storage_kind_t;

typedef enum {
  APXDB_INDEX_EXACT = 0,
  APXDB_INDEX_RANGE,
  APXDB_INDEX_COMPOSITE,
} apxdb_index_kind_t;

enum {
  APXDB_FIELD_FLAG_NULLABLE = 1u << 0,
  APXDB_FIELD_FLAG_INDEXABLE = 1u << 1,
  APXDB_FIELD_FLAG_INDEXED = 1u << 2,
  APXDB_FIELD_FLAG_UNIQUE = 1u << 3,
  APXDB_FIELD_FLAG_FIXED_SIZE = 1u << 4,
};

enum {
  APXDB_INDEX_FLAG_UNIQUE = 1u << 0,
  APXDB_INDEX_FLAG_DESCENDING = 1u << 1,
};

typedef struct apxdb_field_schema_t {
  uint32_t struct_size;
  uint32_t version;
  uint32_t field_id;
  const char* name;
  apxdb_field_type_t type;
  uint32_t flags;
  uint32_t type_size;
  apxdb_storage_kind_t storage_kind;
} apxdb_field_schema_t;

typedef struct apxdb_index_schema_t {
  uint32_t struct_size;
  uint32_t version;
  uint32_t index_id;
  const char* name;
  apxdb_index_kind_t kind;
  uint32_t flags;
  uint32_t field_count;
  const uint32_t* field_ids;
} apxdb_index_schema_t;

typedef struct apxdb_collection_schema_t {
  uint32_t struct_size;
  uint32_t version;
  uint32_t collection_id;
  const char* name;
  uint32_t field_count;
  const apxdb_field_schema_t* fields;
  uint32_t index_count;
  const apxdb_index_schema_t* indexes;
} apxdb_collection_schema_t;

const apxdb_collection_schema_t* apxdb_register_schema(const apxdb_collection_schema_t* schema);
const apxdb_collection_schema_t* apxdb_find_collection_schema_by_name(const char* name);
const apxdb_collection_schema_t* apxdb_find_collection_schema_by_id(uint32_t collection_id);
void apxdb_unregister_all_schemas(void);

int32_t apxdb_last_query_path(void);
uint32_t apxdb_last_query_doc_count(void);
int32_t apxdb_last_query_metrics(apxdb_query_metrics_t* out_metrics);
const char* apxdb_create_document(const char* json_utf8);
const char* apxdb_create_document_bytes(const uint8_t* bytes, size_t length);
const uint8_t* apxdb_get_document_bytes(const char* id, size_t* out_length);
const char* apxdb_find_document(const char* query_utf8);
void apxdb_release_string(const char* utf8);
void apxdb_release_bytes(const uint8_t* bytes);

#ifdef __cplusplus
}
#endif

#endif // APXDB_NATIVE_H
