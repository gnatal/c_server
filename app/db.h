#ifndef APP_DB_H
#define APP_DB_H

#include "todo_types.h"

/*
 * Opens (creating if necessary) the SQLite database at path, runs the
 * "todos" table schema migration, and closes the connection again -
 * this validates path/permissions/schema fail-fast, without leaving a
 * connection open. Returns 0 on success, -1 on failure (path unopenable,
 * migration failed), which main() should treat as fatal at startup
 * (deny-by-default: don't start serving against a broken database).
 *
 * path is remembered internally so db_worker_init (below) can open this
 * app's own private connection later - see that function's doc comment for
 * why this two-step open-then-close-then-reopen exists.
 */
int db_open(const char *path);

/* Closes the current connection, if any. Safe to call when none is open. */
void db_close(void);

/*
 * Opens this process's own private connection to the path db_open last
 * validated, and configures it for safe multi-process access
 * (sqlite3_busy_timeout, WAL journal mode).
 *
 * Intended to be registered via app_on_worker_start(app, db_worker_init)
 * (lib/connection.h) rather than called directly - see lib/CLAUDE.md
 * ("Worker lifecycle hooks") for why: lib/cluster.c may fork() worker
 * processes after main() runs, and SQLite connections are not safe to share
 * across a fork (documented locking-corruption risk). db_open validates and
 * migrates the schema once in the original process and closes its
 * connection before app_listen() (where forking may happen); this function
 * is what actually opens the connection every CRUD function below uses, and
 * it runs once per worker process - standalone, or each individual forked
 * cluster worker - always after any fork already happened.
 */
void db_worker_init(void);

/*
 * Pure validation - no database touched: true when title is non-NULL,
 * non-empty, and fits within TODO_TITLE_MAX - 1 bytes. Exposed separately
 * from the write path so handlers can reject a bad title before ever
 * reaching the database.
 */
int todo_title_is_valid(const char *title);

/*
 * CRUD operations. Every function returns -1 on a database-layer error
 * (caller should respond 500); the per-id operations (db_get_todo,
 * db_replace_todo, db_patch_todo, db_delete_todo) return 0 when no row
 * matches id (caller should respond 404) and 1 on success. db_create_todo
 * returns 0 on success (mirroring db_list_todos below), -1 on failure.
 * Every successful per-row operation (create/get/replace/patch) fills *out
 * with the row's current values, including the timestamps SQLite itself
 * generated/updated.
 */
int db_list_todos(TodoList *out, const int *done_filter);
int db_get_todo(long long id, Todo *out);
int db_create_todo(const char *title, Todo *out);
int db_replace_todo(long long id, const char *title, int done, Todo *out);
int db_patch_todo(long long id, const char *title, const int *done, Todo *out);
int db_delete_todo(long long id);

#endif /* APP_DB_H */
