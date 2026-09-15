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

/* Growable byte buffer used internally by jsonWriter.c while serializing. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

#endif /* JSON_TYPES_H */
