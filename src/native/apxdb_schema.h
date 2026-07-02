#ifndef APXDB_SCHEMA_H
#define APXDB_SCHEMA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

// apxdb_schema.h
// Public schema metadata API for ApxDB native core.
// This header defines schema-related object types and runtime
// declarations used by other modules.

typedef enum {
  APXDB_TYPE_BYTE,
  APXDB_TYPE_SHORT,
  APXDB_TYPE_INT,
  APXDB_TYPE_FLOAT,
  APXDB_TYPE_DOUBLE,
  APXDB_TYPE_BOOL,
  APXDB_TYPE_STRING,
  APXDB_TYPE_DATETIME,
  APXDB_TYPE_EMBEDDED,
  APXDB_TYPE_ENUM,
  APXDB_TYPE_LIST,
} apxdb_type_t;

typedef enum {
  APXDB_ENUM_STRATEGY_ORDINAL,
  APXDB_ENUM_STRATEGY_ORDINAL32,
  APXDB_ENUM_STRATEGY_NAME,
  APXDB_ENUM_STRATEGY_VALUE,
} apxdb_enum_strategy_t;

#define APXDB_INT_NULL INT64_MIN
#define APXDB_FLOAT_NULL NAN
#define APXDB_DOUBLE_NULL NAN
#define APXDB_BOOL_NULL 2

typedef union {
  uint8_t byte_value;
  int32_t short_value;
  int64_t int_value;
  float float_value;
  double double_value;
  uint8_t bool_value;
  const char* string_value;
  int64_t datetime_value;
  const void* opaque;
} apxdb_value_t;

struct apxdb_schema_t;

typedef struct {
  const char* name;
  int64_t value;
} apxdb_enum_value_t;

typedef struct {
  const char* name;
  apxdb_type_t type;
  bool nullable;
  const struct apxdb_schema_t* embedded_schema;
  apxdb_type_t list_element_type;
  const struct apxdb_schema_t* list_element_schema;
  apxdb_enum_strategy_t enum_strategy;
  const apxdb_enum_value_t* enum_values;
  size_t enum_value_count;
} apxdb_schema_field_t;

typedef struct apxdb_schema_t {
  const char* collection_name;
  const apxdb_schema_field_t* fields;
  size_t field_count;
} apxdb_schema_t;

// Schema registry API
const apxdb_schema_t* apxdb_schema_register(const apxdb_schema_t* schema);
const apxdb_schema_t* apxdb_schema_find(const char* collection_name);
void apxdb_schema_unregister_all(void);

// Schema helper utilities
const apxdb_schema_field_t* apxdb_schema_find_field(const apxdb_schema_t* schema, const char* field_name);
const apxdb_schema_field_t* apxdb_schema_find_field_path(const apxdb_schema_t* schema, const char* field_path);
bool apxdb_schema_field_is_list(const apxdb_schema_field_t* field);
bool apxdb_schema_field_is_embedded(const apxdb_schema_field_t* field);
bool apxdb_schema_field_is_nullable(const apxdb_schema_field_t* field);
const apxdb_schema_t* apxdb_schema_field_list_element_schema(const apxdb_schema_field_t* field);
apxdb_type_t apxdb_schema_field_list_element_type(const apxdb_schema_field_t* field);
const char* apxdb_type_name(apxdb_type_t type);
bool apxdb_value_is_null(apxdb_type_t type, apxdb_value_t value);

#ifdef __cplusplus
}
#endif

#endif // APXDB_SCHEMA_H
