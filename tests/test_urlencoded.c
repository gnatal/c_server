#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_types.h"
#include "urlencoded.h"

static void test_parse_urlencoded_body_basic(void) {
    const char *body = "name=natal&age=32";
    UrlEncodedForm form;
    parse_urlencoded_body(body, strlen(body), &form);

    assert(form.field_count == 2);
    assert(strcmp(urlencoded_get_field(&form, "name"), "natal") == 0);
    assert(strcmp(urlencoded_get_field(&form, "age"), "32") == 0);
    assert(urlencoded_get_field(&form, "missing") == NULL);
}

static void test_parse_urlencoded_body_decodes_percent_and_plus(void) {
    /* '+' decodes to a space and %XX escapes decode to the literal byte -
     * the application/x-www-form-urlencoded convention, same as
     * parse_query_string (lib/http_parser.c). */
    const char *body = "full+name=Natal%20Silva&note=a%2Bb";
    UrlEncodedForm form;
    parse_urlencoded_body(body, strlen(body), &form);

    assert(form.field_count == 2);
    assert(strcmp(urlencoded_get_field(&form, "full name"), "Natal Silva") == 0);
    assert(strcmp(urlencoded_get_field(&form, "note"), "a+b") == 0);
}

static void test_parse_urlencoded_body_bare_key_gets_empty_value(void) {
    const char *body = "flag&name=natal";
    UrlEncodedForm form;
    parse_urlencoded_body(body, strlen(body), &form);

    assert(form.field_count == 2);
    assert(strcmp(urlencoded_get_field(&form, "flag"), "") == 0);
    assert(strcmp(urlencoded_get_field(&form, "name"), "natal") == 0);
}

static void test_parse_urlencoded_body_skips_empty_pairs(void) {
    /* Consecutive '&'s (leading/trailing/doubled) don't produce empty
     * field entries, same as parse_query_string skipping repeated '&'. */
    const char *body = "&a=1&&b=2&";
    UrlEncodedForm form;
    parse_urlencoded_body(body, strlen(body), &form);

    assert(form.field_count == 2);
    assert(strcmp(urlencoded_get_field(&form, "a"), "1") == 0);
    assert(strcmp(urlencoded_get_field(&form, "b"), "2") == 0);
}

static void test_parse_urlencoded_body_empty_or_null(void) {
    UrlEncodedForm form;

    parse_urlencoded_body("", 0, &form);
    assert(form.field_count == 0);

    parse_urlencoded_body(NULL, 0, &form);
    assert(form.field_count == 0);
}

static void test_parse_urlencoded_body_truncation(void) {
    /* Pairs past MAX_FORM_FIELDS are dropped, not overflowed. */
    char many[4096] = "";
    for (int i = 0; i < MAX_FORM_FIELDS + 5; i++) {
        char pair[32];
        snprintf(pair, sizeof(pair), "%sf%d=v", i == 0 ? "" : "&", i);
        strncat(many, pair, sizeof(many) - strlen(many) - 1);
    }

    UrlEncodedForm form;
    parse_urlencoded_body(many, strlen(many), &form);
    assert(form.field_count == MAX_FORM_FIELDS);
}

int main(void) {
    test_parse_urlencoded_body_basic();
    test_parse_urlencoded_body_decodes_percent_and_plus();
    test_parse_urlencoded_body_bare_key_gets_empty_value();
    test_parse_urlencoded_body_skips_empty_pairs();
    test_parse_urlencoded_body_empty_or_null();
    test_parse_urlencoded_body_truncation();
    printf("all urlencoded tests passed\n");
    return 0;
}
