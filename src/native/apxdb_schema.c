#include "apxdb_schema.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const apxdb_schema_t** g_schema_registry = NULL;
static size_t g_schema_registry_count = 0;
static size_t g_schema_registry_capacity = 0;

static bool grow_registry(void) {
  if (g_schema_registry_count >= g_schema_registry_capacity) {
    size_t next_capacity = g_schema_registry_capacity == 0 ? 8 : g_schema_registry_capacity * 2;
    const apxdb_schema_t** next = (const apxdb_schema_t**)realloc(g_schema_registry, next_capacity * sizeof(apxdb_schema_t*));
    if (!next) {
      return false;
    }
    g_schema_registry = next;
    g_schema_registry_capacity = next_capacity;
  }
  return true;
}

const apxdb_schema_t* apxdb_schema_register(const apxdb_schema_t* schema) {
  if (!schema || !schema->collection_name) {
    return NULL;
  }

  for (size_t index = 0; index < g_schema_registry_count; ++index) {
    if (strcmp(g_schema_registry[index]->collection_name, schema->collection_name) == 0) {
      return g_schema_registry[index];
    }
  }

  if (!grow_registry()) {
    return NULL;
  }

  g_schema_registry[g_schema_registry_count++] = schema;
  return schema;
}

const apxdb_schema_t* apxdb_schema_find(const char* collection_name) {
  if (!collection_name) {
    return NULL;
  }

  for (size_t index = 0; index < g_schema_registry_count; ++index) {
    if (strcmp(g_schema_registry[index]->collection_name, collection_name) == 0) {
      return g_schema_registry[index];
    }
  }
  return NULL;
}

void apxdb_schema_unregister_all(void) {
  free(g_schema_registry);
  g_schema_registry = NULL;
  g_schema_registry_count = 0;
  g_schema_registry_capacity = 0;
}

const apxdb_schema_field_t* apxdb_schema_find_field(const apxdb_schema_t* schema, const char* field_name) {
  if (!schema || !field_name) {
    return NULL;
  }

  for (size_t i = 0; i < schema->field_count; ++i) {
    if (strcmp(schema->fields[i].name, field_name) == 0) {
      return &schema->fields[i];
    }
  }
  return NULL;
}

const apxdb_schema_field_t* apxdb_schema_find_field_path(const apxdb_schema_t* schema, const char* field_path) {
  if (!schema || !field_path) {
    return NULL;
  }

  const apxdb_schema_t* current_schema = schema;
  const apxdb_schema_field_t* current_field = NULL;
  char buffer[256];
  size_t path_length = strlen(field_path);
  if (path_length >= sizeof(buffer)) {
    return NULL;
  }
  memcpy(buffer, field_path, path_length + 1);

  char* segment = strtok(buffer, ".");
  while (segment) {
    current_field = apxdb_schema_find_field(current_schema, segment);
    if (!current_field) {
      return NULL;
    }
    if (current_field->type == APXDB_TYPE_EMBEDDED) {
      current_schema = current_field->embedded_schema;
      if (!current_schema) {
        return NULL;
      }
    } else if (current_field->type == APXDB_TYPE_LIST && current_field->list_element_type == APXDB_TYPE_EMBEDDED) {
      current_schema = current_field->list_element_schema;
      if (!current_schema) {
        return NULL;
      }
    } else {
      if (strtok(NULL, ".") != NULL) {
        return NULL;
      }
      break;
    }
    segment = strtok(NULL, ".");
  }

  return current_field;
}

bool apxdb_schema_field_is_list(const apxdb_schema_field_t* field) {
  return field && field->type == APXDB_TYPE_LIST;
}

bool apxdb_schema_field_is_embedded(const apxdb_schema_field_t* field) {
  return field && field->type == APXDB_TYPE_EMBEDDED;
}

bool apxdb_schema_field_is_nullable(const apxdb_schema_field_t* field) {
  return field && field->nullable;
}

const apxdb_schema_t* apxdb_schema_field_list_element_schema(const apxdb_schema_field_t* field) {
  if (!field || field->type != APXDB_TYPE_LIST) {
    return NULL;
  }
  return field->list_element_schema;
}

apxdb_type_t apxdb_schema_field_list_element_type(const apxdb_schema_field_t* field) {
  if (!field || field->type != APXDB_TYPE_LIST) {
    return APXDB_TYPE_LIST;
  }
  return field->list_element_type;
}

const char* apxdb_type_name(apxdb_type_t type) {
  switch (type) {
    case APXDB_TYPE_BYTE:
      return "byte";
    case APXDB_TYPE_SHORT:
      return "short";
    case APXDB_TYPE_INT:
      return "int";
    case APXDB_TYPE_FLOAT:
      return "float";
    case APXDB_TYPE_DOUBLE:
      return "double";
    case APXDB_TYPE_BOOL:
      return "bool";
    case APXDB_TYPE_STRING:
      return "String";
    case APXDB_TYPE_DATETIME:
      return "DateTime";
    case APXDB_TYPE_EMBEDDED:
      return "embedded";
    case APXDB_TYPE_ENUM:
      return "enum";
    case APXDB_TYPE_LIST:
      return "list";
  }
  return "unknown";
}

bool apxdb_value_is_null(apxdb_type_t type, apxdb_value_t value) {
  switch (type) {
    case APXDB_TYPE_BYTE:
      return value.byte_value == 0xFFu;
    case APXDB_TYPE_SHORT:
      return value.short_value == INT32_MIN;
    case APXDB_TYPE_INT:
      return value.int_value == INT64_MIN;
    case APXDB_TYPE_FLOAT:
      return isnan(value.float_value);
    case APXDB_TYPE_DOUBLE:
      return isnan(value.double_value);
    case APXDB_TYPE_BOOL:
      return value.bool_value == APXDB_BOOL_NULL;
    case APXDB_TYPE_STRING:
      return value.string_value == NULL;
    case APXDB_TYPE_DATETIME:
      return value.datetime_value == INT64_MIN;
    case APXDB_TYPE_EMBEDDED:
    case APXDB_TYPE_ENUM:
    case APXDB_TYPE_LIST:
      return value.opaque == NULL;
    default:
      return true;
  }
}
