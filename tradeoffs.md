# Tradeoffs: Arena Allocator and yyjson Integration

This document outlines the architectural decisions and tradeoffs made when optimizing CExpress for maximum performance, specifically focusing on the replacement of the custom JSON parser with `yyjson`, and the integration of a per-connection Bump Allocator (Arena).

## 1. Per-Connection Arena Allocator (Bump Allocator)

We introduced a custom Arena allocator in `lib/arena.c` to handle all allocations during the lifecycle of an HTTP request/response.

### **The Decision**
Instead of relying on the system `malloc`/`free` for dynamic allocations (e.g., request body, response buffers, string concatenations), we allocate a static 64KB buffer per connection. Memory is allocated by simply incrementing a pointer (`bump allocation`). At the end of the request lifecycle, the entire arena is reset in `O(1)` time by resetting the offset to `0`.

### **Pros**
* **Blazing Fast Allocations**: Allocating memory is just pointer arithmetic. There is no searching for free blocks or lock contention typically found in general-purpose allocators.
* **Zero Fragmentation**: Because memory is allocated linearly and freed all at once, memory fragmentation is completely eliminated.
* **No Memory Leaks**: We don't need to track and `free()` individual allocations. Resetting the arena guarantees everything is cleaned up instantly, greatly simplifying the error-handling paths and middleware lifecycle.

### **Cons / Tradeoffs**
* **Memory Footprint**: Each connection holds a pre-allocated 64KB buffer. If the server handles 10,000 concurrent idle connections, this instantly consumes ~640MB of memory, regardless of whether the connections are actively sending data.
* **Large Allocations Overhead**: For allocations exceeding the 64KB limit, the arena must fall back to standard `malloc` (managing it in a linked list of "large blocks"). While these are cleaned up automatically on arena reset, relying heavily on large allocations defeats the bump allocator's performance benefits.
* **No `free` or `realloc`**: Memory cannot be freed individually. If an endpoint allocates many small strings in a loop and discards them, the memory remains occupied until the request completes.

## 2. Replacing Custom JSON Parser with `yyjson`

We removed the internal `JsonWriter` and `json_new_*` AST implementation in favor of `yyjson`, a high-performance JSON library. 

### **The Decision**
We integrated `yyjson` and hooked it directly into our Arena allocator using `yyjson_alc`.

### **Pros**
* **Unmatched Performance**: `yyjson` is widely considered the fastest JSON library in C. It uses advanced SIMD instructions (if available) and highly optimized parsing loops.
* **Synergy with Arena**: By pointing `yyjson`'s memory allocator to our Arena allocator, JSON AST construction is essentially free (just bump pointer increments).
* **Robustness**: The library is heavily fuzzed and tested against Edge cases in the JSON specification, whereas custom-built parsers often suffer from obscure bugs.

### **Cons / Tradeoffs**
* **External Dependency**: Adds an external vendor library (`lib/vendor/yyjson`), increasing the codebase footprint and slightly increasing binary size.
* **API Complexity**: `yyjson` has a more complex and verbose API compared to the minimal internal AST builder we had (e.g., managing mutable docs vs immutable docs, `yyjson_mut_obj_add_int`).
* **Memory Usage**: `yyjson` creates an in-memory Document Object Model (DOM). For massive JSON payloads, this requires significant memory overhead compared to stream-based (SAX) parsing.

---

### **Conclusion**
The combination of `yyjson` with a Bump Allocator perfectly aligns with the goal of "maximum speed possible." The system is optimized to eliminate `malloc` bottlenecks in the hot path, resulting in ultra-low latency and high throughput for small-to-medium JSON APIs. The primary tradeoff is a statically higher baseline memory footprint per connection.

## 3. PicoHTTPParser Integration

We replaced our custom HTTP parser with `picohttpparser`, a highly optimized, strict RFC-compliant HTTP parser.

### **The Decision**
While our custom parser was fast, it was not SIMD optimized or heavily unrolled like `picohttpparser`. Switching to a battle-tested parser improves both security and peak performance for parsing HTTP headers.

### **Pros**
* **Security & Correctness**: The parser strictly rejects malformed requests, mitigating HTTP request smuggling vulnerabilities and ensuring RFC compliance.
* **Speed**: Heavily optimized, capable of parsing headers significantly faster, which is visible in high connection concurrency tests.

### **Cons / Tradeoffs**
* **Strictness**: The parser expects strict compliance (e.g., `\r\n` line endings, valid HTTP versions). Older, non-compliant HTTP/0.9 clients or badly behaved clients will be outright rejected.
