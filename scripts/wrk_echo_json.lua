-- wrk load-test script for POST /echo/json.
-- Usage: wrk -t8 -c5000 -d15s -s scripts/wrk_echo_json.lua http://127.0.0.1:8080/echo/json

wrk.method = "POST"
wrk.body   = '{"name":"Ada","active":true,"tags":["x","y","z"],"meta":{"age":36,"note":null}}'
wrk.headers["Content-Type"] = "application/json"
