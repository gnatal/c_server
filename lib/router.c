#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include "router.h"
#include "middleware.h"

void app_init(App *app) {
    app->config.port = DEFAULT_PORT;
    app->config.workers = 1;
    app->config.max_connections = DEFAULT_MAX_CONNECTIONS;
    app->open_connections = 0;
    /* Best effort (S3): a failed open just means EMFILE gets the pre-existing silent behavior -
     * see App.spare_fd. */
    app->spare_fd = open("/dev/null", O_RDONLY);
    app->method_tree_count = 0;
    app->middleware_count = 0;
    app->body_limit_count = 0;
    app->error_handler = NULL;
    app->worker_init_hook_count = 0;
    app->server_fd = -1;
    app->accept_via_fd_passing = 0;
#if defined(__linux__) && !defined(CEXPRESS_USE_EPOLL)
    app->ring = NULL;
#else
    app->loop_fd = -1;
#endif
    app->poll_regs = NULL;
    app->poll_regs_cap = 0;
    app->timer_idle_fd = -1;
    app->timer_shutdown_fd = -1;
    app->signal_fd = -1;
    app->is_shutting_down = 0;

    /* Starting allocation for the fd-indexed connections table (app_types.h)
     * - grown later by ensure_connection_capacity (connection.c) as needed.
     * The server can't run at all without this, so a failed calloc aborts
     * immediately rather than leaving app in a half-initialized state -
     * same failure convention as create_server_socket (connection.c). */
    app->connections = calloc(INITIAL_CONNECTION_TABLE_CAP, sizeof(Connection *));
    if (app->connections == NULL) {
        perror("app_init: calloc");
        exit(EXIT_FAILURE);
    }
    app->connections_cap = INITIAL_CONNECTION_TABLE_CAP;

    /* M1: one shared arena for the whole worker process (every Connection.arena it creates just points
     * here), not one per connection - see app_types.h's App.arena and Connection.arena comments. Same
     * failure convention as the calloc above: the server can't run without this either. */
    char *arena_buf = malloc(ARENA_SIZE);
    if (arena_buf == NULL) {
        perror("app_init: malloc (arena)");
        exit(EXIT_FAILURE);
    }
    arena_init(&app->arena, arena_buf, ARENA_SIZE);

    /* M2: the one receive buffer idle connections borrow (App.read_buf), instead of each owning one. */
    app->read_buf = malloc(BUF_SIZE);
    if (app->read_buf == NULL) {
        perror("app_init: malloc (read_buf)");
        exit(EXIT_FAILURE);
    }
}

/* Shared fill logic for one route slot, used by both app_add_route_mw and
 * router_add_route_mw - an App and a Router register routes identically,
 * they just land in different fixed Route[MAX_ROUTES] arrays.
 * S12: a method/path that doesn't fit its fixed slot is rejected outright (0/-1), not silently
 * truncated by strncpy - a truncated pattern used to register a different, shorter route than the
 * caller asked for with no indication anything was wrong. Returns 0 on success, -1 (route left
 * untouched, nothing registered) if either is too long. */
static int fill_route(Route *route, const char *method, const char *path, Handler handler,
                       const Middleware *middlewares, int middleware_count) {
    if (strlen(method) >= sizeof(route->method)) {
        fprintf(stderr, "route registration: method \"%s\" exceeds %zu chars, route \"%s\" not registered\n",
                method, sizeof(route->method) - 1, path);
        return -1;
    }
    if (strlen(path) >= sizeof(route->path)) {
        fprintf(stderr, "route registration: path \"%s\" exceeds %zu chars, route not registered\n",
                path, sizeof(route->path) - 1);
        return -1;
    }
    strncpy(route->method, method, sizeof(route->method) - 1);
    route->method[sizeof(route->method) - 1] = '\0';
    strncpy(route->path, path, sizeof(route->path) - 1);
    route->path[sizeof(route->path) - 1] = '\0';
    route->handler = handler;

    /* Every route filled through here is an ordinary (non-static) route -
     * only app_serve_static sets static_root, after fill_route returns.
     * Route slots aren't zero-initialized before this runs (App/Router are
     * plain structs, not calloc'd), so without this an ordinary route could
     * inherit whatever garbage was previously in this slot and be mistaken
     * for a static mount by chain_next() (middleware.c) - or have it passed
     * to free() by route_free. */
    route->static_root = NULL;

    if (middleware_count > MAX_ROUTE_MIDDLEWARES) {
        fprintf(stderr, "route registration: MAX_ROUTE_MIDDLEWARES exceeded, truncating\n");
        middleware_count = MAX_ROUTE_MIDDLEWARES;
    }
    for (int i = 0; i < middleware_count; i++) {
        route->middlewares[i] = middlewares[i];
    }
    route->middleware_count = middleware_count;
    return 0;
}

