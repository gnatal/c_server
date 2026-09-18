-- wrk load-test script for POST /api/todos (Todo CRUD demo, app/).
-- Usage: wrk -t8 -c1000 -d15s -s scripts/wrk_create_todo.lua http://127.0.0.1:8080/api/todos
--
-- API_KEY env var overrides the bearer token (must match the server's
-- configured key - app/middlewares.c: mw_authenticate_get_key - or every
-- request 401s instead of exercising the real write path).

local api_key = os.getenv("API_KEY") or "my-secret-api-key"

wrk.method = "POST"
wrk.body   = '{"title":"stress test todo"}'
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Authorization"] = "Bearer " .. api_key
