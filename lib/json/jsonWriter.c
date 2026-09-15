#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static int buf_reserve(StrBuf *buf, size_t extra) {
    if (buf->len + extra + 1 <= buf->cap) {
        return 1;
    }
    size_t new_cap = (buf->cap == 0) ? 64 : buf->cap;
    while (new_cap < buf->len + extra + 1) {
        new_cap *= 2;
    }
    char *grown = realloc(buf->data, new_cap);
    if (grown == NULL) {
        return 0;
    }
    buf->data = grown;
    buf->cap = new_cap;
    return 1;
}

static int buf_append(StrBuf *buf, const char *text, size_t text_len) {
    if (!buf_reserve(buf, text_len)) {
        return 0;
    }
    memcpy(buf->data + buf->len, text, text_len);
    buf->len += text_len;
    buf->data[buf->len] = '\0';
    return 1;
}

static int buf_append_str(StrBuf *buf, const char *text) {
    return buf_append(buf, text, strlen(text));
}

static int append_escaped_string(StrBuf *buf, const char *text) {
    if (!buf_append_str(buf, "\"")) {
        return 0;
    }
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; c++) {
        char escaped[8];
        switch (*c) {
            case '"':  if (!buf_append_str(buf, "\\\"")) return 0; continue;
            case '\\': if (!buf_append_str(buf, "\\\\")) return 0; continue;
            case '\n': if (!buf_append_str(buf, "\\n")) return 0; continue;
            case '\r': if (!buf_append_str(buf, "\\r")) return 0; continue;
            case '\t': if (!buf_append_str(buf, "\\t")) return 0; continue;
            default:
                if (*c < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", *c);
                    if (!buf_append_str(buf, escaped)) return 0;
                    continue;
                }
                escaped[0] = (char)*c;
                if (!buf_append(buf, escaped, 1)) return 0;
                continue;
        }
    }
    return buf_append_str(buf, "\"");
}

static int stringify_value(StrBuf *buf, const JsonValue *value) {
    if (value == NULL) {
        return buf_append_str(buf, "null");
    }
    switch (value->type) {
        case JSON_NULL:
            return buf_append_str(buf, "null");
        case JSON_BOOL:
            return buf_append_str(buf, value->as.boolean ? "true" : "false");
        case JSON_NUMBER: {
            char text[32];
            snprintf(text, sizeof(text), "%g", value->as.number);
            return buf_append_str(buf, text);
        }
        case JSON_STRING:
            return append_escaped_string(buf, value->as.string);
        case JSON_ARRAY: {
            if (!buf_append_str(buf, "[")) return 0;
            for (size_t i = 0; i < value->as.array.count; i++) {
                if (i > 0 && !buf_append_str(buf, ",")) return 0;
                if (!stringify_value(buf, value->as.array.items[i])) return 0;
            }
            return buf_append_str(buf, "]");
        }
        case JSON_OBJECT: {
            if (!buf_append_str(buf, "{")) return 0;
            for (size_t i = 0; i < value->as.object.count; i++) {
                if (i > 0 && !buf_append_str(buf, ",")) return 0;
                if (!append_escaped_string(buf, value->as.object.members[i].key)) return 0;
                if (!buf_append_str(buf, ":")) return 0;
                if (!stringify_value(buf, value->as.object.members[i].value)) return 0;
            }
            return buf_append_str(buf, "}");
        }
    }
    return buf_append_str(buf, "null");
}

char *json_stringify(const JsonValue *value) {
    StrBuf buf = { .data = NULL, .len = 0, .cap = 0 };
    if (!buf_reserve(&buf, 1)) {
        return NULL;
    }
    buf.data[0] = '\0';

    if (!stringify_value(&buf, value)) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}
