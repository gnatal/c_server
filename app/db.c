#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include "db.h"

/* Bounds g_path (below) - a filesystem path, not exposed in any header, so
 * this doesn't need to match PATH_MAX (lib/app_types.h) exactly. */
#define DB_PATH_MAX 4096

static sqlite3 *g_db = NULL;
static char g_path[DB_PATH_MAX] = {0};

static const char *const SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS todos ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  title TEXT NOT NULL,"
    "  done INTEGER NOT NULL DEFAULT 0 CHECK (done IN (0, 1)),"
    "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "  updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))"
    ");";

int db_open(const char *path) {
    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "db_open: empty path\n");
        return -1;
    }
    if (strlen(path) >= sizeof(g_path)) {
        fprintf(stderr, "db_open: path too long\n");
        return -1;
    }
    snprintf(g_path, sizeof(g_path), "%s", path);

    sqlite3 *db = NULL;
    if (sqlite3_open(g_path, &db) != SQLITE_OK) {
        fprintf(stderr, "db_open: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    char *err = NULL;
    if (sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "db_open: schema migration failed: %s\n", err != NULL ? err : "unknown error");
        sqlite3_free(err);
        sqlite3_close(db);
        return -1;
    }

    /* Validation only - the real per-process connection is opened by
     * db_worker_init, after any cluster fork (see db.h doc comment). */
    sqlite3_close(db);
    return 0;
}

void db_close(void) {
    if (g_db != NULL) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

void db_worker_init(void) {
    if (g_path[0] == '\0') {
        fprintf(stderr, "db_worker_init: db_open was never called\n");
        return;
    }
    if (g_db != NULL) {
        return;
    }
    if (sqlite3_open(g_path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "db_worker_init: %s\n", sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return;
    }
    /* Multiple worker processes (lib/cluster.c) may hit this same file
     * concurrently - WAL journal mode and a busy timeout let a writer wait
     * for another process's lock instead of immediately failing with
     * SQLITE_BUSY. A WAL request SQLite can't honor (e.g. path is
     * ":memory:") is silently downgraded rather than an error, so this
     * isn't treated as fatal. */
    sqlite3_busy_timeout(g_db, 5000);
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
}

int todo_title_is_valid(const char *title) {
    if (title == NULL) {
        return 0;
    }
    size_t len = strlen(title);
    return len > 0 && len < TODO_TITLE_MAX;
}

/* Fills *out from the current row of a SELECT id, title, done, created_at,
 * updated_at statement (the column order every query below uses). */
static void fill_todo_from_row(sqlite3_stmt *stmt, Todo *out) {
    out->id = sqlite3_column_int64(stmt, 0);
    snprintf(out->title, sizeof(out->title), "%s", (const char *)sqlite3_column_text(stmt, 1));
    out->done = sqlite3_column_int(stmt, 2);
    snprintf(out->created_at, sizeof(out->created_at), "%s", (const char *)sqlite3_column_text(stmt, 3));
    snprintf(out->updated_at, sizeof(out->updated_at), "%s", (const char *)sqlite3_column_text(stmt, 4));
}

int db_list_todos(TodoList *out, const int *done_filter) {
    if (g_db == NULL) {
        return -1;
    }
    out->count = 0;

    const char *sql = done_filter != NULL
        ? "SELECT id, title, done, created_at, updated_at FROM todos WHERE done = ?1 ORDER BY id"
        : "SELECT id, title, done, created_at, updated_at FROM todos ORDER BY id";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_list_todos: prepare failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    if (done_filter != NULL) {
        sqlite3_bind_int(stmt, 1, *done_filter ? 1 : 0);
    }

    /* Unlike MAX_MULTIPART_PARTS/MAX_QUERY_PARAMS elsewhere in this project
     * (lib/CLAUDE.md), which truncate a single anomalous *request*, this cap
     * is a function of total stored rows - once the table naturally grows
     * past TODO_LIST_MAX, truncation is not an anomaly, it's the steady
     * state, and it would otherwise re-fire on every single GET forever.
     * Warn once per process (not per call) so an operator still finds out
     * the cap exists, without turning ordinary read traffic into a stderr
     * flood once the table is large - this is a real load-bearing fix, not
     * just benchmark hygiene: at high request rates the fprintf itself was
     * slow enough to dominate request latency (see scripts/CLAUDE.md). */
    static int warned = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (out->count >= TODO_LIST_MAX) {
            if (!warned) {
                fprintf(stderr, "db_list_todos: TODO_LIST_MAX exceeded, truncating "
                                 "(further occurrences are silent)\n");
                warned = 1;
            }
            break;
        }
        fill_todo_from_row(stmt, &out->items[out->count]);
        out->count++;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        fprintf(stderr, "db_list_todos: step failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    return 0;
}

int db_get_todo(long long id, Todo *out) {
    if (g_db == NULL) {
        return -1;
    }
    static const char *const sql = "SELECT id, title, done, created_at, updated_at FROM todos WHERE id = ?1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_get_todo: prepare failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, id);

    int rc = sqlite3_step(stmt);
    int result;
    if (rc == SQLITE_ROW) {
        fill_todo_from_row(stmt, out);
        result = 1;
    } else if (rc == SQLITE_DONE) {
        result = 0;
    } else {
        fprintf(stderr, "db_get_todo: step failed: %s\n", sqlite3_errmsg(g_db));
        result = -1;
    }
    sqlite3_finalize(stmt);
    return result;
}

int db_create_todo(const char *title, Todo *out) {
    if (g_db == NULL) {
        return -1;
    }
    static const char *const sql = "INSERT INTO todos (title) VALUES (?1)";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_create_todo: prepare failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_create_todo: step failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    long long id = sqlite3_last_insert_rowid(g_db);
    return db_get_todo(id, out) == 1 ? 0 : -1;
}

int db_replace_todo(long long id, const char *title, int done, Todo *out) {
    if (g_db == NULL) {
        return -1;
    }
    static const char *const sql =
        "UPDATE todos SET title = ?1, done = ?2, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?3";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_replace_todo: prepare failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, done ? 1 : 0);
    sqlite3_bind_int64(stmt, 3, id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_replace_todo: step failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    if (sqlite3_changes(g_db) == 0) {
        return 0;
    }
    return db_get_todo(id, out) == 1 ? 1 : -1;
}

int db_patch_todo(long long id, const char *title, const int *done, Todo *out) {
    if (g_db == NULL) {
        return -1;
    }
    static const char *const sql =
        "UPDATE todos SET title = COALESCE(?1, title), done = COALESCE(?2, done), "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?3";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_patch_todo: prepare failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    if (title != NULL) {
        sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 1);
    }
    if (done != NULL) {
        sqlite3_bind_int(stmt, 2, *done ? 1 : 0);
    } else {
        sqlite3_bind_null(stmt, 2);
    }
    sqlite3_bind_int64(stmt, 3, id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_patch_todo: step failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    if (sqlite3_changes(g_db) == 0) {
        return 0;
    }
    return db_get_todo(id, out) == 1 ? 1 : -1;
}

int db_delete_todo(long long id) {
    if (g_db == NULL) {
        return -1;
    }
    static const char *const sql = "DELETE FROM todos WHERE id = ?1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_delete_todo: prepare failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_delete_todo: step failed: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    return sqlite3_changes(g_db) > 0 ? 1 : 0;
}
