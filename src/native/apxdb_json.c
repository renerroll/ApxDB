#include "apxdb_json.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char* skip_whitespace(const char* text) {
  while (*text && isspace((unsigned char)*text)) {
    text++;
  }
  return text;
}

static bool parse_string(const char** input, char** out_string) {
  const char* text = *input;
  if (*text != '"') {
    return false;
  }
  text++;
  size_t capacity = 64;
  size_t length = 0;
  char* result = (char*)malloc(capacity);
  if (!result) {
    return false;
  }

  while (*text && *text != '"') {
    if (*text == '\\') {
      text++;
      char ch = *text;
      if (!ch) {
        free(result);
        return false;
      }
      switch (ch) {
        case '"': result[length++] = '"'; break;
        case '\\': result[length++] = '\\'; break;
        case '/': result[length++] = '/'; break;
        case 'b': result[length++] = '\b'; break;
        case 'f': result[length++] = '\f'; break;
        case 'n': result[length++] = '\n'; break;
        case 'r': result[length++] = '\r'; break;
        case 't': result[length++] = '\t'; break;
        default:
          free(result);
          return false;
      }
      text++;
    } else {
      result[length++] = *text++;
    }
    if (length + 1 >= capacity) {
      capacity *= 2;
      char* next = (char*)realloc(result, capacity);
      if (!next) {
        free(result);
        return false;
      }
      result = next;
    }
  }

  if (*text != '"') {
    free(result);
    return false;
  }
  result[length] = '\0';
  *out_string = result;
  *input = text + 1;
  return true;
}

static bool parse_number(const char** input, apxdb_json_value_t* value) {
  const char* text = *input;
  const char* start = text;
  bool negative = false;
  if (*text == '+' || *text == '-') {
    negative = (*text == '-');
    text++;
  }
  bool has_digits = false;
  while (isdigit((unsigned char)*text)) {
    has_digits = true;
    text++;
  }
  bool is_integer = true;
  if (*text == '.') {
    is_integer = false;
    text++;
    if (!isdigit((unsigned char)*text)) {
      return false;
    }
    while (isdigit((unsigned char)*text)) {
      text++;
    }
  }
  if (*text == 'e' || *text == 'E') {
    is_integer = false;
    text++;
    if (*text == '+' || *text == '-') {
      text++;
    }
    if (!isdigit((unsigned char)*text)) {
      return false;
    }
    while (isdigit((unsigned char)*text)) {
      text++;
    }
  }
  if (!has_digits) {
    return false;
  }

  char* endptr = NULL;
  double double_value = strtod(start, &endptr);
  if (endptr != text) {
    return false;
  }
  int64_t integer_value = 0;
  if (is_integer) {
    integer_value = strtoll(start, &endptr, 10);
    if (endptr != text) {
      return false;
    }
  }

  value->type = APXDB_JSON_NUMBER;
  value->u.number.is_integer = is_integer;
  value->u.number.integer_value = integer_value;
  value->u.number.double_value = double_value;
  *input = text;
  return true;
}

static bool parse_value(const char** input, apxdb_json_value_t* out_value);

static bool parse_array(const char** input, apxdb_json_value_t* out_value) {
  const char* text = *input;
  if (*text != '[') {
    return false;
  }
  text++;
  text = skip_whitespace(text);

  out_value->type = APXDB_JSON_ARRAY;
  out_value->u.array.count = 0;
  out_value->u.array.values = NULL;

  if (*text == ']') {
    *input = text + 1;
    return true;
  }

  while (true) {
    apxdb_json_value_t element;
    if (!parse_value(&text, &element)) {
      apxdb_json_free(out_value);
      return false;
    }

    apxdb_json_value_t* next = (apxdb_json_value_t*)realloc(out_value->u.array.values, (out_value->u.array.count + 1) * sizeof(apxdb_json_value_t));
    if (!next) {
      apxdb_json_free(&element);
      apxdb_json_free(out_value);
      return false;
    }
    out_value->u.array.values = next;
    out_value->u.array.values[out_value->u.array.count++] = element;

    text = skip_whitespace(text);
    if (*text == ']') {
      *input = text + 1;
      return true;
    }
    if (*text != ',') {
      apxdb_json_free(out_value);
      return false;
    }
    text++;
    text = skip_whitespace(text);
  }
}

