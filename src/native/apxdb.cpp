#include "apxdb.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

static std::mutex g_mutex;
static bool g_initialized = false;
static std::unordered_map<std::string, std::string> g_documents;
static std::atomic<int32_t> g_documentCounter{0};

static char* allocateUtf8String(const std::string& text) {
  char* buffer = static_cast<char*>(malloc(text.size() + 1));
  if (!buffer) {
    return nullptr;
  }
  memcpy(buffer, text.c_str(), text.size() + 1);
  return buffer;
}

static std::string buildDocumentId() {
  const int32_t id = ++g_documentCounter;
  std::ostringstream out;
  out << "doc_" << id;
  return out.str();
}

static void runComputeKernelPlaceholder() {
  // Future Vulkan/Metal compute kernels should be launched from here.
  // This stub is a placeholder for platform-specific GPU initialization.
}

int32_t apxdb_initialize() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_initialized) {
    return 0;
  }
  g_documents.clear();
  g_documentCounter = 0;
  g_initialized = true;
  runComputeKernelPlaceholder();
  return 0;
}

int32_t apxdb_shutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized) {
    return 0;
  }
  g_documents.clear();
  g_initialized = false;
  return 0;
}

const char* apxdb_create_document(const char* json_utf8) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized || !json_utf8) {
    return allocateUtf8String("error:uninitialized");
  }
  const std::string document(json_utf8);
  const std::string id = buildDocumentId();
  g_documents[id] = document;
  return allocateUtf8String(id);
}

const char* apxdb_find_document(const char* query_utf8) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized || !query_utf8) {
    return allocateUtf8String("error:uninitialized");
  }
  const std::string query(query_utf8);
  for (const auto& entry : g_documents) {
    if (entry.first == query || entry.second.find(query) != std::string::npos) {
      return allocateUtf8String(entry.second);
    }
  }
  return allocateUtf8String("not_found");
}

void apxdb_release_string(const char* utf8) {
  free(const_cast<char*>(utf8));
}