static void tree_insert(PatriciaNode **root_ptr, const char *path, Route *route);

/* Frees a malloc'd Route and the static_root it owns (NULL for ordinary routes). Every path that
 * drops a heap Route - registration failure, duplicate, app_free_routes - goes through here. Never
 * called on a Router's fixed routes[] slots: those never own a static_root. */
static void route_free(Route *route) {
    free(route->static_root);
    free(route);
}

static void app_insert_route_struct(App *app, Route *route) {
    MethodTree *mt = NULL;
    for (int i = 0; i < app->method_tree_count; i++) {
        if (strcmp(app->method_trees[i].method, route->method) == 0) {
            mt = &app->method_trees[i];
            break;
        }
    }
    if (!mt) {
        if (app->method_tree_count >= 16) {
            fprintf(stderr, "app_add_route: max method trees exceeded\n");
            route_free(route);
            return;
        }
        mt = &app->method_trees[app->method_tree_count++];
        strncpy(mt->method, route->method, sizeof(mt->method) - 1);
        mt->method[sizeof(mt->method) - 1] = '\0';
        mt->tree = NULL;
    }
    tree_insert(&mt->tree, route->path, route);
}

void app_add_route(App *app, const char *method, const char *path, Handler handler) {
    app_add_route_mw(app, method, path, handler, NULL, 0);
}

void app_add_route_mw(App *app, const char *method, const char *path, Handler handler,
                       const Middleware *middlewares, int middleware_count) {
    Route *route = malloc(sizeof(Route));
    if (route == NULL) {
        fprintf(stderr, "app_add_route: out of memory, route \"%s %s\" not registered\n", method, path);
        return;
    }
    if (fill_route(route, method, path, handler, middlewares, middleware_count) != 0) {
        free(route);
        return;
    }
    app_insert_route_struct(app, route);
}

void app_get(App *app, const char *path, Handler handler) {
    app_add_route(app, "GET", path, handler);
}

void app_post(App *app, const char *path, Handler handler) {
    app_add_route(app, "POST", path, handler);
}

void app_get_mw(App *app, const char *path, Handler handler,
                 const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "GET", path, handler, middlewares, middleware_count);
}

void app_post_mw(App *app, const char *path, Handler handler,
                  const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "POST", path, handler, middlewares, middleware_count);
}

void app_put(App *app, const char *path, Handler handler) {
    app_add_route(app, "PUT", path, handler);
}

void app_patch(App *app, const char *path, Handler handler) {
    app_add_route(app, "PATCH", path, handler);
}

void app_delete(App *app, const char *path, Handler handler) {
    app_add_route(app, "DELETE", path, handler);
}

void app_put_mw(App *app, const char *path, Handler handler,
                 const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "PUT", path, handler, middlewares, middleware_count);
}

void app_patch_mw(App *app, const char *path, Handler handler,
                   const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "PATCH", path, handler, middlewares, middleware_count);
}

void app_delete_mw(App *app, const char *path, Handler handler,
                    const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "DELETE", path, handler, middlewares, middleware_count);
}

void app_head(App *app, const char *path, Handler handler) {
    app_add_route(app, "HEAD", path, handler);
}

void app_options(App *app, const char *path, Handler handler) {
    app_add_route(app, "OPTIONS", path, handler);
}

void app_head_mw(App *app, const char *path, Handler handler,
                  const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "HEAD", path, handler, middlewares, middleware_count);
}

void app_options_mw(App *app, const char *path, Handler handler,
                     const Middleware *middlewares, int middleware_count) {
    app_add_route_mw(app, "OPTIONS", path, handler, middlewares, middleware_count);
}

void router_init(Router *router) {
    router->route_count = 0;
    router->middleware_count = 0;
}

void router_add_route(Router *router, const char *method, const char *path, Handler handler) {
    router_add_route_mw(router, method, path, handler, NULL, 0);
}

