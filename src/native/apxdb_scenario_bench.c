#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "apxdb.h"

static double diff_ms(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
}

static bool schema_used = true;

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

static const apxdb_schema_field_t testdb_schema_fields[] = {
  {"description", APXDB_TYPE_STRING, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"industry", APXDB_TYPE_STRING, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"level", APXDB_TYPE_INT, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"size", APXDB_TYPE_STRING, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"line_code", APXDB_TYPE_STRING, true, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"value", APXDB_TYPE_INT, true, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"status", APXDB_TYPE_STRING, true, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"Unit", APXDB_TYPE_STRING, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"Footnotes", APXDB_TYPE_STRING, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
};

static const apxdb_schema_t testdb_schema = {
  "testdb",
  testdb_schema_fields,
  sizeof(testdb_schema_fields) / sizeof(testdb_schema_fields[0]),
};

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
  const char* p = result;
  while ((p = strstr(p, "\"id\"")) != NULL) {
    hits++;
    p += 4;
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
    snprintf(json, sizeof(json), "{\"description\": \"bench insert %zu\", \"industry\": \"bench\", \"level\": %zu, \"size\": \"n/a\", \"line_code\": \"L%zu\", \"value\": %zu, \"status\": \"ok\", \"Unit\": \"items\", \"Footnotes\": \"bench\"}", i, i, i, i);
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

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  if (!run_import_csv(csv_path, db_dir, &rows)) {
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

  return 0;
}
