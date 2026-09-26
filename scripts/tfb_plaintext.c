/* TechEmpower plaintext test, standalone: the same /plaintext handler as the TFB entry
 * (Rest_in_c/techempower/cexpress/src/main.c), without the DB routes. Driven by scripts/tfb_plaintext.sh.
 * Env: PORT (default 8080), CEXPRESS_WORKERS (default 1; 0 = one per core). */
#include "cexpress.h"
#include <stdlib.h>

#define SERVER_NAME "cexpress"
#define HELLO "Hello, World!"

static void handler_plaintext(const Request *req, Response *res) {
    (void)req;
    res_set_header(res, "Server", SERVER_NAME);
    res_send(res, HELLO);
}

int main(void) {
    App app;
    app_init(&app);
    const char *workers_env = getenv("CEXPRESS_WORKERS");
    app.config.workers = workers_env ? atoi(workers_env) : 1;
    const char *port_env = getenv("PORT");
    app_get(&app, "/plaintext", handler_plaintext);
    app_listen(&app, port_env ? atoi(port_env) : 8080);
    app_destroy(&app);
    return 0;
}
