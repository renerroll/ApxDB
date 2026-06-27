#ifndef APXDB_NATIVE_H
#define APXDB_NATIVE_H

#include <stdint.h>
#include "apxdb_schema.h"
#include "apxdb_collection.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t apxdb_initialize();
int32_t apxdb_shutdown();
int32_t apxdb_open(const char* directory_path);
int32_t apxdb_close(void);
const char* apxdb_create_document(const char* json_utf8);
const char* apxdb_find_document(const char* query_utf8);
void apxdb_release_string(const char* utf8);

#ifdef __cplusplus
}
#endif

#endif // APXDB_NATIVE_H
