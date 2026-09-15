#include "appTypes.h"
#include "router.h"
#include "connection.h"
#include "handlers.h"

int main(void) {
    App app;
    app_init(&app);

    app_get(&app, "/", handler_home);
    app_get(&app, "/users/:id", handler_get_user);
    app_post(&app, "/echo", handler_echo);
    app_post(&app, "/echo/json", handler_echo_json);

    app_listen(&app, DEFAULT_PORT);

    return 0;
}
