#include <stdlib.h>
#include <string.h>
#include "json.h"

void json_free(JsonValue *value) {
    if (value == NULL) {
        return;
    }
    switch (value->type) {
        case JSON_STRING:
            free(value->as.string);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < value->as.array.count; i++) {
                json_free(value->as.array.items[i]);
            }
            free(value->as.array.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < value->as.object.count; i++) {
                free(value->as.object.members[i].key);
                json_free(value->as.object.members[i].value);
            }
            free(value->as.object.members);
            break;
        case JSON_NULL:
        case JSON_BOOL:
        case JSON_NUMBER:
            break;
    }
    free(value);
}

const JsonValue *json_object_get(const JsonValue *object, const char *key) {
    if (object == NULL || object->type != JSON_OBJECT || key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < object->as.object.count; i++) {
        if (strcmp(object->as.object.members[i].key, key) == 0) {
            return object->as.object.members[i].value;
        }
    }
    return NULL;
}

const JsonValue *json_array_get(const JsonValue *array, size_t index) {
    if (array == NULL || array->type != JSON_ARRAY || index >= array->as.array.count) {
        return NULL;
    }
    return array->as.array.items[index];
}

size_t json_array_count(const JsonValue *array) {
    return (array != NULL && array->type == JSON_ARRAY) ? array->as.array.count : 0;
}

int json_is_null(const JsonValue *value) {
    return value == NULL || value->type == JSON_NULL;
}

int json_as_bool(const JsonValue *value, int default_value) {
    return (value != NULL && value->type == JSON_BOOL) ? value->as.boolean : default_value;
}

double json_as_number(const JsonValue *value, double default_value) {
    return (value != NULL && value->type == JSON_NUMBER) ? value->as.number : default_value;
}

const char *json_as_string(const JsonValue *value, const char *default_value) {
    return (value != NULL && value->type == JSON_STRING) ? value->as.string : default_value;
}

/* Bounded copy helper for the builders below - malloc+memcpy instead of
 * strdup (a POSIX extension, not a guaranteed-portable libc function) or
 * strcpy (banned project-wide). len is strlen(text); the +1 also copies
 * text's own NUL terminator in the same pass. */
static char *copy_string(const char *text, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, len + 1);
    return copy;
}

JsonValue *json_new_string(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v == NULL) {
        return NULL;
    }
    v->type = JSON_STRING;
    v->as.string = copy_string(value, strlen(value));
    if (v->as.string == NULL) {
        free(v);
        return NULL;
    }
    return v;
}

JsonValue *json_new_object(void) {
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v == NULL) {
        return NULL;
    }
    v->type = JSON_OBJECT;
    return v;
}

JsonValue *json_new_number(double value) {
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v == NULL) {
        return NULL;
    }
    v->type = JSON_NUMBER;
    v->as.number = value;
    return v;
}

JsonValue *json_new_bool(int value) {
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v == NULL) {
        return NULL;
    }
    v->type = JSON_BOOL;
    v->as.boolean = value ? 1 : 0;
    return v;
}

JsonValue *json_new_array(void) {
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v == NULL) {
        return NULL;
    }
    v->type = JSON_ARRAY;
    return v;
}

int json_object_set(JsonValue *object, const char *key, JsonValue *value) {
    if (object == NULL || object->type != JSON_OBJECT || key == NULL) {
        json_free(value);
        return 0;
    }
    if (value == NULL) {
        return 0;
    }

    /* If the key already exists, replace value in-place and free old value */
    for (size_t i = 0; i < object->as.object.count; i++) {
        if (strcmp(object->as.object.members[i].key, key) == 0) {
            json_free(object->as.object.members[i].value);
            object->as.object.members[i].value = value;
            return 1;
        }
    }

    char *key_copy = copy_string(key, strlen(key));
    if (key_copy == NULL) {
        json_free(value);
        return 0;
    }

    /* One member per call rather than parse_object's doubling growth
     * (json_parser.c) - builder objects are small (a handful of fields at
     * most), so the simpler realloc-per-call isn't worth the extra state. */
    JsonMember *grown = realloc(object->as.object.members,
                                 (object->as.object.count + 1) * sizeof(JsonMember));
    if (grown == NULL) {
        free(key_copy);
        json_free(value);
        return 0;
    }
    object->as.object.members = grown;
    object->as.object.members[object->as.object.count].key = key_copy;
    object->as.object.members[object->as.object.count].value = value;
    object->as.object.count++;
    return 1;
}

int json_array_append(JsonValue *array, JsonValue *value) {
    if (array == NULL || array->type != JSON_ARRAY) {
        json_free(value);
        return 0;
    }
    if (value == NULL) {
        return 0;
    }

    /* One item per call, same reasoning as json_object_set above - builder
     * arrays are small. */
    JsonValue **grown = realloc(array->as.array.items,
                                 (array->as.array.count + 1) * sizeof(JsonValue *));
    if (grown == NULL) {
        json_free(value);
        return 0;
    }
    array->as.array.items = grown;
    array->as.array.items[array->as.array.count] = value;
    array->as.array.count++;
    return 1;
}
