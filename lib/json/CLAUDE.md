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

## Emitting JSON: `JsonWriter` (preferred)
`jw_*` (`json.h`, state in `JsonWriter`, `json_types.h`; implementation in `json_writer.c`) appends a
document straight into a growable buffer in document order, with no tree. Commas and quoting are
automatic; strings are escaped in runs (one `memcpy` per clean span). Protocol: inside an object every
value is preceded by `jw_key`; inside an array or at top level there is no key. Misuse (value without
key, key outside an object, unbalanced end, second top-level value, nesting past
`JSON_WRITER_MAX_DEPTH`, out of memory) sets a sticky `failed` flag; every later call is a no-op, and
`jw_ok` / `jw_data` / `jw_len` expose the result only when the document is complete and valid
(`jw_data` is `NULL` otherwise). Ownership: the writer owns its buffer until `jw_free` (idempotent, safe
after failure); `jw_data` is NUL-terminated and valid until then. About 4x faster than the tree path for
a 20-row list (`make bench`), and there is no ownership transfer to get wrong.

**Number formatting** (`append_number`, shared by `json_stringify` and `jw_double`): integers with
`|value| < 2^53` print exactly as integers; other finite doubles print with `%.17g` (round-trips);
NaN and infinity print `null`. The previous `%g` (6 significant digits) turned `1234567` into
`1.23457e+06`. `jw_int` prints any `long long` exactly, including `LLONG_MIN`. No `<math.h>`, so no `-lm`.

## Building a tree by hand (editing or forwarding parsed documents)
`json_new_string`/`json_new_object`/`json_new_number`/`json_new_bool`/`json_new_array`/`json_object_set`/
`json_array_append` (`json_value.c`) construct a `JsonValue` tree without `json_parse`: useful to modify or
forward a parsed document. To emit JSON from application data use `JsonWriter` above (`app/handlers.c` does).
`json_object_set` / `json_array_append` always consume the `value` they are given: attached on success,
`json_free(value)` on failure (bad `object` / `array` / `key`, or allocation failure), so a caller never
frees a value it has handed over and only checks the return code. `json_new_string` and `json_object_set`
copy their string input (`copy_string`, malloc+memcpy: `strdup` is POSIX-only and `strcpy` is banned here);
`json_new_number` / `json_new_bool` copy scalars by value. Builders are a separate implementation from the
parser's internal `value_new` (which is `static` to `json_parser.c`); each `json_object_set` /
`json_array_append` grows its container by one element with `realloc`, fine for small documents.

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
- `JsonWriter` buffer: `StrBuf.data` grown by `realloc` doubling from 2 KiB; freed only by `jw_free`.

## Testing
`tests/test_json.c` covers primitives, nested structures, a parse→stringify→parse round
trip, syntax-error rejection, number formatting (regression for the `%g` bug, precision
round-trip, NaN/infinity), the writer (a full document, escaping, top-level scalars, every misuse
path, depth limit, byte-identical output versus `json_stringify`).
