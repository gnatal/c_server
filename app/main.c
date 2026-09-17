#include "app_types.h"
#include "connection.h"
#include "handlers.h"
#include "middleware.h"
#include "middlewares.h"
#include "router.h"
#include <stdlib.h>

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

  const char *api_key_env = getenv("API_KEY");
  if (api_key_env != NULL && api_key_env[0] != '\0') {
    mw_authenticate_set_key(api_key_env);
  }

  app_use(&app, mw_logger);
  app_use(&app, mw_body_size_guard);
  app_use_error(&app, error_handler_json);

  app_get(&app, "/", handler_home);
  app_get(&app, "/users/:id", handler_get_user);
  /* Query-string demo: GET /search?q=cats resolves via req_get_query(req, "q")
   * (lib/http_parser.c) - the query string is parsed into req->query_* once,
   * up front in parse_http_request, not re-scanned per lookup. */
  app_get(&app, "/search", handler_search);
  /* Wildcard demo: a trailing "*" segment matches that segment and everything
   * after it (lib/router.c: match_path), so this one route answers
   * GET /files/report.pdf as well as GET /files/2024/q1/report.pdf - it does
   * not serve actual files, just echoes req->path to show the match. */
  app_get(&app, "/files/*", handler_files);
  app_post(&app, "/echo", handler_echo);
  /* Cookie demo (lib/response.h/lib/http_parser.h): /login sets a session
   * cookie, /whoami reads it back via req_get_cookie, /logout expires it
   * via res_clear_cookie. */
  app_get(&app, "/login", handler_login);
  app_get(&app, "/whoami", handler_whoami);
  app_get(&app, "/logout", handler_logout);
  /* multipart/form-data demo: POST /upload with fields and/or file parts -
   * handler_upload (lib/multipart.h) echoes each part back as JSON, file
   * parts as {filename, content_type, size} rather than raw bytes. */
  app_post(&app, "/upload", handler_upload);
  /* Only this route requires auth - mw_authenticate is per-route middleware,
   * not app-wide, so it doesn't gate "/" or the other routes above. */
  app_post_mw(&app, "/echo/json", handler_echo_json, (Middleware[]){mw_authenticate}, 1);

  /* Router demo: a standalone route table mounted under "/api" via
   * app_mount, the C analogue of Express's Router() + app.use('/api', router).
   * router_use(mw_authenticate) makes every route on this router require a
   * bearer token - unlike the single per-route app_post_mw above, this scopes
   * the whole "/api" prefix at once, including routes added to api_router
   * later. Paths are relative to the router: "/status" ends up as
   * "GET /api/status", "/users/:id" as "GET /api/users/:id" (path params
   * still resolve normally through the mount, req_get_param(req, "id") works
   * the same as it does for the top-level "/users/:id" route above). */
  Router api_router;
  router_init(&api_router);
  router_use(&api_router, mw_authenticate);
  router_get(&api_router, "/status", handler_api_status);
  router_get(&api_router, "/users/:id", handler_get_user);
  /* PUT/PATCH/DELETE demo: app_put/app_patch/app_delete (and their router_*
   * equivalents) are thin wrappers around app_add_route(_mw)/router_add_route(_mw)
   * that register a different HTTP method against the same "/users/:id"
   * pattern - a path match alone isn't enough, the method has to match too
   * (lib/CLAUDE.md, "Deny-by-default routing"), so these coexist with the
   * GET above without conflicting. */
  router_put(&api_router, "/users/:id", handler_update_user);
  router_patch(&api_router, "/users/:id", handler_patch_user);
  router_delete(&api_router, "/users/:id", handler_delete_user);
  app_mount(&app, "/api", &api_router);

  app_listen(&app, app.config.port);

  return 0;
}
