#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "apxdb.h"

#define ASSERT(cond, msg) do { if (!(cond)) { fprintf(stderr, "ASSERT FAILED: %s\n", msg); exit(1); } } while (0)

static char* make_temp_dir(void) {
  char template[] = "/tmp/apxdb_testXXXXXX";
  char* result = mkdtemp(template);
  if (!result) {
    return NULL;
  }
  return strdup(result);
}

static char* join_path(const char* dir, const char* file_name) {
  size_t dir_len = strlen(dir);
  size_t file_len = strlen(file_name);
  size_t total = dir_len + 1 + file_len + 1;
  char* path = (char*)malloc(total);
  if (!path) {
    return NULL;
  }
  snprintf(path, total, "%s/%s", dir, file_name);
  return path;
}

static int count_substring(const char* text, const char* needle) {
  int count = 0;
  const char* p = text;
  while ((p = strstr(p, needle)) != NULL) {
    ++count;
    p += strlen(needle);
  }
  return count;
}

static bool contains_value(const char* json, const char* key) {
  return strstr(json, key) != NULL;
}

static bool open_db(const char* directory) {
  int32_t result = apxdb_open(directory);
  if (result != APXDB_OK && result != APXDB_OK_GPU_FALLBACK) {
    fprintf(stderr, "open_db failed code=%d\n", result);
    return false;
  }
  return true;
}

