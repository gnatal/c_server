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

    /* Overwriting an existing key replaces the value in-place without duplicating keys */
    assert(json_object_set(obj, "name", json_new_string("alice")) == 1);
    assert(strcmp(json_as_string(json_object_get(obj, "name"), ""), "alice") == 0);
    assert(obj->as.object.count == 2);

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

/* Returns json_stringify(value) for a lone number, freed by the caller. */
static char *stringify_number(const double value) {
    JsonValue *v = json_new_number(value);
    char *out = json_stringify(v);
    json_free(v);
    return out;
}

static void test_number_formatting(void) {
    /* Regression: numbers used to go through "%g" (6 significant digits), so 1234567
     * was emitted as 1.23457e+06 - a corrupted id in any API response past 999999. */
    const struct { double in; const char *out; } cases[] = {
        { 0, "0" }, { 42, "42" }, { -5, "-5" }, { 999999, "999999" }, { 1000000, "1000000" },
        { 1234567, "1234567" }, { 33333333, "33333333" }, { 15000000000.0, "15000000000" },
        { 9007199254740991.0, "9007199254740991" }, { 0.5, "0.5" }, { -2.25, "-2.25" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *out = stringify_number(cases[i].in);
        assert(strcmp(out, cases[i].out) == 0);
        free(out);
    }

    /* Non-integers keep full precision (round-trip through parse). */
    char *pi = stringify_number(3.14159265358979);
    JsonValue *back = json_parse(pi, NULL, 0);
    assert(json_as_number(back, 0) == 3.14159265358979);
    json_free(back);
    free(pi);

    /* NaN / infinity have no JSON form. */
    volatile double zero = 0.0;
    char *nan_out = stringify_number(zero / zero);
    assert(strcmp(nan_out, "null") == 0);
    free(nan_out);
    char *inf_out = stringify_number(1.0 / zero);
    assert(strcmp(inf_out, "null") == 0);
    free(inf_out);

    /* parse -> stringify keeps a large integer intact. */
    JsonValue *doc = json_parse("{\"id\":1234567}", NULL, 0);
    char *round = json_stringify(doc);
    assert(strcmp(round, "{\"id\":1234567}") == 0);
    free(round);
    json_free(doc);
}

static void test_writer_document(void) {
    JsonWriter w;
    jw_init(&w);
    jw_object_begin(&w);
    jw_key(&w, "id");    jw_int(&w, 1234567);
    jw_key(&w, "title"); jw_string(&w, "say \"hi\"\n\t\\ \x01");
    jw_key(&w, "done");  jw_bool(&w, 1);
    jw_key(&w, "none");  jw_null(&w);
    jw_key(&w, "missing"); jw_string(&w, NULL);
    jw_key(&w, "ratio"); jw_double(&w, 0.5);
    jw_key(&w, "tags");
    jw_array_begin(&w);
    jw_string(&w, "a");
    jw_int(&w, -7);
    jw_object_begin(&w);
    jw_key(&w, "k"); jw_array_begin(&w); jw_array_end(&w);
    jw_object_end(&w);
    jw_array_end(&w);
    jw_key(&w, "empty"); jw_object_begin(&w); jw_object_end(&w);
    jw_object_end(&w);

    assert(jw_ok(&w));
    assert(strcmp(jw_data(&w),
        "{\"id\":1234567,\"title\":\"say \\\"hi\\\"\\n\\t\\\\ \\u0001\",\"done\":true,\"none\":null,"
        "\"missing\":null,\"ratio\":0.5,\"tags\":[\"a\",-7,{\"k\":[]}],\"empty\":{}}") == 0);
    assert(jw_len(&w) == strlen(jw_data(&w)));

    /* What the writer emits must be valid JSON the parser accepts. */
    JsonValue *parsed = json_parse(jw_data(&w), NULL, 0);
    assert(parsed != NULL);
    assert(json_as_number(json_object_get(parsed, "id"), 0) == 1234567);
    json_free(parsed);

    jw_free(&w);
    jw_free(&w); /* idempotent */
}

static void test_writer_top_level_values(void) {
    JsonWriter w;
    jw_init(&w);
    jw_int(&w, -9223372036854775807LL - 1);
    assert(jw_ok(&w));
    assert(strcmp(jw_data(&w), "-9223372036854775808") == 0);
    jw_free(&w);

    jw_init(&w);
    jw_array_begin(&w);
    jw_array_end(&w);
    assert(strcmp(jw_data(&w), "[]") == 0);
    jw_free(&w);
}

static void test_writer_misuse_fails_cleanly(void) {
    JsonWriter w;

    /* value inside an object with no key */
    jw_init(&w); jw_object_begin(&w); jw_int(&w, 1); jw_object_end(&w);
    assert(!jw_ok(&w)); assert(jw_data(&w) == NULL); assert(jw_len(&w) == 0);
    jw_free(&w);

    /* key outside an object */
    jw_init(&w); jw_array_begin(&w); jw_key(&w, "k"); jw_array_end(&w);
    assert(!jw_ok(&w)); jw_free(&w);
    jw_init(&w); jw_key(&w, "k");
    assert(!jw_ok(&w)); jw_free(&w);

    /* key with no value */
    jw_init(&w); jw_object_begin(&w); jw_key(&w, "k"); jw_object_end(&w);
    assert(!jw_ok(&w)); jw_free(&w);

    /* two keys in a row */
    jw_init(&w); jw_object_begin(&w); jw_key(&w, "a"); jw_key(&w, "b");
    assert(!jw_ok(&w)); jw_free(&w);

    /* mismatched / unbalanced ends */
    jw_init(&w); jw_object_begin(&w); jw_array_end(&w);
    assert(!jw_ok(&w)); jw_free(&w);
    jw_init(&w); jw_object_end(&w);
    assert(!jw_ok(&w)); jw_free(&w);

    /* incomplete document is not ok; nothing is exposed */
    jw_init(&w); jw_object_begin(&w);
    assert(!jw_ok(&w)); assert(jw_data(&w) == NULL); jw_free(&w);

    /* a second top-level value */
    jw_init(&w); jw_int(&w, 1); jw_int(&w, 2);
    assert(!jw_ok(&w)); jw_free(&w);

    /* failure is sticky: later valid calls cannot resurrect the document */
    jw_init(&w); jw_key(&w, "bad"); jw_int(&w, 1);
    assert(!jw_ok(&w)); jw_free(&w);

    /* nesting past JSON_WRITER_MAX_DEPTH fails instead of overflowing kind[] */
    jw_init(&w);
    for (int i = 0; i < JSON_WRITER_MAX_DEPTH + 1; i++) jw_array_begin(&w);
    assert(!jw_ok(&w)); jw_free(&w);
    jw_init(&w);
    for (int i = 0; i < JSON_WRITER_MAX_DEPTH; i++) jw_array_begin(&w);
    for (int i = 0; i < JSON_WRITER_MAX_DEPTH; i++) jw_array_end(&w);
    assert(jw_ok(&w)); jw_free(&w);
}

static void test_writer_matches_tree_serializer(void) {
    /* The same document through both emitters yields identical bytes. */
    JsonValue *obj = json_new_object();
    json_object_set(obj, "id", json_new_number(1234567));
    json_object_set(obj, "title", json_new_string("a \"q\" \n b"));
    json_object_set(obj, "done", json_new_bool(0));
    char *tree_out = json_stringify(obj);

    JsonWriter w;
    jw_init(&w);
    jw_object_begin(&w);
    jw_key(&w, "id");    jw_int(&w, 1234567);
    jw_key(&w, "title"); jw_string(&w, "a \"q\" \n b");
    jw_key(&w, "done");  jw_bool(&w, 0);
    jw_object_end(&w);
    assert(strcmp(tree_out, jw_data(&w)) == 0);

    jw_free(&w);
    free(tree_out);
    json_free(obj);
}

int main(void) {
    test_primitives();
    test_nested_structure();
    test_round_trip();
    test_builders();
    test_number_bool_array_builders();
    test_syntax_errors();
    test_number_formatting();
    test_writer_document();
    test_writer_top_level_values();
    test_writer_misuse_fails_cleanly();
    test_writer_matches_tree_serializer();
    printf("all json tests passed\n");
    return 0;
}
