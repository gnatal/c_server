#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json/json.h"

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

static void test_builders(void) {
    JsonValue *obj = json_new_object();
    assert(obj != NULL);

    assert(json_object_set(obj, "name", json_new_string("natal")) == 1);
    assert(json_object_set(obj, "quote", json_new_string("say \"hi\"")) == 1);

    assert(strcmp(json_as_string(json_object_get(obj, "name"), ""), "natal") == 0);
    assert(strcmp(json_as_string(json_object_get(obj, "quote"), ""), "say \"hi\"") == 0);

    /* Round-trips through json_stringify with correct escaping - a raw '"'
     * in the value doesn't corrupt the surrounding JSON syntax. */
    char *out = json_stringify(obj);
    assert(out != NULL);
    assert(strstr(out, "\"quote\":\"say \\\"hi\\\"\"") != NULL);

    JsonValue *reparsed = json_parse(out, NULL, 0);
    assert(reparsed != NULL);
    assert(strcmp(json_as_string(json_object_get(reparsed, "quote"), ""), "say \"hi\"") == 0);

    free(out);
    json_free(reparsed);
    json_free(obj);

    /* json_new_string copies its input - mutating the caller's buffer
     * afterward doesn't affect the JsonValue. */
    char scratch[16];
    strncpy(scratch, "original", sizeof(scratch) - 1);
    scratch[sizeof(scratch) - 1] = '\0';
    JsonValue *copied = json_new_string(scratch);
    strncpy(scratch, "mutated!", sizeof(scratch) - 1);
    assert(strcmp(json_as_string(copied, ""), "original") == 0);
    json_free(copied);

    /* json_object_set rejects a non-object target and still frees value
     * (no leak, verified via ASan/valgrind in CI rather than an assertion
     * here) rather than silently doing nothing. */
    JsonValue *not_an_object = json_parse("42", NULL, 0);
    assert(json_object_set(not_an_object, "x", json_new_string("y")) == 0);
    json_free(not_an_object);
}

static void test_number_bool_array_builders(void) {
    JsonValue *arr = json_new_array();
    assert(arr != NULL);
    assert(json_array_append(arr, json_new_number(1.5)) == 1);
    assert(json_array_append(arr, json_new_bool(1)) == 1);
    assert(json_array_append(arr, json_new_bool(0)) == 1);

    assert(json_array_count(arr) == 3);
    assert(json_as_number(json_array_get(arr, 0), 0) == 1.5);
    assert(json_as_bool(json_array_get(arr, 1), 0) == 1);
    assert(json_as_bool(json_array_get(arr, 2), 1) == 0);

    JsonValue *obj = json_new_object();
    assert(json_object_set(obj, "id", json_new_number(42)) == 1);
    assert(json_object_set(obj, "done", json_new_bool(1)) == 1);
    assert(json_object_set(obj, "items", arr) == 1);

    /* Round-trips through json_stringify/json_parse, same as the string
     * builders in test_builders above. */
    char *out = json_stringify(obj);
    assert(out != NULL);

    JsonValue *reparsed = json_parse(out, NULL, 0);
    assert(reparsed != NULL);
    assert(json_as_number(json_object_get(reparsed, "id"), 0) == 42.0);
    assert(json_as_bool(json_object_get(reparsed, "done"), 0) == 1);
    assert(json_array_count(json_object_get(reparsed, "items")) == 3);

    free(out);
    json_free(reparsed);
    json_free(obj);

    /* json_array_append rejects a non-array target and still frees value
     * (no leak - verified via ASan/valgrind in CI), same convention as
     * json_object_set rejecting a non-object target above. */
    JsonValue *not_an_array = json_parse("42", NULL, 0);
    assert(json_array_append(not_an_array, json_new_number(1)) == 0);
    json_free(not_an_array);
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
    test_builders();
    test_number_bool_array_builders();
    test_syntax_errors();
    printf("all json tests passed\n");
    return 0;
}
