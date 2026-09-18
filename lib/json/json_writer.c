#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

/* ---- growable buffer ---- */

static int buf_reserve(StrBuf *buf, const size_t extra) {
    if (buf->len + extra + 1 <= buf->cap) {
        return 1;
    }
    size_t new_cap = (buf->cap == 0) ? 2048 : buf->cap;
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

static int buf_append(StrBuf *buf, const char *text, const size_t text_len) {
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

/* Appends `text` as a quoted, escaped JSON string. Runs of characters that need no escape
 * are copied in one memcpy instead of byte by byte. */
static int append_escaped_string(StrBuf *buf, const char *text) {
    if (!buf_append(buf, "\"", 1)) {
        return 0;
    }
    const char *run = text;
    for (const char *c = text;; c++) {
        const unsigned char ch = (unsigned char)*c;
        if (ch >= 0x20 && ch != '"' && ch != '\\' && ch != '\0') {
            continue;
        }
        if (c > run && !buf_append(buf, run, (size_t)(c - run))) {
            return 0;
        }
        run = c + 1;
        if (ch == '\0') {
            break;
        }
        char escaped[8];
        switch (ch) {
            case '"':  if (!buf_append(buf, "\\\"", 2)) return 0; break;
            case '\\': if (!buf_append(buf, "\\\\", 2)) return 0; break;
            case '\n': if (!buf_append(buf, "\\n", 2)) return 0; break;
            case '\r': if (!buf_append(buf, "\\r", 2)) return 0; break;
            case '\t': if (!buf_append(buf, "\\t", 2)) return 0; break;
            default:
                snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                if (!buf_append(buf, escaped, 6)) return 0;
                break;
        }
    }
    return buf_append(buf, "\"", 1);
}

static int append_int(StrBuf *buf, const long long value) {
    char digits[24];
    size_t i = sizeof(digits);
    /* Negate in unsigned arithmetic so LLONG_MIN does not overflow. */
    unsigned long long magnitude = value < 0 ? 0ULL - (unsigned long long)value : (unsigned long long)value;
    do {
        digits[--i] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0) {
        digits[--i] = '-';
    }
    return buf_append(buf, digits + i, sizeof(digits) - i);
}

/* Integers below 2^53 print exactly (no exponent form); other finite doubles print with 17
 * significant digits so they round-trip; NaN/infinity have no JSON form and become null.
 * Plain comparisons instead of <math.h> keep libm out of the link. */
static int append_number(StrBuf *buf, const double value) {
    if (value != value || value > DBL_MAX || value < -DBL_MAX) {
        return buf_append_str(buf, "null");
    }
    if (value > -9007199254740992.0 && value < 9007199254740992.0) {
        const long long as_int = (long long)value;
        if ((double)as_int == value) {
            return append_int(buf, as_int);
        }
    }
    char text[32];
    const int n = snprintf(text, sizeof(text), "%.17g", value);
    return buf_append(buf, text, (size_t)n);
}

/* ---- tree serialization ---- */

static int stringify_value(StrBuf *buf, const JsonValue *value) {
    if (value == NULL) {
        return buf_append_str(buf, "null");
    }
    switch (value->type) {
        case JSON_NULL:
            return buf_append_str(buf, "null");
        case JSON_BOOL:
            return buf_append_str(buf, value->as.boolean ? "true" : "false");
        case JSON_NUMBER:
            return append_number(buf, value->as.number);
        case JSON_STRING:
            return append_escaped_string(buf, value->as.string);
        case JSON_ARRAY: {
            if (!buf_append(buf, "[", 1)) return 0;
            for (size_t i = 0; i < value->as.array.count; i++) {
                if (i > 0 && !buf_append(buf, ",", 1)) return 0;
                if (!stringify_value(buf, value->as.array.items[i])) return 0;
            }
            return buf_append(buf, "]", 1);
        }
        case JSON_OBJECT: {
            if (!buf_append(buf, "{", 1)) return 0;
            for (size_t i = 0; i < value->as.object.count; i++) {
                if (i > 0 && !buf_append(buf, ",", 1)) return 0;
                if (!append_escaped_string(buf, value->as.object.members[i].key)) return 0;
                if (!buf_append(buf, ":", 1)) return 0;
                if (!stringify_value(buf, value->as.object.members[i].value)) return 0;
            }
            return buf_append(buf, "}", 1);
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

/* ---- streaming writer ---- */

void jw_init(JsonWriter *w) {
    w->buf.data = NULL;
    w->buf.len = 0;
    w->buf.cap = 0;
    w->depth = 0;
    w->expect_value = 0;
    w->failed = 0;
}

/* Runs before every value (or container start): writes the separating comma and enforces the
 * key/value protocol. Returns 0 (and marks the writer failed) on misuse. */
static int begin_value(JsonWriter *w) {
    if (w->failed) {
        return 0;
    }
    if (w->depth == 0) {
        if (w->buf.len != 0) {
            w->failed = 1; /* a second top-level value */
            return 0;
        }
        return 1;
    }
    const int d = w->depth - 1;
    if (w->kind[d] == '{') {
        if (!w->expect_value) {
            w->failed = 1; /* object member without jw_key */
            return 0;
        }
        w->expect_value = 0;
        return 1;
    }
    if (w->has_item[d] && !buf_append(&w->buf, ",", 1)) {
        w->failed = 1;
        return 0;
    }
    w->has_item[d] = 1;
    return 1;
}

static void begin_container(JsonWriter *w, const char kind) {
    if (!begin_value(w)) {
        return;
    }
    if (w->depth >= JSON_WRITER_MAX_DEPTH) {
        w->failed = 1;
        return;
    }
    if (!buf_append(&w->buf, kind == '{' ? "{" : "[", 1)) {
        w->failed = 1;
        return;
    }
    w->kind[w->depth] = kind;
    w->has_item[w->depth] = 0;
    w->depth++;
}

static void end_container(JsonWriter *w, const char kind) {
    if (w->failed) {
        return;
    }
    if (w->depth == 0 || w->kind[w->depth - 1] != kind || w->expect_value) {
        w->failed = 1; /* unbalanced end, wrong kind, or a key with no value */
        return;
    }
    w->depth--;
    if (!buf_append(&w->buf, kind == '{' ? "}" : "]", 1)) {
        w->failed = 1;
        return;
    }
    if (w->depth > 0 && w->kind[w->depth - 1] == '{') {
        w->has_item[w->depth - 1] = 1;
    }
}

void jw_object_begin(JsonWriter *w) { begin_container(w, '{'); }
void jw_object_end(JsonWriter *w)   { end_container(w, '{'); }
void jw_array_begin(JsonWriter *w)  { begin_container(w, '['); }
void jw_array_end(JsonWriter *w)    { end_container(w, '['); }

void jw_key(JsonWriter *w, const char *key) {
    if (w->failed) {
        return;
    }
    if (key == NULL || w->depth == 0 || w->kind[w->depth - 1] != '{' || w->expect_value) {
        w->failed = 1;
        return;
    }
    const int d = w->depth - 1;
    if ((w->has_item[d] && !buf_append(&w->buf, ",", 1)) || !append_escaped_string(&w->buf, key) ||
        !buf_append(&w->buf, ":", 1)) {
        w->failed = 1;
        return;
    }
    w->has_item[d] = 1;
    w->expect_value = 1;
}

/* Object members are marked has_item by jw_key; array items by begin_value. */
void jw_string(JsonWriter *w, const char *value) {
    if (!begin_value(w)) {
        return;
    }
    if (!(value == NULL ? buf_append_str(&w->buf, "null") : append_escaped_string(&w->buf, value))) {
        w->failed = 1;
    }
}

void jw_int(JsonWriter *w, const long long value) {
    if (begin_value(w) && !append_int(&w->buf, value)) {
        w->failed = 1;
    }
}

void jw_double(JsonWriter *w, const double value) {
    if (begin_value(w) && !append_number(&w->buf, value)) {
        w->failed = 1;
    }
}

void jw_bool(JsonWriter *w, const int value) {
    if (begin_value(w) && !buf_append_str(&w->buf, value ? "true" : "false")) {
        w->failed = 1;
    }
}

void jw_null(JsonWriter *w) {
    if (begin_value(w) && !buf_append_str(&w->buf, "null")) {
        w->failed = 1;
    }
}

int jw_ok(const JsonWriter *w) {
    return !w->failed && w->depth == 0 && !w->expect_value && w->buf.len > 0;
}

const char *jw_data(const JsonWriter *w) {
    return jw_ok(w) ? w->buf.data : NULL;
}

size_t jw_len(const JsonWriter *w) {
    return jw_ok(w) ? w->buf.len : 0;
}

void jw_free(JsonWriter *w) {
    free(w->buf.data);
    w->buf.data = NULL;
    w->buf.len = 0;
    w->buf.cap = 0;
    w->depth = 0;
    w->expect_value = 0;
}
