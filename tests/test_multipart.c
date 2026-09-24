#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_types.h"
#include "multipart.h"

static void test_multipart_parse_boundary(void) {
    char boundary[MAX_BOUNDARY_LEN + 1];

    /* Standard, unquoted boundary. */
    assert(multipart_parse_boundary("multipart/form-data; boundary=----WebKit123", boundary, sizeof(boundary)) == 1);
    assert(strcmp(boundary, "----WebKit123") == 0);

    /* Double-quoted boundary (RFC 2046 permits either form). */
    assert(multipart_parse_boundary("multipart/form-data; boundary=\"abc def\"", boundary, sizeof(boundary)) == 1);
    assert(strcmp(boundary, "abc def") == 0);

    /* A ";charset=..."-style trailing parameter after boundary doesn't break it. */
    assert(multipart_parse_boundary("multipart/form-data; boundary=xyz; charset=utf-8", boundary, sizeof(boundary)) == 1);
    assert(strcmp(boundary, "xyz") == 0);

    /* Case-insensitive media type match, same convention as has_json_content_type. */
    assert(multipart_parse_boundary("MULTIPART/FORM-DATA; boundary=xyz", boundary, sizeof(boundary)) == 1);
    assert(strcmp(boundary, "xyz") == 0);

    /* Not multipart/form-data at all. */
    assert(multipart_parse_boundary("application/json", boundary, sizeof(boundary)) == 0);
    assert(multipart_parse_boundary("multipart/mixed; boundary=xyz", boundary, sizeof(boundary)) == 0);

    /* multipart/form-data with no boundary parameter. */
    assert(multipart_parse_boundary("multipart/form-data", boundary, sizeof(boundary)) == 0);

    /* NULL content type. */
    assert(multipart_parse_boundary(NULL, boundary, sizeof(boundary)) == 0);

    /* Boundary too long to fit the destination. */
    char tiny[4];
    assert(multipart_parse_boundary("multipart/form-data; boundary=abcdefgh", tiny, sizeof(tiny)) == 0);
}

static void test_parse_multipart_body_fields_and_file(void) {
    const char *body =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"username\"\r\n"
        "\r\n"
        "natal\r\n"
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"avatar\"; filename=\"pic.png\"\r\n"
        "Content-Type: image/png\r\n"
        "\r\n"
        "not-really-png-bytes\r\n"
        "--BOUNDARY--\r\n";

    MultipartForm form;
    int count = parse_multipart_body(body, strlen(body), "BOUNDARY", &form);
    assert(count == 2);
    assert(form.part_count == 2);

    const MultipartPart *username = multipart_get_part(&form, "username");
    assert(username != NULL);
    assert(strcmp(username->filename, "") == 0);
    assert(strcmp(username->content_type, "") == 0);
    assert(username->data_len == strlen("natal"));
    assert(memcmp(username->data, "natal", username->data_len) == 0);

    const MultipartPart *avatar = multipart_get_part(&form, "avatar");
    assert(avatar != NULL);
    assert(strcmp(avatar->filename, "pic.png") == 0);
    assert(strcmp(avatar->content_type, "image/png") == 0);
    assert(avatar->data_len == strlen("not-really-png-bytes"));
    assert(memcmp(avatar->data, "not-really-png-bytes", avatar->data_len) == 0);

    assert(multipart_get_part(&form, "missing") == NULL);
}

static void test_parse_multipart_body_binary_with_embedded_nul(void) {
    /* A file part's payload can be arbitrary bytes, including embedded
     * NULs - data/data_len must carry it in full, never truncated at the
     * first NUL the way treating it as a C string would. */
    const char prefix[] =
        "--B\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"a.bin\"\r\n"
        "\r\n";
    const char payload[6] = { 'a', '\0', 'b', 'c', '\0', 'd' };
    const char suffix[] = "\r\n--B--\r\n";

    char body[sizeof(prefix) - 1 + sizeof(payload) + sizeof(suffix) - 1];
    size_t offset = 0;
    memcpy(body + offset, prefix, sizeof(prefix) - 1);
    offset += sizeof(prefix) - 1;
    memcpy(body + offset, payload, sizeof(payload));
    offset += sizeof(payload);
    memcpy(body + offset, suffix, sizeof(suffix) - 1);
    offset += sizeof(suffix) - 1;

    MultipartForm form;
    assert(parse_multipart_body(body, offset, "B", &form) == 1);
    const MultipartPart *file = multipart_get_part(&form, "file");
    assert(file != NULL);
    assert(file->data_len == sizeof(payload));
    assert(memcmp(file->data, payload, sizeof(payload)) == 0);
}

static void test_parse_multipart_body_edge_cases(void) {
    MultipartForm form;

    /* NULL/empty boundary is rejected outright. */
    assert(parse_multipart_body("--B\r\n\r\n--B--\r\n", 14, NULL, &form) == -1);
    assert(parse_multipart_body("--B\r\n\r\n--B--\r\n", 14, "", &form) == -1);

    /* Body doesn't contain the given boundary at all -> zero parts, not an error. */
    const char *no_match = "just some text, not multipart at all";
    assert(parse_multipart_body(no_match, strlen(no_match), "BOUNDARY", &form) == 0);
    assert(form.part_count == 0);

    /* A part with no Content-Disposition name= is skipped (malformed - no
     * field to key it by), but well-formed parts around it still parse. */
    const char *missing_name =
        "--B\r\n"
        "Content-Disposition: form-data\r\n"
        "\r\n"
        "orphaned\r\n"
        "--B\r\n"
        "Content-Disposition: form-data; name=\"ok\"\r\n"
        "\r\n"
        "value\r\n"
        "--B--\r\n";
    assert(parse_multipart_body(missing_name, strlen(missing_name), "B", &form) == 1);
    assert(multipart_get_part(&form, "ok") != NULL);

    /* Truncation: parts past MAX_MULTIPART_PARTS are dropped, not overflowed. */
    char many[8192] = "";
    for (int i = 0; i < MAX_MULTIPART_PARTS + 5; i++) {
        char part[128];
        snprintf(part, sizeof(part), "--B\r\nContent-Disposition: form-data; name=\"f%d\"\r\n\r\nv\r\n", i);
        strncat(many, part, sizeof(many) - strlen(many) - 1);
    }
    strncat(many, "--B--\r\n", sizeof(many) - strlen(many) - 1);
    assert(parse_multipart_body(many, strlen(many), "B", &form) == MAX_MULTIPART_PARTS);
    assert(form.part_count == MAX_MULTIPART_PARTS);
}