void router_add_route_mw(Router *router, const char *method, const char *path, Handler handler,
                          const Middleware *middlewares, int middleware_count) {
    if (router->route_count >= MAX_ROUTER_ROUTES) {
        fprintf(stderr, "router_add_route: MAX_ROUTER_ROUTES exceeded\n");
        return;
    }
    if (fill_route(&router->routes[router->route_count], method, path, handler,
                    middlewares, middleware_count) != 0) {
        return; /* slot left unfilled and uncounted - nothing to free, it's part of the fixed array */
    }
    router->route_count++;
}

void router_get(Router *router, const char *path, Handler handler) {
    router_add_route(router, "GET", path, handler);
}

void router_post(Router *router, const char *path, Handler handler) {
    router_add_route(router, "POST", path, handler);
}

void router_get_mw(Router *router, const char *path, Handler handler,
                    const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "GET", path, handler, middlewares, middleware_count);
}

void router_post_mw(Router *router, const char *path, Handler handler,
                     const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "POST", path, handler, middlewares, middleware_count);
}

void router_put(Router *router, const char *path, Handler handler) {
    router_add_route(router, "PUT", path, handler);
}

void router_patch(Router *router, const char *path, Handler handler) {
    router_add_route(router, "PATCH", path, handler);
}

void router_delete(Router *router, const char *path, Handler handler) {
    router_add_route(router, "DELETE", path, handler);
}

void router_put_mw(Router *router, const char *path, Handler handler,
                    const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "PUT", path, handler, middlewares, middleware_count);
}

void router_patch_mw(Router *router, const char *path, Handler handler,
                      const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "PATCH", path, handler, middlewares, middleware_count);
}

void router_delete_mw(Router *router, const char *path, Handler handler,
                       const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "DELETE", path, handler, middlewares, middleware_count);
}

void router_head(Router *router, const char *path, Handler handler) {
    router_add_route(router, "HEAD", path, handler);
}

void router_options(Router *router, const char *path, Handler handler) {
    router_add_route(router, "OPTIONS", path, handler);
}

void router_head_mw(Router *router, const char *path, Handler handler,
                     const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "HEAD", path, handler, middlewares, middleware_count);
}

void router_options_mw(Router *router, const char *path, Handler handler,
                        const Middleware *middlewares, int middleware_count) {
    router_add_route_mw(router, "OPTIONS", path, handler, middlewares, middleware_count);
}

void router_use(Router *router, Middleware mw) {
    if (router->middleware_count >= MAX_MIDDLEWARES) {
        fprintf(stderr, "router_use: MAX_MIDDLEWARES exceeded\n");
        return;
    }
    router->middlewares[router->middleware_count++] = mw;
}

/* Normalizes a mount-point prefix (app_mount, app_serve_static): "" or "/"
 * alone means an unscoped/root mount - both app_use_prefix and
 * build_mounted_path below treat "" as "no prefix to add/match on". A
 * trailing slash (e.g. "/api/") is stripped so concatenating a route's own
 * leading-slash path doesn't double up ("/api//users"). Shared by both
 * mounting entry points rather than duplicated, since the rule is identical. */
static void normalize_mount_prefix(const char *prefix, char *out, size_t out_size) {
    if (prefix == NULL || prefix[0] == '\0' || strcmp(prefix, "/") == 0) {
        out[0] = '\0';
        return;
    }
    strncpy(out, prefix, out_size - 1);
    out[out_size - 1] = '\0';
    const size_t len = strlen(out);
    if (len > 0 && out[len - 1] == '/') {
        out[len - 1] = '\0';
    }
}

/* Builds the mounted path for one router route: prefix + route->path, except
 * a router route registered at "/" (the router's own root) mounts at the
 * prefix itself rather than "prefix/" - so router_get(router, "/", h) mounted
 * at "/api" matches "/api", not "/api/". prefix has already been normalized
 * by app_mount (no trailing slash, "" for an unscoped/root mount). */
static void build_mounted_path(char *out, size_t out_size, const char *prefix, const char *route_path) {
    if (strcmp(route_path, "/") == 0) {
        snprintf(out, out_size, "%s", prefix[0] != '\0' ? prefix : "/");
    } else {
        snprintf(out, out_size, "%s%s", prefix, route_path);
    }
}

