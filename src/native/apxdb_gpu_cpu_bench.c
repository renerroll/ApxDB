// macOS benchmark build:
// clang -x objective-c -fsanitize=address -O2 -I src/native -DAPXDB_ENABLE_DIAGNOSTICS \
//   src/native/apxdb_gpu_cpu_bench.c src/native/apxdb.c src/native/apxdb_json.c \
//   -framework Metal -framework Foundation -lpthread -o /tmp/apxdb_gpu_cpu_bench_asan

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "apxdb.h"

static char* make_temp_dir(void) {
  char template[] = "/tmp/apxdb_gpu_cpu_benchXXXXXX";
  char* dir = mkdtemp(template);
  if (!dir) {
    return NULL;
  }
  return strdup(dir);
}

static void free_temp_dir(const char* dir) {
  if (!dir) {
    return;
  }
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
  system(cmd);
}

static double diff_ms(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
}

static const int kBenchmarkDocCount = 20000;

static bool create_test_data(void) {
  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  if (!txn) {
    return false;
  }

  for (int i = 0; i < kBenchmarkDocCount; ++i) {
    char json[128];
    snprintf(json, sizeof(json), "{\"value\": %d, \"category\": \"cat%d\"}", i, i % 10);
    const char* id = apxdb_put_document("test_collection", json);
    if (!id) {
      apxdb_free_transaction(txn);
      return false;
    }
    apxdb_release_string(id);
  }

  int32_t result = apxdb_commit_write_txn(txn);
  apxdb_free_transaction(txn);
  return result == 0;
}

static const apxdb_schema_field_t benchmark_fields[] = {
  {"value", APXDB_TYPE_INT, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"category", APXDB_TYPE_STRING, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
};

static const apxdb_schema_t benchmark_schema = {
  "test_collection",
  benchmark_fields,
  2,
};

static bool run_query_benchmark(const char* mode, const char* query) {
  printf("--- %s benchmark: %s\n", mode, query);
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  const char* result = apxdb_find_documents("test_collection", query);
  clock_gettime(CLOCK_MONOTONIC, &end);
  if (!result) {
    fprintf(stderr, "%s: query failed\n", mode);
    return false;
  }

  apxdb_query_metrics_t metrics;
  bool metrics_ok = apxdb_last_query_metrics(&metrics) == 0;
  if (!metrics_ok) {
    memset(&metrics, 0, sizeof(metrics));
  }

  uint64_t hits = metrics.result_count;
  if (!metrics_ok) {
    const char* p = result;
    while ((p = strstr(p, "\"category\"")) != NULL) {
      hits++;
      p += 10;
    }
  }
  apxdb_release_string(result);

  printf("  wall_time_ms=%.3f gpu_status=%d query_path=%d result_hits=%" PRIu64 " bytes_uploaded=%" PRIu64 " bytes_reused=%" PRIu64 " cache_hits=%" PRIu64 " cache_misses=%" PRIu64 "\n",
         diff_ms(start, end),
         apxdb_gpu_status(),
         apxdb_last_query_path(),
         hits,
         metrics.bytes_uploaded,
         metrics.bytes_reused,
         metrics.cache_hits,
         metrics.cache_misses);
  return true;
}

static bool run_benchmark(const char* dir, bool gpu_enabled) {
  apxdb_set_gpu_enabled(gpu_enabled);
  int32_t open_result = apxdb_open(dir);
  if (open_result != APXDB_OK && open_result != APXDB_OK_GPU_FALLBACK) {
    fprintf(stderr, "failed to open DB (%d)\n", open_result);
    return false;
  }

  if (apxdb_register_collection(&benchmark_schema) != 0) {
    fprintf(stderr, "failed to register benchmark collection\n");
    apxdb_close();
    return false;
  }

  if (apxdb_load_all_collections(dir) != 0) {
    fprintf(stderr, "failed to load collections after open\n");
    apxdb_close();
    return false;
  }

  fprintf(stderr, "apxdb: open(%s) gpu_status=%d\n", dir, apxdb_gpu_status());

  const char* queries[] = {
    "{\"value\": {\"gte\": 0}}",
    "{\"value\": {\"lt\": 100}}",
    "{\"category\": \"cat5\"}",
    "{\"value\": {\"gte\": 100, \"lt\": 200}}"
  };
  const char* mode = gpu_enabled ? "GPU" : "CPU";

  for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
    if (!run_query_benchmark(mode, queries[i])) {
      apxdb_close();
      return false;
    }
  }

  bool close_ok = apxdb_close() == APXDB_OK;
  if (!close_ok) {
    fprintf(stderr, "%s: close failed\n", mode);
  }
  return close_ok;
}

static bool open_and_populate(const char* dir) {
  apxdb_set_gpu_enabled(false);
  int32_t result = apxdb_open(dir);
  if (result != APXDB_OK && result != APXDB_OK_GPU_FALLBACK) {
    fprintf(stderr, "open failed for populate (%d)\n", result);
    return false;
  }
  if (apxdb_register_collection(&benchmark_schema) != 0) {
    fprintf(stderr, "failed to register benchmark collection for populate\n");
    apxdb_close();
    return false;
  }
  if (!create_test_data()) {
    fprintf(stderr, "failed to create test data\n");
    apxdb_close();
    return false;
  }
  return apxdb_close() == APXDB_OK;
}

int main(void) {
  char* dir = make_temp_dir();
  if (!dir) {
    fprintf(stderr, "failed to create temp dir\n");
    return 1;
  }

  if (!open_and_populate(dir)) {
    free_temp_dir(dir);
    return 1;
  }

  if (!run_benchmark(dir, false)) {
    free_temp_dir(dir);
    return 1;
  }

  if (!run_benchmark(dir, true)) {
    free_temp_dir(dir);
    return 1;
  }

  free_temp_dir(dir);
  printf("Benchmark complete.\n");
  return 0;
}
