#include "apxdb.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const apxdb_schema_t** g_schema_registry = NULL;
static size_t g_schema_registry_count = 0;
static size_t g_schema_registry_capacity = 0;

static const apxdb_collection_schema_t** g_collection_schema_registry = NULL;
static size_t g_collection_schema_registry_count = 0;
static size_t g_collection_schema_registry_capacity = 0;

static apxdb_collection_layout_t** g_collection_layout_registry = NULL;
static size_t g_collection_layout_registry_count = 0;
static size_t g_collection_layout_registry_capacity = 0;

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

static bool grow_layout_registry(void) {
  if (g_collection_layout_registry_count >= g_collection_layout_registry_capacity) {
    size_t next_capacity = g_collection_layout_registry_capacity == 0 ? 8 : g_collection_layout_registry_capacity * 2;
    apxdb_collection_layout_t** next = (apxdb_collection_layout_t**)realloc(
        g_collection_layout_registry, next_capacity * sizeof(apxdb_collection_layout_t*));
    if (!next) {
      return false;
    }
    g_collection_layout_registry = next;
    g_collection_layout_registry_capacity = next_capacity;
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

static bool grow_collection_schema_registry(void) {
  if (g_collection_schema_registry_count >= g_collection_schema_registry_capacity) {
    size_t next_capacity = g_collection_schema_registry_capacity == 0 ? 8 : g_collection_schema_registry_capacity * 2;
    const apxdb_collection_schema_t** next = (const apxdb_collection_schema_t**)realloc(
        g_collection_schema_registry, next_capacity * sizeof(apxdb_collection_schema_t*));
    if (!next) {
      return false;
    }
    g_collection_schema_registry = next;
    g_collection_schema_registry_capacity = next_capacity;
  }
  return true;
}

static bool is_valid_collection_schema(const apxdb_collection_schema_t* schema) {
  if (!schema || schema->struct_size < sizeof(apxdb_collection_schema_t) || schema->version != 1) {
    return false;
  }
  if (!schema->name || schema->field_count == 0 || !schema->fields) {
    return false;
  }
  return true;
}

static uint32_t align_u32(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

static bool is_variable_field(const apxdb_field_schema_t* field) {
  return field->storage_kind == APXDB_STORAGE_VARIABLE;
}

static uint32_t field_storage_size(const apxdb_field_schema_t* field) {
  if (!field) {
    return 0;
  }
  return is_variable_field(field) ? sizeof(uint32_t) * 2 : field->type_size;
}

static apxdb_collection_layout_t* apxdb_build_collection_layout(const apxdb_collection_schema_t* schema) {
  if (!schema || schema->field_count == 0) {
    return NULL;
  }

  size_t nullable_bits = 0;
  size_t fixed_size = 0;
  size_t variable_slot_count = 0;
  for (uint32_t i = 0; i < schema->field_count; ++i) {
    const apxdb_field_schema_t* field = &schema->fields[i];
    if (field->flags & APXDB_FIELD_FLAG_NULLABLE) {
      nullable_bits += 1;
    }
    if (is_variable_field(field)) {
      variable_slot_count += 1;
      fixed_size += sizeof(uint32_t) * 2;
    } else {
      fixed_size += field_storage_size(field);
    }
  }

  uint32_t nullable_bitmap_size = (uint32_t)((nullable_bits + 7) / 8);
  uint32_t row_fixed_size = nullable_bitmap_size + (uint32_t)fixed_size;

  apxdb_collection_layout_t* layout = (apxdb_collection_layout_t*)calloc(1, sizeof(apxdb_collection_layout_t));
  if (!layout) {
    return NULL;
  }

  apxdb_collection_field_layout_t* fields = (apxdb_collection_field_layout_t*)calloc(schema->field_count, sizeof(apxdb_collection_field_layout_t));
  if (!fields) {
    free(layout);
    return NULL;
  }

  layout->struct_size = sizeof(apxdb_collection_layout_t);
  layout->version = APXDB_COLLECTION_LAYOUT_VERSION_1;
  layout->collection_id = schema->collection_id;
  layout->name = schema->name;
  layout->field_count = schema->field_count;
  layout->fields = fields;
  layout->nullable_bitmap_size = nullable_bitmap_size;
  layout->row_fixed_size = row_fixed_size;
  layout->variable_slot_count = (uint32_t)variable_slot_count;

  uint32_t field_offset = nullable_bitmap_size;
  uint32_t variable_slot = 0;
  for (uint32_t i = 0; i < schema->field_count; ++i) {
    const apxdb_field_schema_t* field = &schema->fields[i];
    apxdb_collection_field_layout_t* entry = &fields[i];
    entry->field_id = field->field_id;
    entry->name = field->name;
    entry->type = field->type;
    entry->flags = field->flags;
    entry->storage_kind = field->storage_kind;
    if (is_variable_field(field)) {
      entry->offset = field_offset;
      entry->size = sizeof(uint32_t) * 2;
      entry->variable_slot = variable_slot++;
      field_offset += entry->size;
    } else {
      entry->offset = field_offset;
      entry->size = field_storage_size(field);
      entry->variable_slot = UINT32_MAX;
      field_offset += entry->size;
    }
  }

  return layout;
}

static const apxdb_collection_layout_t* apxdb_register_layout_for_schema(const apxdb_collection_schema_t* schema) {
  if (!schema) {
    return NULL;
  }
  for (size_t index = 0; index < g_collection_layout_registry_count; ++index) {
    if (g_collection_layout_registry[index]->collection_id == schema->collection_id) {
      return g_collection_layout_registry[index];
    }
  }
  if (!grow_layout_registry()) {
    return NULL;
  }
  apxdb_collection_layout_t* layout = apxdb_build_collection_layout(schema);
  if (!layout) {
    return NULL;
  }
  g_collection_layout_registry[g_collection_layout_registry_count++] = layout;
  return layout;
}

const apxdb_collection_schema_t* apxdb_register_schema(const apxdb_collection_schema_t* schema) {
  if (!is_valid_collection_schema(schema)) {
    return NULL;
  }

  for (size_t index = 0; index < g_collection_schema_registry_count; ++index) {
    const apxdb_collection_schema_t* existing = g_collection_schema_registry[index];
    if (existing->collection_id == schema->collection_id ||
        strcmp(existing->name, schema->name) == 0) {
      return existing == schema ? existing : NULL;
    }
  }

  if (!grow_collection_schema_registry()) {
    return NULL;
  }

  g_collection_schema_registry[g_collection_schema_registry_count++] = schema;
  if (!apxdb_register_layout_for_schema(schema)) {
    g_collection_schema_registry_count -= 1;
    return NULL;
  }
  return schema;
}

const apxdb_collection_schema_t* apxdb_find_collection_schema_by_name(const char* name) {
  if (!name) {
    return NULL;
  }
  for (size_t index = 0; index < g_collection_schema_registry_count; ++index) {
    if (strcmp(g_collection_schema_registry[index]->name, name) == 0) {
      return g_collection_schema_registry[index];
    }
  }
  return NULL;
}

const apxdb_collection_schema_t* apxdb_find_collection_schema_by_id(uint32_t collection_id) {
  for (size_t index = 0; index < g_collection_schema_registry_count; ++index) {
    if (g_collection_schema_registry[index]->collection_id == collection_id) {
      return g_collection_schema_registry[index];
    }
  }
  return NULL;
}

const apxdb_collection_layout_t* apxdb_find_collection_layout_by_name(const char* name) {
  if (!name) {
    return NULL;
  }
  for (size_t index = 0; index < g_collection_layout_registry_count; ++index) {
    if (g_collection_layout_registry[index]->name && strcmp(g_collection_layout_registry[index]->name, name) == 0) {
      return g_collection_layout_registry[index];
    }
  }
  return NULL;
}

const apxdb_collection_layout_t* apxdb_find_collection_layout_by_id(uint32_t collection_id) {
  for (size_t index = 0; index < g_collection_layout_registry_count; ++index) {
    if (g_collection_layout_registry[index]->collection_id == collection_id) {
      return g_collection_layout_registry[index];
    }
  }
  return NULL;
}

void apxdb_unregister_all_layouts(void) {
  if (g_collection_layout_registry) {
    for (size_t index = 0; index < g_collection_layout_registry_count; ++index) {
      free((void*)g_collection_layout_registry[index]->fields);
      free(g_collection_layout_registry[index]);
    }
  }
  free(g_collection_layout_registry);
  g_collection_layout_registry = NULL;
  g_collection_layout_registry_count = 0;
  g_collection_layout_registry_capacity = 0;
}

void apxdb_unregister_all_schemas(void) {
  free(g_collection_schema_registry);
  g_collection_schema_registry = NULL;
  g_collection_schema_registry_count = 0;
  g_collection_schema_registry_capacity = 0;
  apxdb_unregister_all_layouts();
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
