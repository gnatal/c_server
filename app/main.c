#include <stdlib.h>
#include "appTypes.h"
#include "router.h"
#include "connection.h"
#include "middleware.h"
#include "handlers.h"
#include "middlewares.h"

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
    app_use(&app, mw_authenticate);
    app_use_error(&app, error_handler_json);

    app_get(&app, "/", handler_home);
    app_get(&app, "/users/:id", handler_get_user);
    app_post(&app, "/echo", handler_echo);
    app_post(&app, "/echo/json", handler_echo_json);

    app_listen(&app, app.config.port);

    return 0;
}