void app_mount(App *app, const char *prefix, const Router *router) {
    char normalized_prefix[128];
    normalize_mount_prefix(prefix, normalized_prefix, sizeof(normalized_prefix));

    for (int i = 0; i < router->middleware_count; i++) {
        app_use_prefix(app, normalized_prefix, router->middlewares[i]);
    }

    for (int i = 0; i < router->route_count; i++) {
        const Route *route = &router->routes[i];
        char mounted_path[256];
        build_mounted_path(mounted_path, sizeof(mounted_path), normalized_prefix, route->path);
        app_add_route_mw(app, route->method, mounted_path, route->handler,
                          route->middlewares, route->middleware_count);
    }
}

void app_serve_static(App *app, const char *prefix, const char *root_dir) {
    char canonical_root[PATH_MAX];
    if (realpath(root_dir, canonical_root) == NULL) {
        fprintf(stderr, "app_serve_static: root directory \"%s\" does not exist, not registered\n", root_dir);
        return;
    }

    char normalized_prefix[128];
    normalize_mount_prefix(prefix, normalized_prefix, sizeof(normalized_prefix));

    char pattern[256];
    snprintf(pattern, sizeof(pattern), "%s/*", normalized_prefix);
    
    Route *route = malloc(sizeof(Route));
    if (route == NULL) {
        fprintf(stderr, "app_serve_static: out of memory, \"%s\" not registered\n", root_dir);
        return;
    }
    if (fill_route(route, "GET", pattern, NULL, NULL, 0) != 0) {
        free(route);
        return;
    }

    /* M6: sized to the canonical root, not PATH_MAX. Owned by the Route from here on - route_free
     * releases it on every path, including tree_insert's own failures and duplicates. */
    const size_t root_size = strlen(canonical_root) + 1;
    route->static_root = malloc(root_size);
    if (route->static_root == NULL) {
        fprintf(stderr, "app_serve_static: out of memory, \"%s\" not registered\n", root_dir);
        free(route);
        return;
    }
    memcpy(route->static_root, canonical_root, root_size);

    app_insert_route_struct(app, route);
}

/* Same "prefix matches path at a segment boundary" rule as chain_next's app-wide middleware match
 * (middleware.c: middleware_prefix_matches) - kept as its own small copy here rather than shared,
 * since app_body_limit_for_path is looked up directly (no Request yet to hand a shared helper) while
 * middleware_prefix_matches works off one, and the two moved independently is not worth a coupling. */
static int body_limit_prefix_matches(const char *prefix, const char *path) {
    if (prefix[0] == '\0' || (prefix[0] == '/' && prefix[1] == '\0')) {
        return 1;
    }
    const size_t prefix_len = strlen(prefix);
    if (strncmp(path, prefix, prefix_len) != 0) {
        return 0;
    }
    const char next = path[prefix_len];
    return next == '\0' || next == '/';
}

void app_use_body_limit(App *app, const char *prefix, size_t max_bytes) {
    if (app->body_limit_count >= MAX_BODY_LIMITS) {
        fprintf(stderr, "app_use_body_limit: MAX_BODY_LIMITS exceeded\n");
        return;
    }
    if (prefix == NULL) {
        prefix = "";
    }
    if (max_bytes > MAX_BODY_SIZE) {
        fprintf(stderr, "app_use_body_limit: %zu exceeds MAX_BODY_SIZE, clamped to %d\n",
                max_bytes, MAX_BODY_SIZE);
        max_bytes = MAX_BODY_SIZE;
    }
    BodyLimitEntry *entry = &app->body_limits[app->body_limit_count++];
    strncpy(entry->prefix, prefix, sizeof(entry->prefix) - 1);
    entry->prefix[sizeof(entry->prefix) - 1] = '\0';
    entry->max_bytes = max_bytes;
}

size_t app_body_limit_for_path(const App *app, const char *path) {
    size_t best_len = 0;
    size_t limit = MAX_BODY_SIZE;
    for (int i = 0; i < app->body_limit_count; i++) {
        const BodyLimitEntry *entry = &app->body_limits[i];
        if (!body_limit_prefix_matches(entry->prefix, path)) {
            continue;
        }
        const size_t entry_len = strlen(entry->prefix);
        if (entry_len >= best_len) {
            best_len = entry_len;
            limit = entry->max_bytes;
        }
    }
    return limit;
}

