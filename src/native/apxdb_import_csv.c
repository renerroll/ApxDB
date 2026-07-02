#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apxdb.h"

#define MAX_CSV_FIELDS 64
#define MAX_FIELD_LENGTH 4096
#define MAX_JSON_LINE 8192

static char* trim_whitespace(char* value) {
  if (!value) {
    return NULL;
  }
  char* start = value;
  while (*start && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) {
    start++;
  }
  char* end = start + strlen(start);
  while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\r' || *(end - 1) == '\n')) {
    end--;
  }
  *end = '\0';
  return start;
}

static bool parse_csv_row(const char* line, char** fields, size_t max_fields, size_t* field_count) {
  char buffer[MAX_FIELD_LENGTH];
  size_t buffer_index = 0;
  size_t idx = 0;
  bool in_quotes = false;

  for (const char* p = line; ; ++p) {
    char c = *p;
    if (in_quotes) {
      if (c == '"') {
        if (*(p + 1) == '"') {
          if (buffer_index + 1 >= sizeof(buffer)) return false;
          buffer[buffer_index++] = '"';
          ++p;
        } else {
          in_quotes = false;
        }
      } else if (c == '\0') {
        break;
      } else {
        if (buffer_index + 1 >= sizeof(buffer)) return false;
        buffer[buffer_index++] = c;
      }
    } else {
      if (c == '"') {
        in_quotes = true;
      } else if (c == ',' || c == '\0' || c == '\n' || c == '\r') {
        buffer[buffer_index] = '\0';
        if (idx >= max_fields) {
          return false;
        }
        fields[idx] = strdup(trim_whitespace(buffer));
        if (!fields[idx]) {
          return false;
        }
        idx++;
        buffer_index = 0;
        if (c == '\0' || c == '\n') {
          break;
        }
      } else {
        if (buffer_index + 1 >= sizeof(buffer)) return false;
        buffer[buffer_index++] = c;
      }
    }
  }

  *field_count = idx;
  return true;
}

static bool json_escape(const char* src, char* dst, size_t dst_size) {
  size_t written = 0;
  while (*src) {
    char c = *src++;
    const char* esc = NULL;
    switch (c) {
      case '"': esc = "\\\""; break;
      case '\\': esc = "\\\\"; break;
      case '\b': esc = "\\b"; break;
      case '\f': esc = "\\f"; break;
      case '\n': esc = "\\n"; break;
      case '\r': esc = "\\r"; break;
      case '\t': esc = "\\t"; break;
      default: break;
    }
    if (esc) {
      size_t len = strlen(esc);
      if (written + len + 1 >= dst_size) return false;
      memcpy(dst + written, esc, len);
      written += len;
    } else {
      if (written + 1 + 1 >= dst_size) return false;
      dst[written++] = c;
    }
  }
  dst[written] = '\0';
  return true;
}

static bool is_integer_value(const char* value) {
  if (!value || *value == '\0') {
    return false;
  }
  const char* s = value;
  if (*s == '+' || *s == '-') {
    s++;
  }
  if (!*s) {
    return false;
  }
  while (*s) {
    if (*s < '0' || *s > '9') {
      return false;
    }
    s++;
  }
  return true;
}

static bool infer_csv_schema(const char* csv_path, size_t* out_field_count, char*** out_names,
                             apxdb_schema_field_t** out_fields, bool** out_nullable) {
  FILE* file = fopen(csv_path, "r");
  if (!file) {
    return false;
  }

  char* line = NULL;
  size_t len = 0;
  ssize_t read;
  if ((read = getline(&line, &len, file)) <= 0) {
    free(line);
    fclose(file);
    return false;
  }

  char* header_fields[MAX_CSV_FIELDS];
  size_t header_count = 0;
  if (!parse_csv_row(line, header_fields, MAX_CSV_FIELDS, &header_count)) {
    free(line);
    fclose(file);
    return false;
  }

  bool int_candidate[MAX_CSV_FIELDS] = {0};
  bool has_blank[MAX_CSV_FIELDS] = {0};
  for (size_t i = 0; i < header_count; ++i) {
    int_candidate[i] = true;
  }

  while ((read = getline(&line, &len, file)) != -1) {
    char* cols[MAX_CSV_FIELDS] = {0};
    size_t cols_count = 0;
    if (!parse_csv_row(line, cols, MAX_CSV_FIELDS, &cols_count)) {
      break;
    }
    for (size_t i = 0; i < header_count; ++i) {
      const char* value = i < cols_count ? cols[i] : "";
      if (!value || *value == '\0') {
        has_blank[i] = true;
        continue;
      }
      if (!is_integer_value(value)) {
        int_candidate[i] = false;
      }
    }
    for (size_t i = 0; i < cols_count; ++i) {
      free(cols[i]);
    }
  }

  free(line);
  fclose(file);

  apxdb_schema_field_t* fields = calloc(header_count, sizeof(apxdb_schema_field_t));
  if (!fields) {
    return false;
  }

  char** names = calloc(header_count, sizeof(char*));
  if (!names) {
    free(fields);
    return false;
  }

  bool* nullable = calloc(header_count, sizeof(bool));
  if (!nullable) {
    free(fields);
    free(names);
    return false;
  }

  for (size_t i = 0; i < header_count; ++i) {
    names[i] = strdup(header_fields[i]);
    if (!names[i]) {
      return false;
    }
    nullable[i] = has_blank[i];
    apxdb_schema_field_t field = {
      .name = names[i],
      .type = int_candidate[i] ? APXDB_TYPE_INT : APXDB_TYPE_STRING,
      .nullable = nullable[i],
      .embedded_schema = NULL,
      .list_element_type = 0,
      .list_element_schema = NULL,
      .enum_strategy = APXDB_ENUM_STRATEGY_ORDINAL,
      .enum_values = NULL,
      .enum_value_count = 0,
    };
    fields[i] = field;
  }

  *out_field_count = header_count;
  *out_names = names;
  *out_fields = fields;
  *out_nullable = nullable;
  return true;
}

