#ifndef COOKBOOK_H
#define COOKBOOK_H

#include "app_types.h"

/*
 * Registers every recipe in cookbook.c on `app` (routes, middleware, error handler, one worker hook).
 * tests/test_cookbook.c sends real requests through parse -> route -> dispatch and asserts on the
 * responses, so each recipe is known to compile and behave as described.
 */
void cookbook_register(App *app);

/* Test-only observers (tests/test_cookbook.c). */
int cookbook_last_status_seen(void);
int cookbook_worker_resource_opened(void);
void cookbook_run_worker_hooks(App *app);
/* Recipe 15: watch `fd` (connected, non-blocking) as the job worker's socket. */
void cookbook_attach_jobs(App *app, int fd);

#endif /* COOKBOOK_H */
