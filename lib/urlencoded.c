#include <string.h>
#include "urlencoded.h"
#include "http_parser.h"

int parse_urlencoded_body(const char *body, size_t body_len, UrlEncodedForm *form) {
    form->field_count = 0;
    if (body == NULL || body_len == 0) {
        return 0;
    }

    size_t pos = 0;
    while (pos < body_len && form->field_count < MAX_FORM_FIELDS) {
        const char *amp = memchr(body + pos, '&', body_len - pos);
        size_t pair_end = amp != NULL ? (size_t)(amp - body) : body_len;
        size_t pair_len = pair_end - pos;

        if (pair_len > 0) {
            /* consecutive '&'s (an empty pair) are skipped rather than
             * stored, same as parse_query_string's strtok_r skipping
             * repeated delimiters. */
            const char *eq = memchr(body + pos, '=', pair_len);
            size_t name_len = eq != NULL ? (size_t)(eq - (body + pos)) : pair_len;
            const char *value_start = eq != NULL ? eq + 1 : body + pair_end;
            size_t value_len = eq != NULL ? pair_end - (size_t)(value_start - body) : 0;

            /* decoded straight from body into the slots, so the slot limit applies to the
             * decoded length, and an escape is never split by a pre-decode cut. */
            const int name_rc = url_decode_span(body + pos, name_len, form->field_names[form->field_count],
                                                sizeof(form->field_names[0]), 1);
            const int value_rc = url_decode_span(value_start, value_len, form->field_values[form->field_count],
                                                 sizeof(form->field_values[0]), 1);
            if (name_rc == -1 || value_rc == -1) {
                form->field_count = 0;
                return -1;
            }
            if (name_rc == -2 || value_rc == -2) {
                form->field_count = 0;
                return -2;
            }
            form->field_count++;
        }

        pos = pair_end + 1;
    }
    return form->field_count;
}

const char *urlencoded_get_field(const UrlEncodedForm *form, const char *name) {
    for (int i = 0; i < form->field_count; i++) {
        if (strcmp(form->field_names[i], name) == 0) {
            return form->field_values[i];
        }
    }
    return NULL;
}