/* One part with the given Content-Disposition line, parsed; returns the part or NULL. */
static const MultipartPart *parse_one(const char *disposition, MultipartForm *form) {
    char body[1024];
    const int n = snprintf(body, sizeof(body), "--B\r\n%s\r\n\r\nv\r\n--B--\r\n", disposition);
    assert(n > 0 && (size_t)n < sizeof(body));
    parse_multipart_body(body, (size_t)n, "B", form);
    return form->part_count == 1 ? &form->parts[0] : NULL;
}

/* Parameters are a ";"-separated list, not substrings: "filename=" inside the quoted name is part of
 * the name, and a quoted value runs to its own closing quote (with \" escapes). */
static void test_disposition_params_are_parsed_not_searched(void) {
    MultipartForm form;
    const MultipartPart *p = parse_one("Content-Disposition: form-data; name=\"avatar; filename=../../etc/cron.d/x\"", &form);
    assert(p != NULL);
    assert(strcmp(p->name, "avatar; filename=../../etc/cron.d/x") == 0);
    assert(p->filename[0] == '\0');

    p = parse_one("Content-Disposition: form-data; name=\"f\"; filename=\"a \\\"q\\\".txt\"", &form);
    assert(p != NULL && strcmp(p->filename, "a \"q\".txt") == 0);

    p = parse_one("Content-Disposition: form-data; filename=\"x.txt\"; name=f", &form); /* order, token value */
    assert(p != NULL && strcmp(p->name, "f") == 0 && strcmp(p->filename, "x.txt") == 0);

    p = parse_one("content-disposition: form-data ; NAME = \"f\" ; FileName=\"C:\\Users\\me\\a.txt\"", &form);
    assert(p != NULL && strcmp(p->name, "f") == 0);
    assert(strcmp(p->filename, "C:\\Users\\me\\a.txt") == 0); /* unescaped backslashes are kept */

    p = parse_one("Content-Disposition: form-data; name=\"f\"; filename*=UTF-8''x.txt", &form);
    assert(p != NULL && p->filename[0] == '\0'); /* filename* is not filename */

    p = parse_one("Content-Disposition: form-data; xname=\"a\"; name=\"b\"", &form);
    assert(p != NULL && strcmp(p->name, "b") == 0);

    /* Malformed: unterminated quote, junk after a value, a control character. No name, so skipped. */
    assert(parse_one("Content-Disposition: form-data; name=\"f", &form) == NULL);
    assert(parse_one("Content-Disposition: form-data; name=\"f\"x", &form) == NULL);
    assert(parse_one("Content-Disposition: form-data; name=\"a\x01" "b\"", &form) == NULL);

    /* Header names are matched at a line start only, not inside another header's value. */
    p = parse_one("X-Note: Content-Disposition: form-data; name=\"evil\"\r\n"
                  "Content-Disposition: form-data; name=\"real\"", &form);
    assert(p != NULL && strcmp(p->name, "real") == 0);
    p = parse_one("Content-Disposition: form-data; name=\"f\"\r\nX-Note: Content-Type: text/evil\r\n"
                  "Content-Type: text/plain", &form);
    assert(p != NULL && strcmp(p->content_type, "text/plain") == 0);
}

static void test_safe_filename(void) {
    MultipartPart part;
    char out[64];
    const struct { const char *in; int ok; const char *out; } cases[] = {
        {"a.txt", 1, "a.txt"},
        {"../../../root/.ssh/authorized_keys", 1, "authorized_keys"},
        {"C:\\Users\\me\\report.pdf", 1, "report.pdf"},
        {"/etc/passwd", 1, "passwd"},
        {"a\x01" "b\x7f.txt", 1, "ab.txt"},
        {"", 0, ""},
        {".", 0, ""},
        {"..", 0, ""},
        {"dir/..", 0, ""},
        {"dir/", 0, ""},
        {"x\\..", 0, ""},
        {"\x01\x02", 0, ""},
        {".hidden", 1, ".hidden"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&part, 0, sizeof(part));
        strncpy(part.filename, cases[i].in, sizeof(part.filename) - 1);
        assert(multipart_safe_filename(&part, out, sizeof(out)) == cases[i].ok);
        assert(strcmp(out, cases[i].out) == 0);
    }
    memset(&part, 0, sizeof(part));
    strncpy(part.filename, "a-name-longer-than-eight.txt", sizeof(part.filename) - 1);
    char tiny[8];
    assert(multipart_safe_filename(&part, tiny, sizeof(tiny)) == 0);
}

int main(void) {
    test_multipart_parse_boundary();
    test_parse_multipart_body_fields_and_file();
    test_parse_multipart_body_binary_with_embedded_nul();
    test_parse_multipart_body_edge_cases();
    test_disposition_params_are_parsed_not_searched();
    test_safe_filename();
    printf("all multipart tests passed\n");
    return 0;
}