static const apxdb_schema_field_t test_fields[] = {
  {"value", APXDB_TYPE_INT, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  {"name", APXDB_TYPE_STRING, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
};

static const apxdb_schema_t test_schema = {
  "test_collection",
  test_fields,
  2,
};

static void register_test_collection(void) {
  int32_t result = apxdb_register_collection(&test_schema);
  ASSERT(result == 0, "register_test_collection failed");
}

static void add_index(bool multi_entry) {
  apxdb_index_definition_t index = {
    .field_name = "value",
    .index_type = APXDB_INDEX_TYPE_VALUE,
    .composite = false,
    .multi_entry = multi_entry,
  };
  int32_t result = apxdb_add_index("test_collection", &index);
  ASSERT(result == 0, "apxdb_add_index failed");
}

static void populate_documents(void) {
  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  ASSERT(txn != NULL, "begin transaction failed");

  for (int i = 0; i < 10; ++i) {
    char json[128];
    snprintf(json, sizeof(json), "{\"value\": %d, \"name\": \"doc%d\"}", i, i);
    const char* id = apxdb_put_document("test_collection", json);
    ASSERT(id != NULL, "put_document failed");
    apxdb_release_string(id);
  }

  int32_t result = apxdb_commit_write_txn(txn);
  ASSERT(result == 0, "commit transaction failed");
  apxdb_free_transaction(txn);
}

static void assert_range_query(int expected_count) {
  const char* query = "{\"value\": {\"gt\": 5}}";
  const char* result = apxdb_find_documents("test_collection", query);
  ASSERT(result != NULL, "find_documents returned NULL");
  int count = count_substring(result, "\"name\"");
  apxdb_release_string(result);
  ASSERT(count == expected_count, "unexpected range query count");
}

static void corrupt_idx_header(const char* idx_path) {
  FILE* file = fopen(idx_path, "r+b");
  ASSERT(file != NULL, "failed to open idx for header corruption");
  ASSERT(fputc('X', file) != EOF, "failed to write corrupt header");
  fclose(file);
}

static void corrupt_idx_doc_count(const char* idx_path) {
  FILE* file = fopen(idx_path, "r+b");
  ASSERT(file != NULL, "failed to open idx for doc_count corruption");

  // Skip magic (8 bytes), version, and schema signature.
  ASSERT(fseek(file, 8 + 4 + 8, SEEK_SET) == 0, "seek failed");

  uint32_t name_length = 0;
  ASSERT(fread(&name_length, sizeof(name_length), 1, file) == 1, "read name length failed");
  fseek(file, name_length, SEEK_CUR);

  uint32_t index_count = 0;
  ASSERT(fread(&index_count, sizeof(index_count), 1, file) == 1, "read index count failed");
  ASSERT(index_count > 0, "index count must be > 0");

  uint32_t field_length = 0;
  ASSERT(fread(&field_length, sizeof(field_length), 1, file) == 1, "read field length failed");
  fseek(file, field_length + 4 + 4, SEEK_CUR);

  uint32_t entry_count = 0;
  ASSERT(fread(&entry_count, sizeof(entry_count), 1, file) == 1, "read entry count failed");
  ASSERT(entry_count > 0, "entry count must be > 0");

  uint32_t key_length = 0;
  ASSERT(fread(&key_length, sizeof(key_length), 1, file) == 1, "read key length failed");
  fseek(file, key_length, SEEK_CUR);

  long doc_count_offset = ftell(file);
  ASSERT(doc_count_offset >= 0, "ftell failed");

  uint32_t bad_doc_count = 0xFFFFFFFFu;
  ASSERT(fseek(file, doc_count_offset, SEEK_SET) == 0, "seek to doc_count failed");
  ASSERT(fwrite(&bad_doc_count, sizeof(bad_doc_count), 1, file) == 1, "write bad doc_count failed");

  fclose(file);
}

static void cleanup_test_files(const char* dir) {
  if (!dir) {
    return;
  }
  char* data_path = join_path(dir, "test_collection.apxdb");
  if (data_path) {
    unlink(data_path);
    free(data_path);
  }
  char* idx_path = join_path(dir, "test_collection.idx");
  if (idx_path) {
    unlink(idx_path);
    free(idx_path);
  }
}

static void run_test_save_reopen(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: save_reopen\n");
  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed");
  populate_documents();
  ASSERT(apxdb_close() == APXDB_OK, "close failed");

  char* data_path = join_path(dir, "test_collection.apxdb");
  ASSERT(data_path != NULL, "join_path failed");
  struct stat st;
  ASSERT(stat(data_path, &st) == 0, "data file missing after save");
  free(data_path);

  char* idx_path = join_path(dir, "test_collection.idx");
  ASSERT(idx_path != NULL, "join_path failed");
  ASSERT(stat(idx_path, &st) == 0, "idx file missing after save");
  free(idx_path);

  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "reopen_db failed");
  assert_range_query(4);
  ASSERT(apxdb_close() == APXDB_OK, "close failed after reopen");
}

static void run_test_corrupt_header_rebuild(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: corrupt_header_rebuild\n");
  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed");
  populate_documents();
  ASSERT(apxdb_close() == APXDB_OK, "close failed");

  char* idx_path = join_path(dir, "test_collection.idx");
  ASSERT(idx_path != NULL, "join_path failed");
  corrupt_idx_header(idx_path);

  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed after corrupt header");
  assert_range_query(4);
  ASSERT(apxdb_close() == APXDB_OK, "close failed after corrupt header reopen");
  free(idx_path);
}

static void run_test_declaration_mismatch(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: declaration_mismatch\n");
  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed");
  populate_documents();
  ASSERT(apxdb_close() == APXDB_OK, "close failed");

  register_test_collection();
  add_index(true);
  ASSERT(open_db(dir), "open_db failed after declaration mismatch");
  assert_range_query(4);
  ASSERT(apxdb_close() == APXDB_OK, "close failed after declaration mismatch reopen");
}

static void run_test_missing_idx_rebuild(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: missing_idx_rebuild\n");
  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed");
  populate_documents();
  ASSERT(apxdb_close() == APXDB_OK, "close failed");

  char* idx_path = join_path(dir, "test_collection.idx");
  ASSERT(idx_path != NULL, "join_path failed");
  ASSERT(unlink(idx_path) == 0, "failed to remove idx file");
  free(idx_path);

  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed after missing idx");
  assert_range_query(4);
  char* recreated_idx_path = join_path(dir, "test_collection.idx");
  ASSERT(recreated_idx_path != NULL, "join_path failed");
  struct stat st;
  ASSERT(stat(recreated_idx_path, &st) == 0, "idx file was not recreated");
  free(recreated_idx_path);
  ASSERT(apxdb_close() == APXDB_OK, "close failed after missing idx reopen");
}

static void run_test_stale_doc_ids_reject(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: stale_doc_ids_reject\n");
  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed");
  populate_documents();
  ASSERT(apxdb_close() == APXDB_OK, "close failed");

  char* idx_path = join_path(dir, "test_collection.idx");
  ASSERT(idx_path != NULL, "join_path failed");
  corrupt_idx_doc_count(idx_path);

  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed after stale doc_ids corruption");
  assert_range_query(4);
  ASSERT(apxdb_close() == APXDB_OK, "close failed after stale doc_ids reopen");
  free(idx_path);
}

static void assert_query_result_count(const char* collection_name, const char* query, int expected_count) {
  const char* result_json = apxdb_find_documents(collection_name, query);
  ASSERT(result_json != NULL, "find_documents returned NULL");
  int count = count_substring(result_json, "\"name\"");
  if (count != expected_count) {
    fprintf(stderr, "QUERY FAILED: %s -> count=%d expected=%d result=%s\n", query, count, expected_count, result_json);
  }
  apxdb_release_string(result_json);
  ASSERT(count == expected_count, query);
}

static void run_test_gpu_numeric_query_path(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: gpu_numeric_query_path\n");

  register_test_collection();
  ASSERT(open_db(dir), "open_db failed");

  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  ASSERT(txn != NULL, "begin transaction failed");

  const int doc_count = 200;
  for (int i = 0; i < doc_count; ++i) {
    char json[128];
    snprintf(json, sizeof(json), "{\"value\": %d, \"name\": \"doc%d\"}", i, i);
    const char* id = apxdb_put_document("test_collection", json);
    ASSERT(id != NULL, "put_document failed");
    apxdb_release_string(id);
  }

  int32_t result = apxdb_commit_write_txn(txn);
  ASSERT(result == 0, "commit transaction failed");
  apxdb_free_transaction(txn);

  const char* query = "{\"value\": {\"gte\": 0}}";
  const char* result_json = apxdb_find_documents("test_collection", query);
  ASSERT(result_json != NULL, "first find_documents returned NULL");
  int count = count_substring(result_json, "\"name\"");
  apxdb_release_string(result_json);
  ASSERT(count == doc_count, "unexpected first query result count");

  int32_t gpu_status = apxdb_gpu_status();
  int32_t query_path = apxdb_last_query_path();
  if (gpu_status == APXDB_GPU_METAL_ACTIVE || gpu_status == APXDB_GPU_VULKAN_ACTIVE) {
    ASSERT(query_path == APXDB_QUERY_GPU_USED, "expected first query to use GPU path");
  }

  const char* second_json = apxdb_find_documents("test_collection", query);
  ASSERT(second_json != NULL, "second find_documents returned NULL");
  int second_count = count_substring(second_json, "\"name\"");
  apxdb_release_string(second_json);
  ASSERT(second_count == doc_count, "unexpected second query result count");

  apxdb_query_metrics_t metrics;
  ASSERT(apxdb_last_query_metrics(&metrics) == 0, "failed to read last query metrics");
  if (gpu_status == APXDB_GPU_METAL_ACTIVE || gpu_status == APXDB_GPU_VULKAN_ACTIVE) {
    ASSERT(metrics.cache_hits > 0, "expected GPU cache hit on second query");
  }

  ASSERT(apxdb_close() == APXDB_OK, "close failed");
}

static void run_test_numeric_exact_and_range_queries(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: numeric_exact_and_range_queries\n");

  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed");

  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  ASSERT(txn != NULL, "begin transaction failed");

  const int values[] = {0, 1, 2, 5, 5, 5, 7, 8, 9, 10};
  const int doc_count = sizeof(values) / sizeof(values[0]);
  for (int i = 0; i < doc_count; ++i) {
    char json[128];
    snprintf(json, sizeof(json), "{\"value\": %d, \"name\": \"doc%d\"}", values[i], i);
    const char* id = apxdb_put_document("test_collection", json);
    ASSERT(id != NULL, "put_document failed");
    apxdb_release_string(id);
  }

  int32_t result = apxdb_commit_write_txn(txn);
  ASSERT(result == 0, "commit transaction failed");
  apxdb_free_transaction(txn);

  const char* query1 = "{\"value\": 5}";
  const char* result1 = apxdb_find_documents("test_collection", query1);
  ASSERT(result1 != NULL, "find_documents returned NULL");
  ASSERT(apxdb_last_query_path() == APXDB_QUERY_INDEX_EXACT, "expected exact index path");
  apxdb_release_string(result1);
  assert_query_result_count("test_collection", query1, 3);

  const char* query2 = "{\"value\": {\"gte\": 9}}";
  const char* result2 = apxdb_find_documents("test_collection", query2);
  ASSERT(result2 != NULL, "find_documents returned NULL");
  ASSERT(apxdb_last_query_path() == APXDB_QUERY_INDEX_RANGE, "expected range index path");
  apxdb_release_string(result2);
  assert_query_result_count("test_collection", query2, 2);

  assert_query_result_count("test_collection", "{\"value\": {\"lt\": 3}}", 3);
  assert_query_result_count("test_collection", "{\"value\": {\"gt\": 5}}", 4);
  assert_query_result_count("test_collection", "{\"value\": {\"lte\": 5}}", 6);

  const char* query3 = "{\"name\": \"doc0\"}";
  const char* result3 = apxdb_find_documents("test_collection", query3);
  ASSERT(result3 != NULL, "find_documents returned NULL");
  ASSERT(apxdb_last_query_plan() == APXDB_QUERY_PLAN_SCAN, "expected scan plan for non-indexed field");
  apxdb_release_string(result3);
  assert_query_result_count("test_collection", query3, 1);

  ASSERT(apxdb_close() == APXDB_OK, "close failed");
}

static void run_test_multi_predicate_planner(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: multi_predicate_planner\n");

  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed");

  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  ASSERT(txn != NULL, "begin transaction failed");

  for (int i = 0; i < 10; ++i) {
    char json[128];
    snprintf(json, sizeof(json), "{\"value\": %d, \"name\": \"doc%d\"}", i, i);
    const char* id = apxdb_put_document("test_collection", json);
    ASSERT(id != NULL, "put_document failed");
    apxdb_release_string(id);
  }

  ASSERT(apxdb_commit_write_txn(txn) == 0, "commit transaction failed");
  apxdb_free_transaction(txn);

  const char* exact_query = "{\"value\": 5, \"name\": \"doc5\"}";
  const char* exact_result = apxdb_find_documents("test_collection", exact_query);
  ASSERT(exact_result != NULL, "find_documents returned NULL");
  ASSERT(apxdb_last_query_plan() == APXDB_QUERY_PLAN_INDEX_EXACT, "expected exact index plan for multi-predicate query");
  apxdb_release_string(exact_result);
  assert_query_result_count("test_collection", exact_query, 1);

  const char* range_query = "{\"value\": {\"gte\": 9}, \"name\": \"doc9\"}";
  const char* range_result = apxdb_find_documents("test_collection", range_query);
  ASSERT(range_result != NULL, "find_documents returned NULL");
  ASSERT(apxdb_last_query_plan() == APXDB_QUERY_PLAN_INDEX_RANGE, "expected range index plan for multi-predicate query");
  apxdb_release_string(range_result);
  assert_query_result_count("test_collection", range_query, 1);

  ASSERT(apxdb_close() == APXDB_OK, "close failed");
}

static void run_test_multi_predicate_exact_beats_wide_range(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: multi_predicate_exact_beats_wide_range\n");

  register_test_collection();
  add_index(false);
  apxdb_index_definition_t name_index = {
    .field_name = "name",
    .index_type = APXDB_INDEX_TYPE_VALUE,
    .composite = false,
    .multi_entry = false,
  };
  int32_t result = apxdb_add_index("test_collection", &name_index);
  ASSERT(result == 0, "apxdb_add_index failed for name field");
  ASSERT(open_db(dir), "open_db failed");

  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  ASSERT(txn != NULL, "begin transaction failed");

  for (int i = 0; i < 10; ++i) {
    char json[128];
    snprintf(json, sizeof(json), "{\"value\": %d, \"name\": \"doc%d\"}", i, i);
    const char* id = apxdb_put_document("test_collection", json);
    ASSERT(id != NULL, "put_document failed");
    apxdb_release_string(id);
  }

  ASSERT(apxdb_commit_write_txn(txn) == 0, "commit transaction failed");
  apxdb_free_transaction(txn);

  const char* query = "{\"value\": {\"gte\": 5}, \"name\": \"doc7\"}";
  const char* query_result = apxdb_find_documents("test_collection", query);
  ASSERT(query_result != NULL, "find_documents returned NULL");
  ASSERT(apxdb_last_query_plan() == APXDB_QUERY_PLAN_INDEX_EXACT, "expected exact plan to beat wide range");
  apxdb_release_string(query_result);
  assert_query_result_count("test_collection", query, 1);

  ASSERT(apxdb_close() == APXDB_OK, "close failed");
}

static void run_test_multi_predicate_scan_fallback(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: multi_predicate_scan_fallback\n");

  register_test_collection();
  ASSERT(open_db(dir), "open_db failed");

  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  ASSERT(txn != NULL, "begin transaction failed");

  for (int i = 0; i < 10; ++i) {
    char json[128];
    snprintf(json, sizeof(json), "{\"value\": %d, \"name\": \"doc%d\"}", i, i);
    const char* id = apxdb_put_document("test_collection", json);
    ASSERT(id != NULL, "put_document failed");
    apxdb_release_string(id);
  }

  ASSERT(apxdb_commit_write_txn(txn) == 0, "commit transaction failed");
  apxdb_free_transaction(txn);

  const char* query = "{\"value\": {\"gte\": 5}, \"name\": \"doc7\"}";
  const char* result = apxdb_find_documents("test_collection", query);
  ASSERT(result != NULL, "find_documents returned NULL");
  ASSERT(apxdb_last_query_plan() == APXDB_QUERY_PLAN_SCAN, "expected scan plan when wide range is not worth index route");
  apxdb_release_string(result);
  assert_query_result_count("test_collection", query, 1);

  ASSERT(apxdb_close() == APXDB_OK, "close failed");
}

static void run_test_numeric_index_persistence(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: numeric_index_persistence\n");

  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "open_db failed");

  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  ASSERT(txn != NULL, "begin transaction failed");

  const int values[] = {1, 2, 5, 5, 10};
  const int doc_count = sizeof(values) / sizeof(values[0]);
  for (int i = 0; i < doc_count; ++i) {
    char json[128];
    snprintf(json, sizeof(json), "{\"value\": %d, \"name\": \"doc%d\"}", values[i], i);
    const char* id = apxdb_put_document("test_collection", json);
    ASSERT(id != NULL, "put_document failed");
    apxdb_release_string(id);
  }

  ASSERT(apxdb_commit_write_txn(txn) == 0, "commit transaction failed");
  apxdb_free_transaction(txn);
  ASSERT(apxdb_close() == APXDB_OK, "close failed");

  register_test_collection();
  add_index(false);
  ASSERT(open_db(dir), "reopen_db failed");

  assert_query_result_count("test_collection", "{\"value\": 5}", 2);
  assert_query_result_count("test_collection", "{\"value\": {\"gte\": 5}}", 3);
  assert_query_result_count("test_collection", "{\"value\": {\"lt\": 5}}", 2);

  ASSERT(apxdb_close() == APXDB_OK, "close failed");
}

