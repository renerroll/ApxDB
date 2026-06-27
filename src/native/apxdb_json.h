#ifndef APXDB_JSON_H
#define APXDB_JSON_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  APXDB_JSON_NULL,
  APXDB_JSON_BOOL,
  APXDB_JSON_NUMBER,
  APXDB_JSON_STRING,
  APXDB_JSON_ARRAY,
  APXDB_JSON_OBJECT,
  APXDB_JSON_INVALID,
} apxdb_json_type_t;

typedef struct apxdb_json_value apxdb_json_value_t;

typedef struct {
  size_t count;
  apxdb_json_value_t* values;
} apxdb_json_array_t;

typedef struct {
  char* name;
  apxdb_json_value_t* value;
} apxdb_json_member_t;

typedef struct {
  size_t count;
  apxdb_json_member_t* members;
} apxdb_json_object_t;

struct apxdb_json_value {
  apxdb_json_type_t type;
  union {
    bool bool_value;
    struct {
      bool is_integer;
      int64_t integer_value;
      double double_value;
    } number;
    char* string_value;
    apxdb_json_array_t array;
    apxdb_json_object_t object;
  } u;
};

bool apxdb_json_parse(const char* text, apxdb_json_value_t* out_value);
void apxdb_json_free(apxdb_json_value_t* value);
const apxdb_json_value_t* apxdb_json_object_get(const apxdb_json_value_t* object, const char* name);
const apxdb_json_value_t* apxdb_json_array_get(const apxdb_json_value_t* array, size_t index);

#ifdef __cplusplus
}
#endif

#endif // APXDB_JSON_H
