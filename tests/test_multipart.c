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

int main(void) {
    test_multipart_parse_boundary();
    test_parse_multipart_body_fields_and_file();
    test_parse_multipart_body_binary_with_embedded_nul();
    test_parse_multipart_body_edge_cases();
    printf("all multipart tests passed\n");
    return 0;
}
