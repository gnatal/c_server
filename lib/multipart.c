#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "multipart.h"

static int is_ows(const char c) {
    return c == ' ' || c == '\t';
}

/* RFC 9110 tchar: the characters a parameter name or an unquoted value may hold. */
static int is_tchar(const unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

/*
 * Looks up parameter param_name in v[0..len), a header value of the form
 * "<type> *( OWS ";" OWS name "=" value )" (Content-Type's media type, or
 * Content-Disposition's "form-data"), and copies its value into out.
 * Parsed left to right as parameters, never searched as text: a value is a
 * token, or a quoted-string running to its closing '"', so "name=" or
 * "filename=" inside a quoted value is part of that value, never a
 * parameter of its own. Names match case-insensitively and exactly
 * ("name" is not "filename"); the first occurrence wins. In a quoted value,
 * \" and \\ are escapes; any other backslash is kept (browsers send
 * Windows paths unescaped, and multipart_safe_filename splits on it).
 * Returns 1 on success; 0 if the parameter is absent, its value doesn't fit
 * out_size or holds a control character, or the parameter list is
 * malformed at or before it (out is left untouched on failure).
 */
static int extract_param(const char *v, const size_t len, const char *param_name, char *out, const size_t out_size) {
    if (v == NULL || param_name == NULL || out_size == 0) {
        return 0;
    }
    const size_t want_len = strlen(param_name);
    size_t i = 0;
    while (i < len && v[i] != ';') {
        if (v[i] == '"') {
            return 0; /* the type itself is a token, never quoted */
        }
        i++;
    }
    while (i < len) {
        i++; /* past ';' */
        while (i < len && is_ows(v[i])) i++;
        const size_t name_start = i;
        while (i < len && is_tchar((unsigned char)v[i])) i++;
        const size_t name_len = i - name_start;
        while (i < len && is_ows(v[i])) i++;
        if (name_len == 0 || i >= len || v[i] != '=') {
            return 0;
        }
        i++;
        while (i < len && is_ows(v[i])) i++;
        const int wanted = name_len == want_len && strncasecmp(v + name_start, param_name, want_len) == 0;

        char tmp[512];
        size_t tmp_len = 0;
        if (i < len && v[i] == '"') {
            i++;
            for (;;) {
                if (i >= len) {
                    return 0; /* unterminated quoted-string */
                }
                unsigned char c = (unsigned char)v[i];
                if (c == '"') {
                    i++;
                    break;
                }
                if (c == '\\' && i + 1 < len && (v[i + 1] == '"' || v[i + 1] == '\\')) {
                    c = (unsigned char)v[++i];
                } else if ((c < 0x20 && c != '\t') || c == 0x7f) {
                    return 0;
                }
                if (tmp_len + 1 >= sizeof(tmp)) {
                    return 0;
                }
                tmp[tmp_len++] = (char)c;
                i++;
            }
        } else {
            while (i < len && is_tchar((unsigned char)v[i])) {
                if (tmp_len + 1 >= sizeof(tmp)) {
                    return 0;
                }
                tmp[tmp_len++] = v[i++];
            }
            if (tmp_len == 0) {
                return 0; /* "name=" with no value */
            }
        }
        while (i < len && is_ows(v[i])) i++;
        if (i < len && v[i] != ';') {
            return 0; /* junk after the value */
        }
        if (wanted) {
            if (tmp_len >= out_size) {
                return 0;
            }
            memcpy(out, tmp, tmp_len);
            out[tmp_len] = '\0';
            return 1;
        }
    }
    return 0;
}

int multipart_parse_boundary(const char *content_type, char *boundary_out, size_t boundary_out_size) {
    static const char prefix[] = "multipart/form-data";
    if (content_type == NULL || strncasecmp(content_type, prefix, strlen(prefix)) != 0) {
        return 0;
    }
    return extract_param(content_type, strlen(content_type), "boundary", boundary_out, boundary_out_size);
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
/* The value of header `name` in a NUL-terminated CRLF header block: matched only at a line start and
 * case-insensitively, leading OWS skipped; *len_out is the value's length up to its line's CRLF. NULL
 * if absent. Line-anchored, so a header name quoted inside another header's value is never found. */
static const char *find_header_value(const char *block, const char *name, size_t *len_out) {
    const size_t name_len = strlen(name);
    const char *line = block;
    while (*line != '\0') {
        const char *eol = strstr(line, "\r\n");
        const size_t line_len = eol != NULL ? (size_t)(eol - line) : strlen(line);
        if (line_len > name_len && strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *v = line + name_len + 1;
            const char *end = line + line_len;
            while (v < end && is_ows(*v)) v++;
            *len_out = (size_t)(end - v);
            return v;
        }
        if (eol == NULL) {
            break;
        }
        line = eol + 2;
    }
    return NULL;
}

static void parse_part_headers(const char *header_block, size_t header_block_len, MultipartPart *part) {
    char buf[512];
    size_t copy_len = header_block_len < sizeof(buf) - 1 ? header_block_len : sizeof(buf) - 1;
    memcpy(buf, header_block, copy_len);
    buf[copy_len] = '\0';

    size_t len;
    const char *disposition = find_header_value(buf, "Content-Disposition", &len);
    if (disposition != NULL) {
        extract_param(disposition, len, "name", part->name, sizeof(part->name));
        extract_param(disposition, len, "filename", part->filename, sizeof(part->filename));
    }

    const char *type_header = find_header_value(buf, "Content-Type", &len);
    if (type_header != NULL) {
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

int multipart_safe_filename(const MultipartPart *part, char *out, const size_t out_size) {
    if (part == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    const char *base = part->filename;
    for (const char *p = part->filename; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1; /* basename only: a client's directory (Unix or Windows) is never kept */
        }
    }
    size_t n = 0;
    for (const char *p = base; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            continue;
        }
        if (n + 1 >= out_size) {
            return 0;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    if (n == 0 || strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}