/* Advances *cursor past '/' separators and returns the next path segment
 * (pointer + length), or NULL at the end. Empty segments are skipped, so
 * "/a//b/" has the segments "a" and "b". Allocation-free and read-only. */
static const char *next_segment(const char **cursor, size_t *len_out) {
    const char *p = *cursor;
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        *cursor = p;
        return NULL;
    }
    const char *start = p;
    while (*p != '\0' && *p != '/') {
        p++;
    }
    *cursor = p;
    *len_out = (size_t)(p - start);
    return start;
}

int match_path(const char *pattern, const char *path, Request *req) {
    const char *pattern_cursor = pattern;
    const char *path_cursor = path;
    size_t pattern_len = 0;
    size_t path_len = 0;

    if (req != NULL) {
        req->param_count = 0;
    }

    const char *pattern_seg = next_segment(&pattern_cursor, &pattern_len);
    const char *path_seg = next_segment(&path_cursor, &path_len);

    while (pattern_seg != NULL && path_seg != NULL) {
        if (pattern_len == 1 && pattern_seg[0] == '*') {
            pattern_seg = next_segment(&pattern_cursor, &pattern_len);
            if (pattern_seg == NULL) {
                return 1; /* trailing "*": this segment and everything after it */
            }
            /* mid-pattern "*": consumes exactly one path segment, captures nothing */
            path_seg = next_segment(&path_cursor, &path_len);
            continue;
        }
        if (pattern_seg[0] == ':') {
            if (req != NULL && req->param_count < MAX_PARAMS) {
                const size_t name_len = pattern_len - 1;
                const size_t name_cap = sizeof(req->param_names[0]) - 1;
                const size_t value_cap = sizeof(req->param_values[0]) - 1;
                char *name_slot = req->param_names[req->param_count];
                char *value_slot = req->param_values[req->param_count];
                const size_t name_copy = name_len < name_cap ? name_len : name_cap;
                const size_t value_copy = path_len < value_cap ? path_len : value_cap;
                memcpy(name_slot, pattern_seg + 1, name_copy);
                name_slot[name_copy] = '\0';
                memcpy(value_slot, path_seg, value_copy);
                value_slot[value_copy] = '\0';
                req->param_count++;
            }
        } else if (pattern_len != path_len || memcmp(pattern_seg, path_seg, pattern_len) != 0) {
            return 0;
        }
        pattern_seg = next_segment(&pattern_cursor, &pattern_len);
        path_seg = next_segment(&path_cursor, &path_len);
    }

    /* Both must be fully consumed, otherwise one is longer than the other. */
    return pattern_seg == NULL && path_seg == NULL;
}

/* Total order over segment bytes (short-lexicographic: shared prefix compares first, shorter wins
 * ties) so children can be kept sorted and searched with binary search instead of a linear scan
 * (P7: was O(siblings) per segment, 8.4 us at 5,000 siblings). */
static int compare_seg(const char *a, size_t a_len, const char *b, size_t b_len) {
    size_t min_len = a_len < b_len ? a_len : b_len;
    int c = memcmp(a, b, min_len);
    if (c != 0) return c;
    if (a_len < b_len) return -1;
    if (a_len > b_len) return 1;
    return 0;
}

/* Binary search current->children (kept sorted by compare_seg) for seg. Returns the index of an
 * exact match via *out_idx and 1, or the sorted insertion point via *out_idx and 0. */
static int find_child(PatriciaNode *const *children, int child_count, const char *seg, size_t seg_len,
                       int *out_idx) {
    int low = 0, high = child_count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        int c = compare_seg(seg, seg_len, children[mid]->prefix, (size_t)children[mid]->prefix_len);
        if (c == 0) {
            *out_idx = mid;
            return 1;
        }
        if (c < 0) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    *out_idx = low;
    return 0;
}

/* S12: returns NULL (nothing partially allocated - a failed prefix malloc frees the node before
 * returning) on either allocation failing, instead of handing the caller a node with a dangling or
 * missing prefix. */
static PatriciaNode *create_patricia_node(const char *prefix, size_t prefix_len, NodeType type) {
    PatriciaNode *n = calloc(1, sizeof(PatriciaNode));
    if (n == NULL) {
        return NULL;
    }
    if (prefix_len > 0) {
        n->prefix = malloc(prefix_len + 1);
        if (n->prefix == NULL) {
            free(n);
            return NULL;
        }
        memcpy(n->prefix, prefix, prefix_len);
        n->prefix[prefix_len] = '\0';
        n->prefix_len = (int)prefix_len;
    }
    n->type = type;
    return n;
}