static bool import_csv_to_apxdb(const char* csv_path, const char* db_dir) {
  size_t field_count = 0;
  char** names = NULL;
  apxdb_schema_field_t* fields = NULL;
  bool* nullable = NULL;
  if (!infer_csv_schema(csv_path, &field_count, &names, &fields, &nullable)) {
    fprintf(stderr, "failed to infer CSV schema from '%s'\n", csv_path);
    return false;
  }

  apxdb_schema_t* schema = calloc(1, sizeof(apxdb_schema_t));
  if (!schema) {
    fprintf(stderr, "failed to allocate CSV schema\n");
    return false;
  }
  schema->collection_name = "testdb";
  schema->fields = fields;
  schema->field_count = field_count;

  apxdb_set_gpu_enabled(false);
  int32_t open_result = apxdb_open(db_dir);
  if (open_result != APXDB_OK && open_result != APXDB_OK_GPU_FALLBACK) {
    fprintf(stderr, "failed to open DB directory '%s': %d\n", db_dir, open_result);
    return false;
  }

  if (apxdb_register_collection(schema) != 0) {
    fprintf(stderr, "failed to register collection '%s'\n", schema->collection_name);
    apxdb_close();
    return false;
  }

  FILE* file = fopen(csv_path, "r");
  if (!file) {
    fprintf(stderr, "failed to open CSV file '%s': %s\n", csv_path, strerror(errno));
    apxdb_close();
    return false;
  }

  char* line = NULL;
  size_t len = 0;
  if (getline(&line, &len, file) <= 0) {
    fprintf(stderr, "CSV file '%s' is empty\n", csv_path);
    free(line);
    fclose(file);
    apxdb_close();
    return false;
  }

  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  if (!txn) {
    fprintf(stderr, "failed to begin write transaction\n");
    free(line);
    fclose(file);
    apxdb_close();
    return false;
  }

  size_t inserted = 0;
  char* cols[MAX_CSV_FIELDS] = {0};
  while (getline(&line, &len, file) != -1) {
    size_t cols_count = 0;
    if (!parse_csv_row(line, cols, field_count, &cols_count)) {
      fprintf(stderr, "warning: skipping invalid CSV line\n");
      continue;
    }
    char json[MAX_JSON_LINE];
    size_t json_len = 0;
    json[json_len++] = '{';
    for (size_t i = 0; i < field_count; ++i) {
      const char* value = i < cols_count ? cols[i] : "";
      if (i > 0) {
        if (json_len + 2 >= sizeof(json)) {
          json_len = 0;
          break;
        }
        json[json_len++] = ',';
      }
      char escaped[MAX_FIELD_LENGTH * 2];
      if (fields[i].type == APXDB_TYPE_INT && is_integer_value(value)) {
        int n = snprintf(json + json_len, sizeof(json) - json_len, "\"%s\": %s", fields[i].name, value);
        if (n < 0 || (size_t)n >= sizeof(json) - json_len) {
          json_len = 0;
          break;
        }
        json_len += (size_t)n;
      } else if (fields[i].type == APXDB_TYPE_INT && (!value || *value == '\0')) {
        int n = snprintf(json + json_len, sizeof(json) - json_len, "\"%s\": null", fields[i].name);
        if (n < 0 || (size_t)n >= sizeof(json) - json_len) {
          json_len = 0;
          break;
        }
        json_len += (size_t)n;
      } else {
        if (!json_escape(value ? value : "", escaped, sizeof(escaped))) {
          json_len = 0;
          break;
        }
        int n = snprintf(json + json_len, sizeof(json) - json_len, "\"%s\": \"%s\"", fields[i].name, escaped);
        if (n < 0 || (size_t)n >= sizeof(json) - json_len) {
          json_len = 0;
          break;
        }
        json_len += (size_t)n;
      }
    }
    if (json_len == 0) {
      for (size_t i = 0; i < cols_count; ++i) {
        free(cols[i]);
        cols[i] = NULL;
      }
      continue;
    }
    if (json_len + 2 >= sizeof(json)) {
      continue;
    }
    json[json_len++] = '}';
    json[json_len] = '\0';

    const char* id = apxdb_put_document(schema->collection_name, json);
    if (!id) {
      fprintf(stderr, "failed to insert document: %s\n", json);
      apxdb_rollback_write_txn(txn);
      apxdb_free_transaction(txn);
      free(line);
      for (size_t i = 0; i < cols_count; ++i) {
        free(cols[i]);
      }
      fclose(file);
      apxdb_close();
      return false;
    }
    apxdb_release_string(id);
    inserted++;
    for (size_t i = 0; i < cols_count; ++i) {
      free(cols[i]);
      cols[i] = NULL;
    }
  }

  free(line);
  fclose(file);

  int32_t commit_result = apxdb_commit_write_txn(txn);
  apxdb_free_transaction(txn);
  if (commit_result != 0) {
    fprintf(stderr, "failed to commit transaction\n");
    apxdb_close();
    return false;
  }

  printf("imported %zu rows into '%s'\n", inserted, db_dir);
  bool close_ok = apxdb_close() == APXDB_OK;
  if (!close_ok) {
    fprintf(stderr, "failed to close DB after import\n");
  }
  return close_ok;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <csv-path> <db-dir>\n", argv[0]);
    return 1;
  }
  const char* csv_path = argv[1];
  const char* db_dir = argv[2];
  return import_csv_to_apxdb(csv_path, db_dir) ? 0 : 1;
}