static bool parse_object(const char** input, apxdb_json_value_t* out_value) {
  const char* text = *input;
  if (*text != '{') {
    return false;
  }
  text++;
  text = skip_whitespace(text);

  out_value->type = APXDB_JSON_OBJECT;
  out_value->u.object.count = 0;
  out_value->u.object.members = NULL;

  if (*text == '}') {
    *input = text + 1;
    return true;
  }

  while (true) {
    char* name = NULL;
    if (!parse_string(&text, &name)) {
      apxdb_json_free(out_value);
      return false;
    }
    text = skip_whitespace(text);
    if (*text != ':') {
      free(name);
      apxdb_json_free(out_value);
      return false;
    }
    text++;
    text = skip_whitespace(text);

    apxdb_json_value_t member_value;
    if (!parse_value(&text, &member_value)) {
      free(name);
      apxdb_json_free(out_value);
      return false;
    }

    apxdb_json_member_t* next = (apxdb_json_member_t*)realloc(out_value->u.object.members, (out_value->u.object.count + 1) * sizeof(apxdb_json_member_t));
    if (!next) {
      free(name);
      apxdb_json_free(&member_value);
      apxdb_json_free(out_value);
      return false;
    }
    out_value->u.object.members = next;
    out_value->u.object.members[out_value->u.object.count].name = name;
    out_value->u.object.members[out_value->u.object.count].value = (apxdb_json_value_t*)malloc(sizeof(apxdb_json_value_t));
    if (!out_value->u.object.members[out_value->u.object.count].value) {
      free(name);
      apxdb_json_free(&member_value);
      apxdb_json_free(out_value);
      return false;
    }
    *out_value->u.object.members[out_value->u.object.count].value = member_value;
    out_value->u.object.count++;

    text = skip_whitespace(text);
    if (*text == '}') {
      *input = text + 1;
      return true;
    }
    if (*text != ',') {
      apxdb_json_free(out_value);
      return false;
    }
    text++;
    text = skip_whitespace(text);
  }
}

static bool parse_value(const char** input, apxdb_json_value_t* out_value) {
  const char* text = skip_whitespace(*input);
  if (!*text) {
    return false;
  }
  if (*text == '"') {
    out_value->type = APXDB_JSON_STRING;
    out_value->u.string_value = NULL;
    if (!parse_string(&text, &out_value->u.string_value)) {
      return false;
    }
    *input = text;
    return true;
  }
  if (*text == '{') {
    return parse_object(&text, out_value) ? (*input = text, true) : false;
  }
  if (*text == '[') {
    return parse_array(&text, out_value) ? (*input = text, true) : false;
  }
  if (*text == 't' && strncmp(text, "true", 4) == 0) {
    out_value->type = APXDB_JSON_BOOL;
    out_value->u.bool_value = true;
    *input = text + 4;
    return true;
  }
  if (*text == 'f' && strncmp(text, "false", 5) == 0) {
    out_value->type = APXDB_JSON_BOOL;
    out_value->u.bool_value = false;
    *input = text + 5;
    return true;
  }
  if (*text == 'n' && strncmp(text, "null", 4) == 0) {
    out_value->type = APXDB_JSON_NULL;
    *input = text + 4;
    return true;
  }
  return parse_number(&text, out_value) ? (*input = text, true) : false;
}

bool apxdb_json_parse(const char* text, apxdb_json_value_t* out_value) {
  if (!text || !out_value) {
    return false;
  }
  const char* cursor = text;
  if (!parse_value(&cursor, out_value)) {
    return false;
  }
  cursor = skip_whitespace(cursor);
  return *cursor == '\0';
}

static void free_value(apxdb_json_value_t* value) {
  if (!value) {
    return;
  }
  switch (value->type) {
    case APXDB_JSON_STRING:
      free(value->u.string_value);
      break;
    case APXDB_JSON_ARRAY:
      for (size_t i = 0; i < value->u.array.count; ++i) {
        free_value(&value->u.array.values[i]);
      }
      free(value->u.array.values);
      break;
    case APXDB_JSON_OBJECT:
      for (size_t i = 0; i < value->u.object.count; ++i) {
        free(value->u.object.members[i].name);
        free_value(value->u.object.members[i].value);
        free(value->u.object.members[i].value);
      }
      free(value->u.object.members);
      break;
    default:
      break;
  }
  value->type = APXDB_JSON_INVALID;
}

void apxdb_json_free(apxdb_json_value_t* value) {
  free_value(value);
}

const apxdb_json_value_t* apxdb_json_object_get(const apxdb_json_value_t* object, const char* name) {
  if (!object || object->type != APXDB_JSON_OBJECT || !name) {
    return NULL;
  }
  for (size_t i = 0; i < object->u.object.count; ++i) {
    if (strcmp(object->u.object.members[i].name, name) == 0) {
      return object->u.object.members[i].value;
    }
  }
  return NULL;
}

const apxdb_json_value_t* apxdb_json_array_get(const apxdb_json_value_t* array, size_t index) {
  if (!array || array->type != APXDB_JSON_ARRAY) {
    return NULL;
  }
  if (index >= array->u.array.count) {
    return NULL;
  }
  return &array->u.array.values[index];
}
