# lib/json/ — JSON value, parser, writer

## Architecture
Three-file split around one recursive `JsonValue` tree (`json_types.h`):
`json_parser.c` (bytes → tree), `json_value.c` (tree accessors + `json_free`),
`json_writer.c` (tree → bytes). `json.h` is the only public surface; `json_types.h`
holds every `typedef`/`struct` the three `.c` files share or use internally
(including `StrBuf`, the growable buffer `json_writer.c` serializes into — kept in
the header rather than the `.c` file even though nothing outside `json_writer.c`
uses it).

`json_parse` is a single-pass recursive-descent parser over a `JsonParser` cursor
(`pos`/`len` into the input, no separate tokenizing pass). It does no I/O; callers
(`app/handlers.c`) are responsible for getting bytes into a NUL-terminated buffer
first.

## Memory lifecycle
- Every `JsonValue` (root or nested) is heap-allocated (`value_new` → `calloc`) and
  owned by its parent container once attached; the whole tree is released by one
  `json_free(root)` call, which recurses into `JSON_ARRAY`/`JSON_OBJECT` children
  and frees `JSON_STRING` payloads and object member keys along the way. Callers
  never free nested values directly.
- On a parse error partway through an array/object, the partially-built parent
  `JsonValue` (which already owns everything parsed so far) is passed to
  `json_free` before returning `NULL`, so a failed parse never leaks the nodes it
  did manage to allocate.
- `parse_string_raw` returns a `malloc`'d, unescaped C string. Every call site
  either attaches it to a `JsonValue`/`JsonMember` (freed later via `json_free`) or,
  on a subsequent error before that attachment happens, `free`s it directly at the
  point of failure.
- `json_stringify` returns one `malloc`'d (via `StrBuf`'s `realloc`-doubling)
  NUL-terminated string; the caller owns it and must `free()` it (documented on the
  declaration in `json.h`).

## Testing
`tests/test_json.c` covers primitives, nested structures, a parse→stringify→parse round
trip, and syntax-error rejection.