static void run_test_layout_invariants(const char* dir) {
  (void)dir;
  printf("TEST: layout_invariants\n");

  static const apxdb_field_schema_t layout_fields[] = {
    {.struct_size = sizeof(apxdb_field_schema_t), .version = 1, .field_id = 1, .name = "value", .type = APXDB_FIELD_INT64, .flags = APXDB_FIELD_FLAG_NULLABLE, .type_size = sizeof(int64_t), .storage_kind = APXDB_STORAGE_FIXED},
    {.struct_size = sizeof(apxdb_field_schema_t), .version = 1, .field_id = 2, .name = "name", .type = APXDB_FIELD_STRING, .flags = 0, .type_size = 0, .storage_kind = APXDB_STORAGE_VARIABLE},
    {.struct_size = sizeof(apxdb_field_schema_t), .version = 1, .field_id = 3, .name = "score", .type = APXDB_FIELD_DOUBLE, .flags = APXDB_FIELD_FLAG_NULLABLE, .type_size = sizeof(double), .storage_kind = APXDB_STORAGE_FIXED},
  };

  static const apxdb_collection_schema_t layout_schema = {
    .struct_size = sizeof(apxdb_collection_schema_t),
    .version = 1,
    .collection_id = 1,
    .name = "test_collection",
    .field_count = 3,
    .fields = layout_fields,
    .index_count = 0,
    .indexes = NULL,
  };

  const apxdb_collection_schema_t* registered = apxdb_register_schema(&layout_schema);
  ASSERT(registered == &layout_schema, "apxdb_register_schema failed");

  const apxdb_collection_layout_t* layout = apxdb_find_collection_layout_by_name("test_collection");
  ASSERT(layout != NULL, "layout registry lookup failed");
  ASSERT(layout->field_count == 3, "unexpected layout field count");
  ASSERT(layout->variable_slot_count == 1, "unexpected layout variable slot count");
  ASSERT(layout->nullable_bitmap_size == 1, "unexpected nullable bitmap size");
  ASSERT(layout->row_fixed_size == 1 + sizeof(int64_t) + sizeof(uint32_t) * 2 + sizeof(double), "unexpected row fixed size");

  for (size_t i = 0; i < layout->field_count; ++i) {
    const apxdb_collection_field_layout_t* field = &layout->fields[i];
    ASSERT(field->offset + field->size <= layout->row_fixed_size, "layout offset overflow");
  }

  for (size_t i = 0; i < layout->field_count; ++i) {
    for (size_t j = i + 1; j < layout->field_count; ++j) {
      uint32_t start_i = layout->fields[i].offset;
      uint32_t end_i = layout->fields[i].offset + layout->fields[i].size;
      uint32_t start_j = layout->fields[j].offset;
      uint32_t end_j = layout->fields[j].offset + layout->fields[j].size;
      ASSERT(end_i <= start_j || end_j <= start_i, "layout field ranges overlap");
    }
  }

  apxdb_unregister_all_schemas();
}

