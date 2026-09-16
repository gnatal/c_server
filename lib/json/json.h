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

/*
 * Builders: construct a JsonValue tree by hand instead of parsing one, for
 * callers that need to serialize data they already have in memory (e.g.
 * echoing parsed query-string params back as JSON). Returns NULL on
 * allocation failure. json_new_string copies value, so the caller's own
 * buffer doesn't need to outlive the returned JsonValue.
 */
JsonValue *json_new_string(const char *value);
JsonValue *json_new_object(void);

/*
 * Attaches value under key on object (which must be a JSON_OBJECT, e.g. from
 * json_new_object), copying key. Takes ownership of value on both success and
 * failure - json_free(value) has already been called if this returns 0
 * (out of memory, object is NULL/not an object, or key is NULL), so the
 * caller must not touch or free value again either way. Once attached,
 * value is released only as part of object's own json_free.
 */
int json_object_set(JsonValue *object, const char *key, JsonValue *value);

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
