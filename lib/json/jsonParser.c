#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

static JsonValue *parse_value(JsonParser *p);

static void set_error(JsonParser *p, const char *msg) {
    if (p->error[0] == '\0') {
        snprintf(p->error, sizeof(p->error), "%s at offset %zu", msg, p->pos);
    }
}

static char peek(const JsonParser *p) {
    return p->pos < p->len ? p->input[p->pos] : '\0';
}

static char advance(JsonParser *p) {
    return p->pos < p->len ? p->input[p->pos++] : '\0';
}

static int match(JsonParser *p, char expected) {
    if (peek(p) != expected) {
        return 0;
    }
    p->pos++;
    return 1;
}

static void skip_whitespace(JsonParser *p) {
    while (p->pos < p->len) {
        const char c = p->input[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        p->pos++;
    }
}

static int match_literal(JsonParser *p, const char *literal) {
    const size_t literal_len = strlen(literal);
    if (p->pos + literal_len > p->len) {
        return 0;
    }
    if (strncmp(p->input + p->pos, literal, literal_len) != 0) {
        return 0;
    }
    p->pos += literal_len;
    return 1;
}

static JsonValue *value_new(JsonType type) {
    JsonValue *value = calloc(1, sizeof(JsonValue));
    if (value != NULL) {
        value->type = type;
    }
    return value;
}

/* --- dynamic byte buffer, used while unescaping a string literal --- */

static int buf_push(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) {
        const size_t new_cap = (*cap == 0) ? 32 : (*cap * 2);
        char *grown = realloc(*buf, new_cap);
        if (grown == NULL) {
            return 0;
        }
        *buf = grown;
        *cap = new_cap;
    }
    (*buf)[(*len)++] = c;
    return 1;
}

