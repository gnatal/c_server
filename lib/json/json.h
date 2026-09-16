#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include "json_types.h"

/*
 * Parses a NUL-terminated JSON document. Returns NULL on failure; if err is
 * non-NULL, a human-readable message is written into it (up to err_size
 * bytes). On success, the caller owns the returned value and must release
 * it with json_free.
 */
JsonValue *json_parse(const char *input, char *err, size_t err_size);

/* Recursively releases a value returned by json_parse. Safe to call with NULL. */
void json_free(JsonValue *value);

/* Accessors: return the given default/NULL when value is NULL or the wrong type, never crash. */
const JsonValue *json_object_get(const JsonValue *object, const char *key);
const JsonValue *json_array_get(const JsonValue *array, size_t index);
size_t json_array_count(const JsonValue *array);
int json_is_null(const JsonValue *value);
int json_as_bool(const JsonValue *value, int default_value);
double json_as_number(const JsonValue *value, double default_value);
const char *json_as_string(const JsonValue *value, const char *default_value);

/* Serializes value into a newly heap-allocated NUL-terminated string; caller frees it with free(). */
char *json_stringify(const JsonValue *value);

#endif /* JSON_H */
