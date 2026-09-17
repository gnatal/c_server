#ifndef URLENCODED_H
#define URLENCODED_H

#include <stddef.h>
#include "app_types.h"

/*
 * Pure: splits body (body_len raw bytes - not assumed to be a
 * NUL-terminated C string, same convention as parse_multipart_body,
 * lib/multipart.h) on '&' then the first '=' into form->field_names/
 * field_values, bounded by MAX_FORM_FIELDS (extra pairs past the cap are
 * dropped rather than overflowing the fixed arrays, same as
 * MAX_QUERY_PARAMS/MAX_HEADERS elsewhere). A pair with no '=' (e.g. a bare
 * "flag") gets an empty-string value, same as parse_query_string. Both name
 * and value are then URL-decoded (url_decode, decode_plus = 1 - the
 * application/x-www-form-urlencoded convention) into the destination slots,
 * same as parse_query_string decodes req->query_names/query_values -
 * body itself is left untouched (never mutated), unlike parse_query_string's
 * strtok_r-based split which needs its own local copy of the (much smaller,
 * fixed-size) query string.
 *
 * body may be NULL (treated the same as an empty body: form->field_count is
 * left at 0, not an error).
 */
void parse_urlencoded_body(const char *body, size_t body_len, UrlEncodedForm *form);

/*
 * Looks up the first field with the given name, or NULL if absent - mirrors
 * req_get_query/multipart_get_part's first-match lookup convention.
 */
const char *urlencoded_get_field(const UrlEncodedForm *form, const char *name);

#endif /* URLENCODED_H */