/* S12: every node allocation and the children realloc are checked; on failure this frees `route` (route_free)
 * (never inserted) and logs, rather than dereferencing a NULL node or leaking `route`. Any tree
 * structure already linked in before the failing allocation is harmless - a PatriciaNode with no
 * route is already a normal, valid internal node, and it's still shared by any other route that
 * needs that same path prefix. */
static void tree_insert(PatriciaNode **root_ptr, const char *path, Route *route) {
    if (*root_ptr == NULL) {
        *root_ptr = create_patricia_node("", 0, NODE_STATIC);
        if (*root_ptr == NULL) {
            fprintf(stderr, "route registration: out of memory, route \"%s\" not registered\n", path);
            route_free(route);
            return;
        }
    }

    PatriciaNode *current = *root_ptr;
    const char *cursor = path;
    size_t seg_len;
    const char *seg = next_segment(&cursor, &seg_len);

    while (seg != NULL) {
        NodeType type = NODE_STATIC;
        if (seg_len == 1 && seg[0] == '*') {
            const char *lookahead = cursor;
            size_t next_len;
            if (next_segment(&lookahead, &next_len) == NULL) {
                type = NODE_CATCH_ALL;
            } else {
                type = NODE_PARAM;
            }
        } else if (seg[0] == ':') {
            type = NODE_PARAM;
        }

        PatriciaNode *next_node = NULL;

        if (type == NODE_STATIC) {
            int idx;
            if (find_child(current->children, current->child_count, seg, seg_len, &idx)) {
                next_node = current->children[idx];
            } else {
                if (current->child_count >= current->child_cap) {
                    int new_cap = current->child_cap == 0 ? 4 : current->child_cap * 2;
                    PatriciaNode **new_children = realloc(current->children, (size_t)new_cap * sizeof(PatriciaNode *));
                    if (new_children == NULL) {
                        fprintf(stderr, "route registration: out of memory, route \"%s\" not registered\n", path);
                        route_free(route);
                        return;
                    }
                    current->children = new_children;
                    current->child_cap = new_cap;
                }
                next_node = create_patricia_node(seg, seg_len, NODE_STATIC);
                if (next_node == NULL) {
                    fprintf(stderr, "route registration: out of memory, route \"%s\" not registered\n", path);
                    route_free(route);
                    return;
                }
                memmove(&current->children[idx + 1], &current->children[idx],
                        (size_t)(current->child_count - idx) * sizeof(PatriciaNode *));
                current->children[idx] = next_node;
                current->child_count++;
            }
        } else if (type == NODE_PARAM) {
            if (!current->param_child) {
                if (seg[0] == ':') {
                    current->param_child = create_patricia_node(seg + 1, seg_len - 1, NODE_PARAM);
                } else {
                    current->param_child = create_patricia_node("*", 1, NODE_PARAM);
                }
                if (current->param_child == NULL) {
                    fprintf(stderr, "route registration: out of memory, route \"%s\" not registered\n", path);
                    route_free(route);
                    return;
                }
            }
            next_node = current->param_child;
        } else if (type == NODE_CATCH_ALL) {
            if (!current->catch_all_child) {
                current->catch_all_child = create_patricia_node("*", 1, NODE_CATCH_ALL);
                if (current->catch_all_child == NULL) {
                    fprintf(stderr, "route registration: out of memory, route \"%s\" not registered\n", path);
                    route_free(route);
                    return;
                }
            }
            next_node = current->catch_all_child;
        }

        current = next_node;
        seg = next_segment(&cursor, &seg_len);
    }

    if (!current->route) {
        current->route = route;
    } else {
        fprintf(stderr, "Warning: Route %s already registered, ignoring duplicate\n", path);
        route_free(route);
    }
}

