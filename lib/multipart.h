#ifndef MULTIPART_H
#define MULTIPART_H

#include <stddef.h>
#include "app_types.h"

/*
 * Pure: checks whether content_type declares a multipart/form-data body
 * (prefix match against "multipart/form-data", case-insensitive - trailing
 * parameters/casing don't break the match, same convention as
 * app/handlers.c: has_json_content_type) and, if so, extracts its
 * "boundary=" parameter into boundary_out. RFC 2046 allows the boundary
 * value to be double-quoted or bare; either form is accepted, and quotes
 * are stripped either way.
 *
 * Returns 1 and fills boundary_out (NUL-terminated) on success. Returns 0,
 * leaving boundary_out untouched, when content_type is NULL, isn't
 * multipart/form-data, has no boundary parameter, or the boundary is too
 * long to fit boundary_out (see MAX_BOUNDARY_LEN, app_types.h).
 */
int multipart_parse_boundary(const char *content_type, char *boundary_out, size_t boundary_out_size);

/*
 * Pure: splits body (body_len raw bytes - not assumed to be a
 * NUL-terminated C string, since a file part's payload can contain
 * arbitrary bytes, including embedded NULs) on the RFC 2046 boundary
 * delimiters ("--boundary\r\n" between parts, "--boundary--" after the
 * last one) into form->parts, bounded by MAX_MULTIPART_PARTS (extra parts
 * past the cap are dropped with a stderr warning rather than overflowing
 * the fixed array, same convention as MAX_ROUTES/MAX_HEADERS elsewhere).
 *
 * Each part's own header block (Content-Disposition, optionally
 * Content-Type) is parsed for its form field `name` and, for a file part,
 * `filename` and `content_type` (app_types.h: MultipartPart). A part with
 * no Content-Disposition name= parameter is skipped outright - it's
 * malformed, there's no field to key it by. MultipartPart.data points
 * directly into body (never copied) and is NOT NUL-terminated - callers
 * must use data_len, never strlen, to read it.
 *
 * Returns the number of parts parsed into form (0 if the body has none,
 * e.g. a missing or mismatched boundary), or -1 if boundary is NULL,
 * empty, or longer than MAX_BOUNDARY_LEN can hold.
 */
int parse_multipart_body(const char *body, size_t body_len, const char *boundary, MultipartForm *form);

/*
 * Looks up the first part with the given form field name (its
 * Content-Disposition name= parameter), or NULL if absent - mirrors
 * req_get_header/req_get_query's first-match lookup convention
 * (lib/http_parser.h).
 */
const MultipartPart *multipart_get_part(const MultipartForm *form, const char *name);

#endif /* MULTIPART_H */
