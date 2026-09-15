#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

static void test_primitives(void) {
    JsonValue *n = json_parse("null", NULL, 0);
    assert(json_is_null(n));
    json_free(n);

    JsonValue *t = json_parse("true", NULL, 0);
    assert(json_as_bool(t, 0) == 1);
    json_free(t);

    JsonValue *num = json_parse("  -12.5e2  ", NULL, 0);
    assert(json_as_number(num, 0) == -1250.0);
    json_free(num);

    JsonValue *str = json_parse("\"hi\\n\\u00e9\"", NULL, 0);
    assert(strcmp(json_as_string(str, ""), "hi\n\xc3\xa9") == 0);
    json_free(str);
}

static void test_nested_structure(void) {
    const char *input =
        "{"
        "  \"name\": \"Ada\","
        "  \"active\": true,"
        "  \"tags\": [\"a\", \"b\", \"c\"],"
        "  \"meta\": {\"age\": 36, \"note\": null}"
        "}";

    char err[128];
    JsonValue *root = json_parse(input, err, sizeof(err));
    assert(root != NULL);

    assert(strcmp(json_as_string(json_object_get(root, "name"), ""), "Ada") == 0);
    assert(json_as_bool(json_object_get(root, "active"), 0) == 1);

    const JsonValue *tags = json_object_get(root, "tags");
    assert(json_array_count(tags) == 3);
    assert(strcmp(json_as_string(json_array_get(tags, 1), ""), "b") == 0);

    const JsonValue *meta = json_object_get(root, "meta");
    assert(json_as_number(json_object_get(meta, "age"), 0) == 36.0);
    assert(json_is_null(json_object_get(meta, "note")));

    json_free(root);
}

static void test_round_trip(void) {
    const char *input = "{\"a\":[1,2,3],\"b\":\"x\\\"y\"}";
    JsonValue *root = json_parse(input, NULL, 0);
    assert(root != NULL);

    char *out = json_stringify(root);
    assert(out != NULL);

    JsonValue *reparsed = json_parse(out, NULL, 0);
    assert(reparsed != NULL);
    assert(json_as_number(json_array_get(json_object_get(reparsed, "a"), 2), 0) == 3.0);
    assert(strcmp(json_as_string(json_object_get(reparsed, "b"), ""), "x\"y") == 0);

    free(out);
    json_free(root);
    json_free(reparsed);
}

static void test_syntax_errors(void) {
    char err[128];

    assert(json_parse("{\"a\":}", err, sizeof(err)) == NULL);
    assert(json_parse("[1,2,]", err, sizeof(err)) == NULL);
    assert(json_parse("nul", err, sizeof(err)) == NULL);
    assert(json_parse("\"unterminated", err, sizeof(err)) == NULL);
    assert(json_parse("{} garbage", err, sizeof(err)) == NULL);
}

int main(void) {
    test_primitives();
    test_nested_structure();
    test_round_trip();
    test_syntax_errors();
    printf("all json tests passed\n");
    return 0;
}