static const Route *tree_search_recursive(PatriciaNode *node, const char *cursor, const char *seg, size_t seg_len, Request *req) {
    if (seg == NULL) {
        return node->route;
    }
    
    int idx;
    if (find_child(node->children, node->child_count, seg, seg_len, &idx)) {
        const char *next_cursor = cursor;
        size_t next_seg_len;
        const char *next_seg = next_segment(&next_cursor, &next_seg_len);
        const Route *res = tree_search_recursive(node->children[idx], next_cursor, next_seg, next_seg_len, req);
        if (res) return res;
    }
    
    if (node->param_child) {
        int saved_param_count = req ? req->param_count : 0;
        if (req && req->param_count < MAX_PARAMS && node->param_child->prefix_len > 0 && node->param_child->prefix[0] != '*') {
            const size_t name_len = node->param_child->prefix_len;
            const size_t name_cap = sizeof(req->param_names[0]) - 1;
            const size_t value_cap = sizeof(req->param_values[0]) - 1;
            const size_t name_copy = name_len < name_cap ? name_len : name_cap;
            const size_t value_copy = seg_len < value_cap ? seg_len : value_cap;
            
            char *name_slot = req->param_names[req->param_count];
            char *value_slot = req->param_values[req->param_count];
            memcpy(name_slot, node->param_child->prefix, name_copy);
            name_slot[name_copy] = '\0';
            memcpy(value_slot, seg, value_copy);
            value_slot[value_copy] = '\0';
            req->param_count++;
        }
        
        const char *next_cursor = cursor;
        size_t next_seg_len;
        const char *next_seg = next_segment(&next_cursor, &next_seg_len);
        const Route *res = tree_search_recursive(node->param_child, next_cursor, next_seg, next_seg_len, req);
        if (res) return res;
        
        if (req) req->param_count = saved_param_count;
    }
    
    if (node->catch_all_child) {
        return node->catch_all_child->route;
    }
    
    return NULL;
}

static const Route *tree_search(PatriciaNode *node, const char *path, Request *req) {
    if (!node) return NULL;
    const char *cursor = path;
    size_t seg_len;
    const char *seg = next_segment(&cursor, &seg_len);
    return tree_search_recursive(node, cursor, seg, seg_len, req);
}

const Route *match_route(const App *app, Request *req) {
    if (req != NULL) {
        req->param_count = 0;
    }
    
    for (int i = 0; i < app->method_tree_count; i++) {
        if (strcmp(app->method_trees[i].method, req->method) == 0) {
            const Route *route = tree_search(app->method_trees[i].tree, req->path, req);
            if (route) return route;
            break;
        }
    }
    
    if (strcmp(req->method, "HEAD") == 0) {
        if (req != NULL) {
            req->param_count = 0;
        }
        for (int i = 0; i < app->method_tree_count; i++) {
            if (strcmp(app->method_trees[i].method, "GET") == 0) {
                const Route *route = tree_search(app->method_trees[i].tree, req->path, req);
                if (route) return route;
                break;
            }
        }
    }
    
    return NULL;
}

static int tree_has_match(PatriciaNode *node, const char *path) {
    return tree_search(node, path, NULL) != NULL;
}

int match_route_allowed_methods(const App *app, const Request *req, char *allowed, size_t allowed_size) {
    int seen_count = 0;
    allowed[0] = '\0';
    
    for (int i = 0; i < app->method_tree_count; i++) {
        if (tree_has_match(app->method_trees[i].tree, req->path)) {
            if (allowed[0] != '\0') {
                strncat(allowed, ", ", allowed_size - strlen(allowed) - 1);
            }
            strncat(allowed, app->method_trees[i].method, allowed_size - strlen(allowed) - 1);
            seen_count++;
        }
    }
    
    return seen_count;
}

void free_patricia_tree(PatriciaNode *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++) {
        free_patricia_tree(node->children[i]);
    }
    if (node->children) free(node->children);
    if (node->param_child) free_patricia_tree(node->param_child);
    if (node->catch_all_child) free_patricia_tree(node->catch_all_child);
    if (node->prefix) free(node->prefix);
    if (node->route) route_free(node->route);
    free(node);
}

void app_free_routes(App *app) {
    for (int i = 0; i < app->method_tree_count; i++) {
        free_patricia_tree(app->method_trees[i].tree);
        app->method_trees[i].tree = NULL;
    }
    app->method_tree_count = 0;
}

const char *req_get_param(const Request *req, const char *name) {
    for (int i = 0; i < req->param_count; i++) {
        if (strcmp(req->param_names[i], name) == 0) {
            return req->param_values[i];
        }
    }
    return NULL;
}

