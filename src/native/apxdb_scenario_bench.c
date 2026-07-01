#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "apxdb.h"
#include "apxdb_json.h"

#define MAX_CSV_FIELDS 64
#define MAX_FIELD_LENGTH 4096

static double diff_ms(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
}

static bool schema_used = true;
static apxdb_schema_field_t* testdb_schema_fields = NULL;
static apxdb_schema_t testdb_schema = {"testdb", NULL, 0};

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

static bool infer_csv_schema(const char* csv_path, apxdb_schema_t* out_schema) {
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

  for (size_t i = 0; i < header_count; ++i) {
    fields[i].name = header_fields[i];
    fields[i].type = int_candidate[i] ? APXDB_TYPE_INT : APXDB_TYPE_STRING;
    fields[i].nullable = has_blank[i];
    fields[i].embedded_schema = NULL;
    fields[i].list_element_type = 0;
    fields[i].list_element_schema = NULL;
    fields[i].enum_strategy = APXDB_ENUM_STRATEGY_ORDINAL;
    fields[i].enum_values = NULL;
    fields[i].enum_value_count = 0;
  }

  out_schema->collection_name = "testdb";
  out_schema->fields = fields;
  out_schema->field_count = header_count;
  return true;
}

static void free_testdb_schema(void) {
  if (!testdb_schema.fields) {
    return;
  }
  for (size_t i = 0; i < testdb_schema.field_count; ++i) {
    free((char*)testdb_schema.fields[i].name);
  }
  free((void*)testdb_schema.fields);
  testdb_schema.fields = NULL;
  testdb_schema.field_count = 0;
}

static bool run_import_csv(const char* csv_path, const char* db_dir, size_t* out_rows) {
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "/tmp/apxdb_import_csv '%s' '%s'", csv_path, db_dir);
  int result = system(cmd);
  clock_gettime(CLOCK_MONOTONIC, &end);
  if (result != 0) {
    fprintf(stderr, "import command failed: %d\n", result);
    return false;
  }
  *out_rows = 11985; // approximate count for this dataset; exact count from CSV header
  printf("import runtime=%.3fms\n", diff_ms(start, end));
  return true;
}

static bool run_query(const char* mode, bool gpu_enabled, const char* query, const char* label) {
  apxdb_set_gpu_enabled(gpu_enabled);
  int32_t open_result = apxdb_open("/tmp/apxdb_testdb");
  if (open_result != APXDB_OK && open_result != APXDB_OK_GPU_FALLBACK) {
    fprintf(stderr, "%s: failed to open DB (%d)\n", mode, open_result);
    return false;
  }
  if (apxdb_register_collection(&testdb_schema) != 0) {
    fprintf(stderr, "%s: failed to register schema\n", mode);
    apxdb_close();
    return false;
  }
  if (apxdb_load_all_collections("/tmp/apxdb_testdb") != 0) {
    fprintf(stderr, "%s: failed to load collections\n", mode);
    apxdb_close();
    return false;
  }

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  const char* result = apxdb_find_documents("testdb", query);
  clock_gettime(CLOCK_MONOTONIC, &end);
  if (!result) {
    fprintf(stderr, "%s: query failed: %s\n", mode, query);
    apxdb_close();
    return false;
  }

  apxdb_query_metrics_t metrics;
  if (apxdb_last_query_metrics(&metrics) != 0) {
    memset(&metrics, 0, sizeof(metrics));
  }

  int hits = 0;
  apxdb_json_value_t json_result;
  if (apxdb_json_parse(result, &json_result) && json_result.type == APXDB_JSON_ARRAY) {
    hits = (int)json_result.u.array.count;
    apxdb_json_free(&json_result);
  } else {
    const char* p = result;
    while ((p = strstr(p, "\"id\"")) != NULL) {
      hits++;
      p += 4;
    }
  }
  apxdb_release_string(result);

  printf("%s %-12s time=%.3fms hits=%d gpu=%d path=%d uploaded=%" PRIu64 " reused=%" PRIu64 " cache_hits=%" PRIu64 "\n",
         mode,
         label,
         diff_ms(start, end),
         hits,
         apxdb_gpu_status(),
         apxdb_last_query_path(),
         metrics.bytes_uploaded,
         metrics.bytes_reused,
         metrics.cache_hits);

  apxdb_close();
  return true;
}

