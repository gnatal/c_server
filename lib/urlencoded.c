#include <string.h>
#include "urlencoded.h"
#include "http_parser.h"

void parse_urlencoded_body(const char *body, size_t body_len, UrlEncodedForm *form) {
    form->field_count = 0;
    if (body == NULL || body_len == 0) {
        return;
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

            char *name_slot = form->field_names[form->field_count];
            char *value_slot = form->field_values[form->field_count];

            size_t copy_name = name_len < sizeof(form->field_names[0]) - 1 ? name_len : sizeof(form->field_names[0]) - 1;
            memcpy(name_slot, body + pos, copy_name);
            name_slot[copy_name] = '\0';

            size_t copy_value = value_len < sizeof(form->field_values[0]) - 1 ? value_len : sizeof(form->field_values[0]) - 1;
            memcpy(value_slot, value_start, copy_value);
            value_slot[copy_value] = '\0';

            url_decode(name_slot, name_slot, sizeof(form->field_names[0]), 1);
            url_decode(value_slot, value_slot, sizeof(form->field_values[0]), 1);
            form->field_count++;
        }

        pos = pair_end + 1;
    }
}

const char *urlencoded_get_field(const UrlEncodedForm *form, const char *name) {
    for (int i = 0; i < form->field_count; i++) {
        if (strcmp(form->field_names[i], name) == 0) {
            return form->field_values[i];
        }
    }
    return NULL;
}
