#include <stdio.h>
#include <string.h>
#include "multipart.h"

/*
 * Finds "param_name=" within header_value (case-insensitive on the name
 * itself, per RFC 2045 parameter-name matching) and copies its value into
 * out: everything up to the matching closing '"' when the value is
 * double-quoted, or up to the next ';' (or end of string) otherwise.
 * Requires the match to be a real parameter occurrence - preceded by the
 * start of the string, ';', or a space, not just a substring inside a
 * longer name or value (e.g. looking up "name" must not match inside
 * "filename=") - so it re-scans past a rejected candidate rather than
 * stopping there. Returns 1 on success, 0 if param_name isn't present or
 * its value doesn't fit in out_size (out is left untouched on failure).
 */
static int extract_param(const char *header_value, const char *param_name, char *out, size_t out_size) {
    if (header_value == NULL || param_name == NULL || out_size == 0) {
        return 0;
    }

    const size_t name_len = strlen(param_name);
    const char *cursor = header_value;
    while ((cursor = strcasestr(cursor, param_name)) != NULL) {
        const char *after_name = cursor + name_len;
        const int left_ok = (cursor == header_value) || cursor[-1] == ';' || cursor[-1] == ' ';
        if (left_ok && *after_name == '=') {
            const char *value = after_name + 1;
            const char *end;
            if (*value == '"') {
                value++;
                end = strchr(value, '"');
                if (end == NULL) {
                    return 0;
                }
            } else {
                end = value;
                while (*end != '\0' && *end != ';') {
                    end++;
                }
            }

            const size_t len = (size_t)(end - value);
            if (len >= out_size) {
                return 0;
            }
            memcpy(out, value, len);
            out[len] = '\0';
            return 1;
        }
        cursor = after_name;
    }
    return 0;
}

int multipart_parse_boundary(const char *content_type, char *boundary_out, size_t boundary_out_size) {
    static const char prefix[] = "multipart/form-data";
    if (content_type == NULL || strncasecmp(content_type, prefix, strlen(prefix)) != 0) {
        return 0;
    }
    return extract_param(content_type, "boundary", boundary_out, boundary_out_size);
}

/*
 * Fills in part->name/filename/content_type from one part's raw header
 * block (everything between the boundary line and the blank-line
 * separator, header_block_len bytes, not NUL-terminated). Unlike a part's
 * body, a header block is genuinely textual and short - safe to copy into
 * a bounded stack buffer and treat as a C string; a block that doesn't fit
 * is truncated (Content-Disposition is always the first header a real
 * client sends, so truncation would only ever lose an already-unusual
 * trailing header).
 */
static void parse_part_headers(const char *header_block, size_t header_block_len, MultipartPart *part) {
    char buf[512];
    size_t copy_len = header_block_len < sizeof(buf) - 1 ? header_block_len : sizeof(buf) - 1;
    memcpy(buf, header_block, copy_len);
    buf[copy_len] = '\0';

    const char *disposition = strcasestr(buf, "Content-Disposition:");
    if (disposition != NULL) {
        disposition += strlen("Content-Disposition:");
        extract_param(disposition, "name", part->name, sizeof(part->name));
        extract_param(disposition, "filename", part->filename, sizeof(part->filename));
    }

    const char *type_header = strcasestr(buf, "Content-Type:");
    if (type_header != NULL) {
        type_header += strlen("Content-Type:");
        while (*type_header == ' ') {
            type_header++;
        }
        const char *line_end = strstr(type_header, "\r\n");
        size_t len = line_end != NULL ? (size_t)(line_end - type_header) : strlen(type_header);
        if (len >= sizeof(part->content_type)) {
            len = sizeof(part->content_type) - 1;
        }
        memcpy(part->content_type, type_header, len);
        part->content_type[len] = '\0';
    }
}

int parse_multipart_body(const char *body, size_t body_len, const char *boundary, MultipartForm *form) {
    form->part_count = 0;
    if (boundary == NULL || boundary[0] == '\0') {
        return -1;
    }

    char dash_boundary[MAX_BOUNDARY_LEN + 2];
    int written = snprintf(dash_boundary, sizeof(dash_boundary), "--%s", boundary);
    if (written < 0 || (size_t)written >= sizeof(dash_boundary)) {
        return -1;
    }
    const size_t dash_len = (size_t)written;

    char crlf_dash_boundary[MAX_BOUNDARY_LEN + 4];
    written = snprintf(crlf_dash_boundary, sizeof(crlf_dash_boundary), "\r\n--%s", boundary);
    if (written < 0 || (size_t)written >= sizeof(crlf_dash_boundary)) {
        return -1;
    }
    const size_t crlf_dash_len = (size_t)written;

    const void *first = memmem(body, body_len, dash_boundary, dash_len);
    if (first == NULL) {
        return 0;
    }
    size_t pos = (size_t)((const char *)first - body) + dash_len;

    while (1) {
        if (pos + 2 <= body_len && body[pos] == '-' && body[pos + 1] == '-') {
            break; /* terminal "--boundary--" delimiter: end of the body */
        }
        if (pos + 2 > body_len || body[pos] != '\r' || body[pos + 1] != '\n') {
            break; /* malformed boundary line - stop rather than misparse */
        }
        if (form->part_count >= MAX_MULTIPART_PARTS) {
            fprintf(stderr, "parse_multipart_body: MAX_MULTIPART_PARTS exceeded, truncating\n");
            break;
        }
        pos += 2; /* skip the boundary line's CRLF - part headers start here */

        const void *next = memmem(body + pos, body_len - pos, crlf_dash_boundary, crlf_dash_len);
        if (next == NULL) {
            break; /* no closing boundary for this part - malformed */
        }
        const size_t part_end = (size_t)((const char *)next - body);

        const void *sep = memmem(body + pos, part_end - pos, "\r\n\r\n", 4);
        if (sep != NULL) {
            const size_t header_len = (size_t)((const char *)sep - (body + pos));
            const size_t content_start = pos + header_len + 4;

            MultipartPart *part = &form->parts[form->part_count];
            memset(part, 0, sizeof(*part));
            parse_part_headers(body + pos, header_len, part);
            if (part->name[0] != '\0') {
                part->data = body + content_start;
                part->data_len = part_end - content_start;
                form->part_count++;
            }
        }
        /* A part with no header/body separator is silently skipped, same as
         * parse_headers (http_parser.c) skipping a header line with no ':'. */

        pos = part_end + 2 + dash_len; /* past this delimiter's CRLF + "--boundary" */
    }

    return form->part_count;
}

const MultipartPart *multipart_get_part(const MultipartForm *form, const char *name) {
    for (int i = 0; i < form->part_count; i++) {
        if (strcmp(form->parts[i].name, name) == 0) {
            return &form->parts[i];
        }
    }
    return NULL;
}