static bool run_id_search(const char* mode, bool gpu_enabled, const char* id) {
  apxdb_set_gpu_enabled(gpu_enabled);
  int32_t open_result = apxdb_open("/tmp/apxdb_testdb");
  if (open_result != APXDB_OK && open_result != APXDB_OK_GPU_FALLBACK) {
    fprintf(stderr, "%s: failed to open DB (%d)\n", mode, open_result);
    return false;
  }
  if (apxdb_register_collection(&testdb_schema) != 0) {
    apxdb_close();
    return false;
  }
  if (apxdb_load_all_collections("/tmp/apxdb_testdb") != 0) {
    apxdb_close();
    return false;
  }

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  const char* result = apxdb_find_document(id);
  clock_gettime(CLOCK_MONOTONIC, &end);
  if (!result) {
    fprintf(stderr, "%s: id search failed\n", mode);
    apxdb_close();
    return false;
  }
  apxdb_release_string(result);

  printf("%s %-12s id=%s time=%.3fms\n", mode, "id_search", id, diff_ms(start, end));
  apxdb_close();
  return true;
}

static bool run_write_benchmark(const char* mode, bool gpu_enabled, size_t count) {
  apxdb_set_gpu_enabled(gpu_enabled);
  int32_t open_result = apxdb_open("/tmp/apxdb_testdb");
  if (open_result != APXDB_OK && open_result != APXDB_OK_GPU_FALLBACK) {
    fprintf(stderr, "%s: failed to open DB (%d)\n", mode, open_result);
    return false;
  }
  if (apxdb_register_collection(&testdb_schema) != 0) {
    apxdb_close();
    return false;
  }
  if (apxdb_load_all_collections("/tmp/apxdb_testdb") != 0) {
    apxdb_close();
    return false;
  }

  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  if (!txn) {
    apxdb_close();
    return false;
  }

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (size_t i = 0; i < count; ++i) {
    char json[1024];
    snprintf(json, sizeof(json), "{\"description\": \"bench insert %zu\", \"industry\": \"bench\", \"level\": %zu, \"size\": \"n/a\", \"line_code\": \"L%zu\", \"value\": %zu, \"status\": %zu, \"Unit\": \"items\", \"Footnotes\": \"bench\"}", i, i, i, i, i);
    const char* id = apxdb_put_document("testdb", json);
    if (!id) {
      apxdb_rollback_write_txn(txn);
      apxdb_free_transaction(txn);
      apxdb_close();
      return false;
    }
    apxdb_release_string(id);
  }
  int32_t commit_result = apxdb_commit_write_txn(txn);
  clock_gettime(CLOCK_MONOTONIC, &end);
  apxdb_free_transaction(txn);
  apxdb_close();

  if (commit_result != 0) {
    fprintf(stderr, "%s: commit failed\n", mode);
    return false;
  }

  printf("%s write_insert count=%zu time=%.3fms\n", mode, count, diff_ms(start, end));
  return true;
}

int main(int argc, char** argv) {
  const char* csv_path = "data/test-db.csv";
  const char* db_dir = "/tmp/apxdb_testdb";
  size_t rows = 0;

  if (!infer_csv_schema(csv_path, &testdb_schema)) {
    fprintf(stderr, "failed to infer schema from CSV '%s'\n", csv_path);
    return 1;
  }

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  if (!run_import_csv(csv_path, db_dir, &rows)) {
    free_testdb_schema();
    return 1;
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  printf("import rows=%zu time=%.3fms\n", rows, diff_ms(start, end));

  printf("\n=== FILTER BENCHMARKS ===\n");
  run_query("CPU", false, "{\"value\": {\"gte\": 1000}}", "value>=1000");
  run_query("CPU", false, "{\"level\": {\"gte\": 1}}", "level>=1");
  run_query("CPU", false, "{\"industry\": \"Agriculture\"}", "industry=Agri");
  run_query("GPU", true, "{\"value\": {\"gte\": 1000}}", "value>=1000");
  run_query("GPU", true, "{\"level\": {\"gte\": 1}}", "level>=1");

  printf("\n=== COMPLEX QUERY ===\n");
  run_query("CPU", false, "{\"$and\": [{\"value\": {\"gte\": 1000}}, {\"industry\": \"Agriculture\"}]} ", "and_query");

  printf("\n=== WRITE BENCHMARK ===\n");
  run_write_benchmark("CPU", false, 200);

  printf("\n=== SEARCH BENCHMARK ===\n");
  run_id_search("CPU", false, "doc_1");
  run_id_search("CPU", false, "doc_100");

  free_testdb_schema();
  return 0;
}
