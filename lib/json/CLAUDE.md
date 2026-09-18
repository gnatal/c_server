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

## Building a tree by hand
`json_new_string`/`json_new_object`/`json_new_number`/`json_new_bool`/
`json_new_array`/`json_object_set`/`json_array_append` (`json_value.c`) let a
caller construct a `JsonValue` tree without going through `json_parse` - for
serializing data the app already has in memory (e.g. `app/handlers.c:
handler_search` turning parsed query-string params, `lib/http_parser.c`,
into a JSON object; `app/db.c`/`app/handlers.c: todo_to_json` turning a
`Todo` row into `{"id", "title", "done", "created_at", "updated_at"}`,
`app/CLAUDE.md`). They follow the same allocation shape as the parser's
internal `value_new`/growth pattern (`json_parser.c`) but are a separate,
independent implementation - `value_new` is `static` to `json_parser.c` and
not shared - since keeping the builders in `json_value.c` alongside the
other tree-level operations reads better than reaching into the parser file
for them. `json_object_set`/`json_array_append` always consume the `value`
they're given, attaching it on success or calling `json_free(value)` on
failure (bad `object`/`array`/`key`, or an allocation failure) - a caller
never frees a `JsonValue` it has already handed to either function, checking
the return value for success/failure only. Neither builder function ever
needs `key`/`value` to stay alive past the call - `json_new_string` and
`json_object_set` copy their string input rather than retaining the
caller's pointer (`copy_string`, a malloc+memcpy helper used in place of
`strdup` - a POSIX extension, not portable libc - or `strcpy`, banned
project-wide); `json_new_number`/`json_new_bool` copy their scalar argument
by value, same as the parser itself does for `JSON_NUMBER`/`JSON_BOOL`
nodes.

## Memory lifecycle
- Every `JsonValue` (root or nested) is heap-allocated (`value_new` in
  `json_parser.c`, or the builders above in `json_value.c` - both just `calloc`)
  and owned by its parent container once attached; the whole tree is released by
  one `json_free(root)` call, which recurses into `JSON_ARRAY`/`JSON_OBJECT`
  children and frees `JSON_STRING` payloads and object member keys along the way.
  Callers never free nested values directly.
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
