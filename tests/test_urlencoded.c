#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_types.h"
#include "urlencoded.h"
#include "http_parser.h"

static void test_parse_urlencoded_body_basic(void) {
    const char *body = "name=natal&age=32";
    UrlEncodedForm form;
    assert(parse_urlencoded_body(body, strlen(body), &form) == 2);

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

    assert(parse_urlencoded_body("", 0, &form) == 0);
    assert(form.field_count == 0);

    assert(parse_urlencoded_body(NULL, 0, &form) == 0);
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
    assert(parse_urlencoded_body(many, strlen(many), &form) == MAX_FORM_FIELDS);
    assert(form.field_count == MAX_FORM_FIELDS);
}

static void test_parse_urlencoded_body_rejects_embedded_nul(void) {
    /* "%00" used to decode into a NUL that cut the value: file=shell.php%00.png read as "shell.php". */
    const char *bodies[] = {"file=shell.php%00.png", "a=1&file%00x=2", "a=1&b=%00", "x=a%00"};
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        UrlEncodedForm form;
        assert(parse_urlencoded_body(bodies[i], strlen(bodies[i]), &form) == -1);
        assert(form.field_count == 0);
        assert(urlencoded_get_field(&form, "file") == NULL);
    }
    /* a raw NUL byte inside the body span is rejected the same way */
    const char raw[] = {'f', '=', 'a', '\0', 'b'};
    UrlEncodedForm form;
    assert(parse_urlencoded_body(raw, sizeof(raw), &form) == -1);
    assert(form.field_count == 0);
    /* other escapes are unaffected */
    assert(parse_urlencoded_body("f=a%01b", 7, &form) == 1);
    assert(strcmp(urlencoded_get_field(&form, "f"), "a\x01" "b") == 0);
}

static void test_parse_urlencoded_body_rejects_overlong_fields(void) {
    UrlEncodedForm form;
    char body[1024];

    /* value of exactly 255 decoded bytes fits; 256 is rejected instead of being cut to 255 */
    memcpy(body, "v=", 2);
    memset(body + 2, 'a', 255);
    assert(parse_urlencoded_body(body, 2 + 255, &form) == 1);
    assert(strlen(urlencoded_get_field(&form, "v")) == 255);
    memset(body + 2, 'a', 256);
    assert(parse_urlencoded_body(body, 2 + 256, &form) == -2);
    assert(form.field_count == 0);

    /* the limit applies to the decoded length: 255 "%41" escapes (765 raw bytes) fit */
    size_t n = 2;
    for (int i = 0; i < 255; i++) {
        memcpy(body + n, "%41", 3);
        n += 3;
    }
    assert(parse_urlencoded_body(body, n, &form) == 1);
    const char *v = urlencoded_get_field(&form, "v");
    assert(strlen(v) == 255 && v[0] == 'A' && v[254] == 'A');

    /* name: 63 decoded bytes fit, 64 are rejected */
    memset(body, 'n', 63);
    memcpy(body + 63, "=1", 2);
    assert(parse_urlencoded_body(body, 65, &form) == 1);
    memset(body, 'n', 64);
    memcpy(body + 64, "=1", 2);
    assert(parse_urlencoded_body(body, 66, &form) == -2);
    assert(form.field_count == 0);

    /* an overlong field after valid ones still rejects the whole form */
    n = (size_t)snprintf(body, sizeof(body), "ok=1&v=");
    memset(body + n, 'b', 300);
    assert(parse_urlencoded_body(body, n + 300, &form) == -2);
    assert(form.field_count == 0);
    assert(urlencoded_get_field(&form, "ok") == NULL);
}

static void test_url_decode_span(void) {
    char dst[4];
    /* reads exactly src_len bytes: the "zz" past it is never seen */
    assert(url_decode_span("a+bzz", 3, dst, sizeof(dst), 1) == 0);
    assert(strcmp(dst, "a b") == 0);
    assert(url_decode_span("%41%42%43", 9, dst, sizeof(dst), 0) == 0);
    assert(strcmp(dst, "ABC") == 0);
    assert(url_decode_span("abcd", 4, dst, sizeof(dst), 0) == -2);
    assert(strcmp(dst, "abc") == 0);
    assert(url_decode_span("a%00", 4, dst, sizeof(dst), 0) == -1);
    assert(url_decode_span("", 0, dst, sizeof(dst), 0) == 0);
    assert(dst[0] == '\0');
}

int main(void) {
    test_parse_urlencoded_body_basic();
    test_parse_urlencoded_body_decodes_percent_and_plus();
    test_parse_urlencoded_body_bare_key_gets_empty_value();
    test_parse_urlencoded_body_skips_empty_pairs();
    test_parse_urlencoded_body_empty_or_null();
    test_parse_urlencoded_body_truncation();
    test_parse_urlencoded_body_rejects_embedded_nul();
    test_parse_urlencoded_body_rejects_overlong_fields();
    test_url_decode_span();
    printf("all urlencoded tests passed\n");
    return 0;
}
