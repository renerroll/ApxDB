#ifndef APXDB_COLLECTION_H
#define APXDB_COLLECTION_H

#include <stdint.h>
#include <stdbool.h>
#include "apxdb_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct apxdb_collection_t apxdb_collection_t;
typedef struct apxdb_query_t apxdb_query_t;
typedef struct apxdb_transaction_t apxdb_transaction_t;

typedef enum {
  APXDB_INDEX_TYPE_VALUE,
  APXDB_INDEX_TYPE_HASH,
  APXDB_INDEX_TYPE_HASH_ELEMENTS,
} apxdb_index_type_t;

typedef struct {
  const char* field_name;
  apxdb_index_type_t index_type;
  bool composite;
  bool multi_entry;
} apxdb_index_definition_t;

int32_t apxdb_register_collection(const apxdb_schema_t* schema);
int32_t apxdb_add_index(const char* collection_name, const apxdb_index_definition_t* index);

void apxdb_collection_unregister_all(void);

apxdb_transaction_t* apxdb_begin_write_txn(void);
int32_t apxdb_commit_write_txn(apxdb_transaction_t* txn);
int32_t apxdb_rollback_write_txn(apxdb_transaction_t* txn);
void apxdb_free_transaction(apxdb_transaction_t* txn);

const char* apxdb_get_document(const char* collection_name, const char* id);
const char* apxdb_find_documents(const char* collection_name, const char* query_json);
const char* apxdb_put_document(const char* collection_name, const char* json_utf8);
int32_t apxdb_delete_document(const char* collection_name, const char* id);

int32_t apxdb_save_collection(const char* collection_name, const char* file_path);
int32_t apxdb_load_collection(const char* collection_name, const char* file_path);
int32_t apxdb_save_all_collections(const char* directory_path);
int32_t apxdb_load_all_collections(const char* directory_path);

#ifdef __cplusplus
}
#endif

#endif // APXDB_COLLECTION_H
