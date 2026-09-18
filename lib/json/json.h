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
 * Tree builders (json_new_*, json_object_set, json_array_append): for editing or forwarding a
 * parsed document. To EMIT JSON from your own data prefer JsonWriter (jw_*, below): no tree,
 * no per-node malloc (about 4x faster, see `make bench`), and no ownership transfer to get wrong.
 * Returns NULL on allocation failure. json_new_string copies value.
 */
JsonValue *json_new_string(const char *value);
JsonValue *json_new_object(void);

/* Same allocation shape as json_new_string/json_new_object above. */
JsonValue *json_new_number(double value);
JsonValue *json_new_bool(int value);
JsonValue *json_new_array(void);

/*
 * Attaches value under key on object (which must be a JSON_OBJECT, e.g. from
 * json_new_object), copying key. Takes ownership of value on both success and
 * failure - json_free(value) has already been called if this returns 0
 * (out of memory, object is NULL/not an object, or key is NULL), so the
 * caller must not touch or free value again either way. Once attached,
 * value is released only as part of object's own json_free.
 */
int json_object_set(JsonValue *object, const char *key, JsonValue *value);

/*
 * Appends value to the end of array (which must be a JSON_ARRAY, e.g. from
 * json_new_array). Same takes-ownership-either-way contract as
 * json_object_set: value is freed on failure (array is NULL/not an array,
 * value is NULL, or allocation failure), attached on success.
 */
int json_array_append(JsonValue *array, JsonValue *value);

/* Accessors: return the given default/NULL when value is NULL or the wrong type, never crash. */
const JsonValue *json_object_get(const JsonValue *object, const char *key);
const JsonValue *json_array_get(const JsonValue *array, size_t index);
size_t json_array_count(const JsonValue *array);
int json_is_null(const JsonValue *value);
int json_as_bool(const JsonValue *value, int default_value);
double json_as_number(const JsonValue *value, double default_value);
const char *json_as_string(const JsonValue *value, const char *default_value);

/* Serializes value into a newly heap-allocated NUL-terminated string; caller frees it with free().
 * Integers up to 2^53 print exactly; other doubles print with 17 significant digits (round-trips);
 * NaN and infinity become null. */
char *json_stringify(const JsonValue *value);

/*
 * JsonWriter: append-only JSON emitter. Write in document order; commas are automatic.
 *
 *   JsonWriter w;
 *   jw_init(&w);
 *   jw_object_begin(&w);
 *     jw_key(&w, "id");    jw_int(&w, 42);
 *     jw_key(&w, "tags");  jw_array_begin(&w); jw_string(&w, "a"); jw_string(&w, "b"); jw_array_end(&w);
 *   jw_object_end(&w);
 *   if (jw_ok(&w)) res_json(res, jw_data(&w)); else { res_status(res, 500); res_send(res, "error"); }
 *   jw_free(&w);                       // always call; safe after failure and when called twice
 *
 * Inside an object every value must be preceded by jw_key; inside an array or at top level no key.
 * Strings are copied and escaped; NULL string -> null. jw_data is NUL-terminated and stays valid
 * until jw_free or the next write. Nesting is limited to JSON_WRITER_MAX_DEPTH.
 */
void jw_init(JsonWriter *w);
void jw_object_begin(JsonWriter *w);
void jw_object_end(JsonWriter *w);
void jw_array_begin(JsonWriter *w);
void jw_array_end(JsonWriter *w);
void jw_key(JsonWriter *w, const char *key);
void jw_string(JsonWriter *w, const char *value);
void jw_int(JsonWriter *w, long long value);
void jw_double(JsonWriter *w, double value);
void jw_bool(JsonWriter *w, int value);
void jw_null(JsonWriter *w);
int jw_ok(const JsonWriter *w);          /* 1 if the document is complete and valid so far, else 0 */
const char *jw_data(const JsonWriter *w); /* NULL when !jw_ok */
size_t jw_len(const JsonWriter *w);
void jw_free(JsonWriter *w);

#endif /* JSON_H */
