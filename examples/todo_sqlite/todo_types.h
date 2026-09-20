#ifndef TODO_TYPES_H
#define TODO_TYPES_H

/* Bounds Todo.title (below) - long enough for a real task description,
 * short enough to keep Todo a fixed-size, stack-friendly struct rather than
 * a heap-allocated one, same convention as Request/Response fields
 * (lib/app_types.h). */
#define TODO_TITLE_MAX 256

/* Bounds Todo.created_at/updated_at - an ISO-8601 UTC timestamp
 * ("YYYY-MM-DDTHH:MM:SS.sssZ") is 24 bytes + NUL; rounded up for headroom. */
#define TODO_TIMESTAMP_MAX 32

/* Bounds TodoList.items (below) - the max number of rows db_list_todos
 * (app/db.h) returns in one call. Extra rows past this cap are not
 * returned (see app/CLAUDE.md), same truncate convention as
 * MAX_MULTIPART_PARTS/MAX_ROUTES elsewhere in this project. */
#define TODO_LIST_MAX 256

typedef struct {
    long long id;
    char title[TODO_TITLE_MAX];
    int done;
    char created_at[TODO_TIMESTAMP_MAX];
    char updated_at[TODO_TIMESTAMP_MAX];
} Todo;

typedef struct {
    Todo items[TODO_LIST_MAX];
    int count;
} TodoList;

#endif /* TODO_TYPES_H */
