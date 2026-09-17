# Improvements

Notes on design decisions worth revisiting later, written up when the decision was
made rather than reconstructed after the fact. See `pending.txt` for the full gap
list against Express; this file is for gaps that deserve more explanation than a
bullet point, or where a real follow-up improvement was deliberately deferred.

## Body buffering vs. true streaming

**What this server does today:** a request body can be up to `MAX_BODY_SIZE` (10 MiB,
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
   chunked encoding) — this server can now decode a chunked body (`lib/CLAUDE.md`,
   "Chunked Transfer-Encoding"), but only in the same *buffer-then-call-the-handler*
   shape `Content-Length` bodies already get: `chunked_body_scan` waits for the
   whole thing to arrive before `chunked_body_decode` ever runs. True incremental
   streaming would still mean handing chunks to a handler as each one completes,
   rather than after the terminating `0\r\n\r\n`.
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

## HTTP/1.1 only — no HTTP/2 or HTTP/3

**What this server does today:** CExpress speaks plaintext HTTP/1.1 exclusively.
`parse_http_request` (`lib/http_parser.c`) expects a textual request line
(`sscanf("%s %s %s", method, path, version)`), the connection model is one request
handled at a time per TCP connection (`handle_readable` buffers until a single
request is complete, dispatches it, then waits for the next one), and there's no
TLS at all yet (`pending.txt`, section 6). That last point matters more than it
might seem: in real-world deployments, HTTP/2 is negotiated via ALPN *inside the
TLS handshake* — browsers essentially never speak HTTP/2 over a cleartext
connection (the `h2c` cleartext-upgrade path exists in the spec but was dropped by
every major browser), so this server is missing the prerequisite for HTTP/2 before
even getting to HTTP/2 itself.

**Why this isn't "a bigger/different parser" the way chunked encoding was:**
Chunked-encoding support (above) slotted into the existing model because HTTP/1.1
framing stayed textual and one-request-at-a-time. HTTP/2 changes the framing to a
binary, length-prefixed frame format (`HEADERS`, `DATA`, `SETTINGS`, ...), compresses
headers with a stateful codec (HPACK — headers can reference and update a shared
dynamic table across the connection, so frames can't be parsed independently of
prior ones), and multiplexes many logical *streams* concurrently over one TCP
connection with flow control and stream-priority semantics. That last part is a
superset of the "true streaming" redesign described earlier in this file: instead
of one in-flight body per connection, `handle_readable`'s state machine would need
to track many concurrent partial reads/writes per connection, each belonging to a
different logical request, all sharing one socket buffer.

HTTP/3 is a bigger jump still: it runs over **QUIC, which is UDP-based, not TCP**.
`connection.c` is built entirely around `socket(AF_INET, SOCK_STREAM, ...)` and a
kqueue loop watching TCP-oriented `EVFILT_READ`/`EVFILT_WRITE` events per
connection fd — HTTP/3 support wouldn't extend that loop, it would mean standing up
an entirely separate UDP/QUIC transport stack alongside it (QUIC's own connection
IDs, stream multiplexing, loss recovery/congestion control, and TLS 1.3 baked
directly into the QUIC handshake — QUIC has no cleartext mode at all, unlike
HTTP/1.1 or even `h2c`).

**What real support would require, roughly:**
1. **TLS termination first** (`pending.txt`, section 6) — a prerequisite for HTTP/2
   in practice, not an independent gap to schedule alongside it.
2. **HTTP/2:** a binary frame reader/writer, an HPACK encoder/decoder (stateful,
   security-sensitive — HPACK implementation bugs have caused real CVEs elsewhere),
   and a per-connection multiplexed-stream state machine replacing the current
   one-request-at-a-time model in `handle_readable`/`dispatch`.
3. **HTTP/3:** a QUIC implementation (almost certainly a third-party library, not
   something to hand-roll — QUIC's loss recovery and congestion control alone are
   substantial) plus a parallel UDP-based accept/read/write loop that the existing
   kqueue-over-TCP code has no natural extension point for.

**Verdict:** out of scope for now, and bigger in scope than the streaming-body
redesign above — it touches the transport layer, connection state machine, and
adds an entire security-sensitive compression codec (HTTP/2), or an entirely
separate protocol stack (HTTP/3). TLS and true request/response streaming are
smaller, more self-contained wins that would need to land first regardless; this is
worth revisiting only if CExpress's ambitions grow well past "Express-like
developer experience in C" toward being a general-purpose production web server.
