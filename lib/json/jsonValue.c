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
