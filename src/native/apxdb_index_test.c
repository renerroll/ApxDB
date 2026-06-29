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

int main(void) {
  char* dir = make_temp_dir();
  ASSERT(dir != NULL, "failed to create temporary directory");

  run_test_save_reopen(dir);
  run_test_corrupt_header_rebuild(dir);
  run_test_declaration_mismatch(dir);
  run_test_missing_idx_rebuild(dir);
  run_test_stale_doc_ids_reject(dir);

  printf("All apxdb index persistence tests passed.\n");
  rmdir(dir);
  free(dir);
  return 0;
}
