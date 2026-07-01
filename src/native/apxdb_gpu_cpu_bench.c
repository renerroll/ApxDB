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

extern bool run_gpu_query_int(const int32_t* values, const uint32_t* valid_mask, size_t count, int op, int32_t threshold, uint8_t* out_mask);
extern bool run_gpu_query_int_count(const int32_t* values, const uint32_t* valid_mask, size_t count, int op, int32_t threshold, uint32_t* out_count);

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

typedef struct {
  const char* query;
  double wall_time_ms;
  int gpu_status;
  int query_path;
  uint64_t result_hits;
  uint64_t bytes_uploaded;
  uint64_t bytes_reused;
  uint64_t cache_hits;
  uint64_t cache_misses;
} benchmark_result_t;

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

static bool run_query_benchmark(const char* mode, const char* query, const char* label, benchmark_result_t* out_result) {
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

  out_result->query = label;
  out_result->wall_time_ms = diff_ms(start, end);
  out_result->gpu_status = apxdb_gpu_status();
  out_result->query_path = apxdb_last_query_path();
  out_result->result_hits = metrics.result_count;
  out_result->bytes_uploaded = metrics.bytes_uploaded;
  out_result->bytes_reused = metrics.bytes_reused;
  out_result->cache_hits = metrics.cache_hits;
  out_result->cache_misses = metrics.cache_misses;

  printf("  wall_time_ms=%.3f gpu_status=%d query_path=%d result_hits=%" PRIu64 " bytes_uploaded=%" PRIu64 " bytes_reused=%" PRIu64 " cache_hits=%" PRIu64 " cache_misses=%" PRIu64 "\n",
         out_result->wall_time_ms,
         out_result->gpu_status,
         out_result->query_path,
         out_result->result_hits,
         out_result->bytes_uploaded,
         out_result->bytes_reused,
         out_result->cache_hits,
         out_result->cache_misses);
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
  const char* labels[] = {"gte0", "lt100", "cat5", "100-200"};
  const char* mode = gpu_enabled ? "GPU" : "CPU";
  benchmark_result_t results[sizeof(queries) / sizeof(queries[0])];
  double total_time_ms = 0;

  for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
    if (!run_query_benchmark(mode, queries[i], labels[i], &results[i])) {
      apxdb_close();
      return false;
    }
    total_time_ms += results[i].wall_time_ms;
  }

  printf("\n=== %s summary ===\n", mode);
  printf("%-10s %10s %10s %10s %10s\n", "label", "time_ms", "hits", "path", "gpu");
  for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
    printf("%-10s %10.3f %10" PRIu64 " %10d %10d\n",
           results[i].query,
           results[i].wall_time_ms,
           results[i].result_hits,
           results[i].query_path,
           results[i].gpu_status);
  }
  printf("\n%s total_time_ms = %.3f\n\n", mode, total_time_ms);

  bool close_ok = apxdb_close() == APXDB_OK;
  if (!close_ok) {
    fprintf(stderr, "%s: close failed\n", mode);
  }
  return close_ok;
}

static bool run_raw_gpu_cpu_benchmark(void) {
  const size_t count = 1 * 1024 * 1024;
  const int32_t threshold = 5000;
  const int runs = 7;
  int32_t* values = (int32_t*)malloc(count * sizeof(int32_t));
  uint32_t* valid_mask = (uint32_t*)malloc(count * sizeof(uint32_t));
  if (!values || !valid_mask) {
    fprintf(stderr, "failed to allocate raw benchmark buffers\n");
    free(values);
    free(valid_mask);
    return false;
  }

  for (size_t i = 0; i < count; ++i) {
    values[i] = (int32_t)(i % 10000);
    valid_mask[i] = 1;
  }

  printf("\n=== raw CPU/GPU compute benchmark ===\n");
  apxdb_set_gpu_enabled(true);
  int32_t gpu_init = apxdb_initialize();
  if (gpu_init != APXDB_OK && gpu_init != APXDB_OK_GPU_FALLBACK) {
    fprintf(stderr, "raw benchmark: failed to initialize apxdb GPU backend (%d)\n", gpu_init);
    free(values);
    free(valid_mask);
    return false;
  }

  double cpu_time = 0.0;
  int64_t cpu_hits = 0;
  for (int r = 0; r < runs; ++r) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int64_t hits = 0;
    for (size_t i = 0; i < count; ++i) {
      if (valid_mask[i] && values[i] >= threshold) {
        hits++;
      }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (r == 0) {
      cpu_hits = hits;
    }
    cpu_time += diff_ms(start, end);
  }
  cpu_time /= runs;

  double gpu_time = 0.0;
  uint32_t gpu_hits = 0;
  for (int r = 0; r < runs; ++r) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint32_t count_result = 0;
    if (!run_gpu_query_int_count(values, valid_mask, count, 2, threshold, &count_result)) {
      fprintf(stderr, "run_gpu_query_int_count failed\n");
      free(values);
      free(valid_mask);
      return false;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (r == 0) {
      gpu_hits = count_result;
    }
    gpu_time += diff_ms(start, end);
  }
  gpu_time /= runs;

  printf("raw count=%zu threshold=%d runs=%d\n", count, threshold, runs);
  printf("CPU time avg=%.3fms hits=%lld\n", cpu_time, (long long)cpu_hits);
  printf("GPU time avg=%.3fms hits=%u\n", gpu_time, gpu_hits);

  apxdb_shutdown();
  free(values);
  free(valid_mask);
  return true;
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

  if (!run_raw_gpu_cpu_benchmark()) {
    free_temp_dir(dir);
    return 1;
  }

  free_temp_dir(dir);
  printf("Benchmark complete.\n");
  return 0;
}