static int append_utf8(char **buf, size_t *len, size_t *cap, unsigned int cp) {
    unsigned char bytes[4];
    int n;
    if (cp <= 0x7F) {
        bytes[0] = (unsigned char)cp;
        n = 1;
    } else if (cp <= 0x7FF) {
        bytes[0] = (unsigned char)(0xC0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp <= 0xFFFF) {
        bytes[0] = (unsigned char)(0xE0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    for (int i = 0; i < n; i++) {
        if (!buf_push(buf, len, cap, (char)bytes[i])) {
            return 0;
        }
    }
    return 1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex4(JsonParser *p, unsigned int *out) {
    unsigned int value = 0;
    for (int i = 0; i < 4; i++) {
        const int digit = hex_value(advance(p));
        if (digit < 0) {
            return 0;
        }
        value = (value << 4) | (unsigned int)digit;
    }
    *out = value;
    return 1;
}

/* Consumes a "..." literal (cursor must be at the opening quote) and returns
 * a newly malloc'd, unescaped, NUL-terminated copy, or NULL on error. */
static char *parse_string_raw(JsonParser *p) {
    if (!match(p, '"')) {
        set_error(p, "expected string");
        return NULL;
    }

    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    for (;;) {
        if (p->pos >= p->len) {
            set_error(p, "unterminated string");
            free(buf);
            return NULL;
        }
        const char c = advance(p);
        if (c == '"') {
            break;
        }
        if ((unsigned char)c < 0x20) {
            set_error(p, "control character in string");
            free(buf);
            return NULL;
        }
        if (c != '\\') {
            if (!buf_push(&buf, &len, &cap, c)) {
                set_error(p, "out of memory");
                free(buf);
                return NULL;
            }
            continue;
        }

        const char esc = advance(p);
        char out_c;
        switch (esc) {
            case '"': out_c = '"'; break;
            case '\\': out_c = '\\'; break;
            case '/': out_c = '/'; break;
            case 'b': out_c = '\b'; break;
            case 'f': out_c = '\f'; break;
            case 'n': out_c = '\n'; break;
            case 'r': out_c = '\r'; break;
            case 't': out_c = '\t'; break;
            case 'u': {
                unsigned int cp;
                if (!parse_hex4(p, &cp)) {
                    set_error(p, "invalid unicode escape");
                    free(buf);
                    return NULL;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (advance(p) != '\\' || advance(p) != 'u') {
                        set_error(p, "unpaired surrogate");
                        free(buf);
                        return NULL;
                    }
                    unsigned int low;
                    if (!parse_hex4(p, &low) || low < 0xDC00 || low > 0xDFFF) {
                        set_error(p, "invalid low surrogate");
                        free(buf);
                        return NULL;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                if (!append_utf8(&buf, &len, &cap, cp)) {
                    set_error(p, "out of memory");
                    free(buf);
                    return NULL;
                }
                continue;
            }
            default:
                set_error(p, "invalid escape sequence");
                free(buf);
                return NULL;
        }
        if (!buf_push(&buf, &len, &cap, out_c)) {
            set_error(p, "out of memory");
            free(buf);
            return NULL;
        }
    }

    if (!buf_push(&buf, &len, &cap, '\0')) {
        set_error(p, "out of memory");
        free(buf);
        return NULL;
    }
    return buf;
}

static JsonValue *parse_number(JsonParser *p) {
    const size_t start = p->pos;
    if (peek(p) == '-') {
        p->pos++;
    }
    if (peek(p) == '0') {
        p->pos++;
    } else if (isdigit((unsigned char)peek(p))) {
        while (isdigit((unsigned char)peek(p))) {
            p->pos++;
        }
    } else {
        set_error(p, "invalid number");
        return NULL;
    }
    if (peek(p) == '.') {
        p->pos++;
        if (!isdigit((unsigned char)peek(p))) {
            set_error(p, "invalid number");
            return NULL;
        }
        while (isdigit((unsigned char)peek(p))) {
            p->pos++;
        }
    }
    if (peek(p) == 'e' || peek(p) == 'E') {
        p->pos++;
        if (peek(p) == '+' || peek(p) == '-') {
            p->pos++;
        }
        if (!isdigit((unsigned char)peek(p))) {
            set_error(p, "invalid number");
            return NULL;
        }
        while (isdigit((unsigned char)peek(p))) {
            p->pos++;
        }
    }

    const size_t token_len = p->pos - start;
    char number_text[64];
    if (token_len >= sizeof(number_text)) {
        set_error(p, "number too long");
        return NULL;
    }
    memcpy(number_text, p->input + start, token_len);
    number_text[token_len] = '\0';

    JsonValue *value = value_new(JSON_NUMBER);
    if (value == NULL) {
        set_error(p, "out of memory");
        return NULL;
    }
    value->as.number = strtod(number_text, NULL);
    return value;
}

static JsonValue *parse_array(JsonParser *p) {
    JsonValue *value = value_new(JSON_ARRAY);
    if (value == NULL) {
        set_error(p, "out of memory");
        return NULL;
    }
    p->pos++; /* consume '[' */
    skip_whitespace(p);

    if (match(p, ']')) {
        return value;
    }

    size_t capacity = 0;
    for (;;) {
        skip_whitespace(p);
        JsonValue *item = parse_value(p);
        if (item == NULL) {
            json_free(value);
            return NULL;
        }
        if (value->as.array.count == capacity) {
            capacity = (capacity == 0) ? 4 : (capacity * 2);
            JsonValue **grown = realloc(value->as.array.items, capacity * sizeof(JsonValue *));
            if (grown == NULL) {
                set_error(p, "out of memory");
                json_free(item);
                json_free(value);
                return NULL;
            }
            value->as.array.items = grown;
        }
        value->as.array.items[value->as.array.count++] = item;

        skip_whitespace(p);
        if (match(p, ',')) {
            continue;
        }
        if (match(p, ']')) {
            break;
        }
        set_error(p, "expected ',' or ']'");
        json_free(value);
        return NULL;
    }
    return value;
}

static JsonValue *parse_object(JsonParser *p) {
    JsonValue *value = value_new(JSON_OBJECT);
    if (value == NULL) {
        set_error(p, "out of memory");
        return NULL;
    }
    p->pos++; /* consume '{' */
    skip_whitespace(p);

    if (match(p, '}')) {
        return value;
    }

    size_t capacity = 0;
    for (;;) {
        skip_whitespace(p);
        if (peek(p) != '"') {
            set_error(p, "expected string key");
            json_free(value);
            return NULL;
        }
        char *key = parse_string_raw(p);
        if (key == NULL) {
            json_free(value);
            return NULL;
        }

        skip_whitespace(p);
        if (!match(p, ':')) {
            set_error(p, "expected ':'");
            free(key);
            json_free(value);
            return NULL;
        }

        skip_whitespace(p);
        JsonValue *member_value = parse_value(p);
        if (member_value == NULL) {
            free(key);
            json_free(value);
            return NULL;
        }

        if (value->as.object.count == capacity) {
            capacity = (capacity == 0) ? 4 : (capacity * 2);
            JsonMember *grown = realloc(value->as.object.members, capacity * sizeof(JsonMember));
            if (grown == NULL) {
                set_error(p, "out of memory");
                free(key);
                json_free(member_value);
                json_free(value);
                return NULL;
            }
            value->as.object.members = grown;
        }
        value->as.object.members[value->as.object.count].key = key;
        value->as.object.members[value->as.object.count].value = member_value;
        value->as.object.count++;

        skip_whitespace(p);
        if (match(p, ',')) {
            continue;
        }
        if (match(p, '}')) {
            break;
        }
        set_error(p, "expected ',' or '}'");
        json_free(value);
        return NULL;
    }
    return value;
}

static JsonValue *parse_value(JsonParser *p) {
    skip_whitespace(p);
    const char c = peek(p);

    if (c == '{') {
        return parse_object(p);
    }
    if (c == '[') {
        return parse_array(p);
    }
    if (c == '"') {
        char *text = parse_string_raw(p);
        if (text == NULL) {
            return NULL;
        }
        JsonValue *value = value_new(JSON_STRING);
        if (value == NULL) {
            set_error(p, "out of memory");
            free(text);
            return NULL;
        }
        value->as.string = text;
        return value;
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        return parse_number(p);
    }
    if (match_literal(p, "true")) {
        JsonValue *value = value_new(JSON_BOOL);
        if (value == NULL) {
            set_error(p, "out of memory");
            return NULL;
        }
        value->as.boolean = 1;
        return value;
    }
    if (match_literal(p, "false")) {
        JsonValue *value = value_new(JSON_BOOL);
        if (value == NULL) {
            set_error(p, "out of memory");
            return NULL;
        }
        value->as.boolean = 0;
        return value;
    }
    if (match_literal(p, "null")) {
        JsonValue *value = value_new(JSON_NULL);
        if (value == NULL) {
            set_error(p, "out of memory");
        }
        return value;
    }

    set_error(p, "unexpected token");
    return NULL;
}

JsonValue *json_parse(const char *input, char *err, size_t err_size) {
    JsonParser parser = { .input = input, .pos = 0, .len = strlen(input), .error = { 0 } };

    JsonValue *value = parse_value(&parser);
    if (value != NULL) {
        skip_whitespace(&parser);
        if (parser.pos != parser.len) {
            set_error(&parser, "trailing characters after JSON value");
            json_free(value);
            value = NULL;
        }
    }

    if (value == NULL && err != NULL && err_size > 0) {
        snprintf(err, err_size, "%s", parser.error[0] != '\0' ? parser.error : "unknown parse error");
    }
    return value;
}
