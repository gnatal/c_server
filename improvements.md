# Improvements

Notes on design decisions worth revisiting later, written up when the decision was
made rather than reconstructed after the fact. See `pending.txt` for the full gap
list against Express; this file is for gaps that deserve more explanation than a
bullet point, or where a real follow-up improvement was deliberately deferred.

## Body buffering vs. true streaming

**What this server does today:** a request body can be up to `MAX_BODY_SIZE` (1 MiB,
`lib/app_types.h`) without being rejected — `Connection.in_buf` grows via `realloc`
to fit it, and `Request.body` is a `malloc`'d buffer sized to the body's exact
length. But the `Handler` a route registers still only runs once the *entire* body
has arrived and been buffered — `void (*Handler)(const Request *req, Response *res)`
is synchronous, and `req->body` is always a complete, ready-to-read C string by the
time a handler sees it. See `lib/CLAUDE.md` ("Body buffering") for the implementation
details.

**How this compares to Express:** it's easy to assume Express "streams" request
bodies to your route handler, since `req` in Express is technically a Node.js
readable stream. But that's not how the vast majority of Express apps actually
work. What you interact with as `req.body` comes from `express.json()` /
`express.urlencoded()` (or `body-parser` before those were folded into Express
core) — and that middleware **buffers the whole body into memory** before ever
calling `next()`:

```js
app.use(express.json({ limit: '1mb' })); // buffers up to 1mb, then calls next()

app.post('/users', (req, res) => {
  req.body; // already a fully-parsed object by the time this runs
});
```

So for the common case — JSON/form APIs — this server's behavior (buffer up to a
configurable cap, then call the handler synchronously with a complete body) is
already a faithful match for how real Express apps behave, not a shortcut. True
streaming in Express (`req.pipe(...)`, reading `req` directly as a stream) is
reserved for the minority case: file uploads, `multipart/form-data` via `multer`,
proxying a request body straight through to another service.

## If time allows: true incremental body streaming

For the minority case above — large file uploads, or piping a body straight through
without ever holding the whole thing in memory — this server would need a genuinely
different design, not just a bigger buffer:

1. **A streaming-capable `Handler` variant.** The current `Handler` signature
   assumes a complete `Request` up front. Supporting incremental delivery means
   either a second handler type that receives body chunks via a callback (e.g.
   `void (*StreamHandler)(const Request *req_without_body, const char *chunk, size_t
   len, int is_final, Response *res)`), or restructuring `dispatch` so a handler can
   register a chunk callback before the body starts arriving.
2. **`Transfer-Encoding: chunked` support.** Streaming is far more useful when the
   client doesn't have to know the body's length upfront (that's the whole point of
   chunked encoding) — right now this server has no chunked-decoding logic at all
   (`pending.txt`, section 3). True streaming without chunked support only helps
   when `Content-Length` is already known, which is a narrower win.
3. **Event-loop changes.** `handle_readable` currently treats "a complete request
   arrived" as the one moment it hands control to routing/dispatch. Streaming means
   dispatch has to happen *before* the body finishes arriving, and subsequent
   `EVFILT_READ` events need to route bytes to an in-progress handler invocation
   instead of accumulating into `conn->in_buf` for a single one-shot parse. This is
   a meaningfully different connection state machine, not an incremental patch.
4. **Backpressure.** A slow consumer (e.g. writing an uploaded file to disk) needs a
   way to signal "pause reading more off the socket" — this server has no
   backpressure concept anywhere today since everything is currently either
   "buffer it all" or "write it all."

This is a substantial redesign, not a follow-up patch — it changes the `Handler`
API every existing route handler uses, touches the event loop's core state machine,
and only pays for itself once there's an actual use case (large uploads, proxying)
that the current buffered-body approach can't serve. Worth doing if this project
ever needs to handle uploads or streaming proxying; not worth doing preemptively for
JSON API routes, which are what this server is built around today.