static void run_test_get_document_round_trip(const char* dir) {
  cleanup_test_files(dir);
  printf("TEST: get_document_round_trip\n");

  static const apxdb_field_schema_t layout_fields[] = {
    {.struct_size = sizeof(apxdb_field_schema_t), .version = 1, .field_id = 1, .name = "value", .type = APXDB_FIELD_INT64, .flags = 0, .type_size = sizeof(int64_t), .storage_kind = APXDB_STORAGE_FIXED},
    {.struct_size = sizeof(apxdb_field_schema_t), .version = 1, .field_id = 2, .name = "title", .type = APXDB_FIELD_STRING, .flags = 0, .type_size = 0, .storage_kind = APXDB_STORAGE_VARIABLE},
    {.struct_size = sizeof(apxdb_field_schema_t), .version = 1, .field_id = 3, .name = "active", .type = APXDB_FIELD_BOOL, .flags = 0, .type_size = 1, .storage_kind = APXDB_STORAGE_FIXED},
    {.struct_size = sizeof(apxdb_field_schema_t), .version = 1, .field_id = 4, .name = "rating", .type = APXDB_FIELD_DOUBLE, .flags = APXDB_FIELD_FLAG_NULLABLE, .type_size = sizeof(double), .storage_kind = APXDB_STORAGE_FIXED},
    {.struct_size = sizeof(apxdb_field_schema_t), .version = 1, .field_id = 5, .name = "notes", .type = APXDB_FIELD_STRING, .flags = APXDB_FIELD_FLAG_NULLABLE, .type_size = 0, .storage_kind = APXDB_STORAGE_VARIABLE},
  };

  static const apxdb_collection_schema_t layout_schema = {
    .struct_size = sizeof(apxdb_collection_schema_t),
    .version = 1,
    .collection_id = 2,
    .name = "test_collection",
    .field_count = 5,
    .fields = layout_fields,
    .index_count = 0,
    .indexes = NULL,
  };

  const apxdb_collection_schema_t* registered = apxdb_register_schema(&layout_schema);
  ASSERT(registered == &layout_schema, "apxdb_register_schema failed");

  static const apxdb_schema_field_t collection_fields[] = {
    {"value", APXDB_TYPE_INT, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
    {"title", APXDB_TYPE_STRING, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
    {"active", APXDB_TYPE_BOOL, false, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
    {"rating", APXDB_TYPE_DOUBLE, true, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
    {"notes", APXDB_TYPE_STRING, true, NULL, 0, NULL, APXDB_ENUM_STRATEGY_ORDINAL, NULL, 0},
  };

  static const apxdb_schema_t collection_schema = {
    .collection_name = "test_collection",
    .fields = collection_fields,
    .field_count = 5,
  };

  int32_t collection_result = apxdb_register_collection(&collection_schema);
  ASSERT(collection_result == 0, "register_collection failed");
  ASSERT(open_db(dir), "open_db failed");

  apxdb_transaction_t* txn = apxdb_begin_write_txn();
  ASSERT(txn != NULL, "begin transaction failed");

  const char* input_json = "{\"value\": 123, \"title\": \"roundtrip\", \"active\": true, \"rating\": null, \"notes\": null}";
  const char* id = apxdb_put_document("test_collection", input_json);
  ASSERT(id != NULL, "put_document failed");

  int32_t result = apxdb_commit_write_txn(txn);
  ASSERT(result == 0, "commit transaction failed");
  apxdb_free_transaction(txn);

  const char* output = apxdb_get_document("test_collection", id);
  ASSERT(output != NULL, "get_document failed");
  ASSERT(strcmp(output, "{\"value\":123,\"title\":\"roundtrip\",\"active\":true,\"rating\":null,\"notes\":null}") == 0,
         "unexpected round-trip document");
  apxdb_release_string(output);
  apxdb_release_string(id);

  ASSERT(apxdb_close() == APXDB_OK, "close failed");
  apxdb_unregister_all_schemas();
}

int main(void) {
  char* dir = make_temp_dir();
  ASSERT(dir != NULL, "failed to create temporary directory");

  run_test_save_reopen(dir);
  run_test_corrupt_header_rebuild(dir);
  run_test_declaration_mismatch(dir);
  run_test_missing_idx_rebuild(dir);
  run_test_stale_doc_ids_reject(dir);
  run_test_gpu_numeric_query_path(dir);
  run_test_numeric_exact_and_range_queries(dir);
  run_test_multi_predicate_planner(dir);
  run_test_multi_predicate_exact_beats_wide_range(dir);
  run_test_multi_predicate_scan_fallback(dir);
  run_test_numeric_index_persistence(dir);
  run_test_layout_invariants(dir);
  run_test_get_document_round_trip(dir);

  printf("All apxdb index persistence tests passed.\n");
  rmdir(dir);
  free(dir);
  return 0;
}
