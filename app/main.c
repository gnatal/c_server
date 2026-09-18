#include "cexpress.h"
#include "handlers.h"
#include "middlewares.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  App app;
  app_init(&app);

  const char *port_env = getenv("PORT");
  if (port_env != NULL) {
    int parsed_port = atoi(port_env);
    if (parsed_port > 0 && parsed_port <= 65535) {
      app.config.port = parsed_port;
    }
  }

  const char *workers_env = getenv("WORKERS");
  if (workers_env != NULL) {
    if (strcmp(workers_env, "auto") == 0) {
      app.config.workers = 0; /* 0 triggers auto-detection of CPU cores */
    } else {
      int parsed_workers = atoi(workers_env);
      if (parsed_workers >= 0) {
        app.config.workers = parsed_workers;
      }
    }
  }

  const char *api_key_env = getenv("API_KEY");
  if (api_key_env != NULL && api_key_env[0] != '\0') {
    mw_authenticate_set_key(api_key_env);
  }

  const char *cert_env = getenv("TLS_CERT");
  const char *key_env = getenv("TLS_KEY");
  if (cert_env != NULL && key_env != NULL) {
    if (app_enable_tls(&app, cert_env, key_env) != 0) {
      fprintf(stderr, "Failed to configure TLS with cert '%s' and key '%s'\n", cert_env, key_env);
      return 1;
    }
  } else if (cert_env != NULL || key_env != NULL) {
    fprintf(stderr, "Warning: Both TLS_CERT and TLS_KEY must be set to enable TLS/HTTPS\n");
  }

  /* SQLite-backed Todo storage: db_open validates TODO_DB_PATH and runs the
   * schema migration once, in this (pre-fork) process, then closes again -
   * app_on_worker_start registers db_worker_init to open this app's actual,
   * per-process connection later, always after any cluster fork has
   * happened. See lib/CLAUDE.md ("Worker lifecycle hooks") and
   * app/CLAUDE.md for why this two-step dance is necessary. */
  const char *db_path_env = getenv("TODO_DB_PATH");
  const char *db_path = (db_path_env != NULL && db_path_env[0] != '\0') ? db_path_env : "todos.db";
  if (db_open(db_path) != 0) {
    fprintf(stderr, "Failed to initialize database at '%s'\n", db_path);
    return 1;
  }
  db_close();
  app_on_worker_start(&app, db_worker_init);

  const char *quiet_env = getenv("QUIET");
  if (quiet_env == NULL || strcmp(quiet_env, "1") != 0) {
    app_use(&app, mw_logger);
  }
  app_use(&app, mw_body_size_guard);
  app_use_error(&app, error_handler_json);

  /* Todo UI: a single self-contained static page, served directly (not
   * through the /static mount below) via res_send_file. */
  app_get(&app, "/", handler_home);

  /* Generic static-file-serving demo (lib/router.h: app_serve_static),
   * unrelated to the Todo UI above - serves whatever's in app/public/
   * (e.g. GET /static/style.css). */
  app_serve_static(&app, "/static", "app/public");

  /* Todo REST API, mounted under /api/todos via a Router (app_mount) - the
   * C analogue of Express's app.use('/api/todos', router). Reads (GET) are
   * public; writes each attach mw_authenticate as per-route middleware, so
   * a per-route middleware list and a mounted sub-router compose together -
   * unlike router_use, this doesn't gate the GETs on the same router. */
  Router todo_router;
  router_init(&todo_router);
  router_get(&todo_router, "/", handler_list_todos);
  router_get(&todo_router, "/:id", handler_get_todo);
  router_post_mw(&todo_router, "/", handler_create_todo, (Middleware[]){mw_authenticate}, 1);
  router_put_mw(&todo_router, "/:id", handler_replace_todo, (Middleware[]){mw_authenticate}, 1);
  router_patch_mw(&todo_router, "/:id", handler_patch_todo, (Middleware[]){mw_authenticate}, 1);
  router_delete_mw(&todo_router, "/:id", handler_delete_todo, (Middleware[]){mw_authenticate}, 1);
  app_mount(&app, "/api/todos", &todo_router);

  app_listen(&app, app.config.port);
  db_close();
  app_destroy(&app);

  return 0;
}
