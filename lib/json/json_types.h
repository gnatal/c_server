#ifndef JSON_TYPES_H
#define JSON_TYPES_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

/* One key/value pair inside a JSON_OBJECT. */
typedef struct {
    char *key;
    JsonValue *value;
} JsonMember;

struct JsonValue {
    JsonType type;
    union {
        int boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            size_t count;
        } array;
        struct {
            JsonMember *members;
            size_t count;
        } object;
    } as;
};

/* Cursor over the input buffer being parsed; pos advances as tokens are consumed. */
typedef struct {
    const char *input;
    size_t pos;
    size_t len;
    char error[128];
} JsonParser;

/* Growable byte buffer used by json_writer.c (json_stringify and JsonWriter). */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

#define JSON_WRITER_MAX_DEPTH 32

/* Streaming JSON emitter (jw_* in json.h). Owns buf.data until jw_free().
 * Commas and quoting are automatic. `failed` is sticky: out of memory, nesting deeper
 * than JSON_WRITER_MAX_DEPTH, or a misuse (key outside an object, value with no key,
 * unbalanced end) sets it, and every later jw_* call becomes a no-op. */
typedef struct {
    StrBuf buf;
    int depth;
    char kind[JSON_WRITER_MAX_DEPTH];      /* '{' or '[' per open container */
    char has_item[JSON_WRITER_MAX_DEPTH];  /* container already holds an item (next needs a comma) */
    int expect_value;                      /* a key was just written; next call must be a value */
    int failed;
} JsonWriter;

#endif /* JSON_TYPES_H */
