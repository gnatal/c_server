# Multi-stage build for lightweight Linux container image
FROM alpine:3.20 AS builder

# Install build toolchain and OpenSSL development headers
RUN apk add --no-cache gcc musl-dev make openssl-dev openssl-libs-static sqlite-dev liburing-dev linux-headers

WORKDIR /app

# Copy source code
COPY . .

# Compile static library and application binaries on native Linux
RUN make clean && make all && make demo

# Minimal runtime image
FROM alpine:3.20

RUN apk add --no-cache libssl3 libcrypto3 sqlite-libs liburing

WORKDIR /app

# Copy compiled binary and static assets from builder
COPY --from=builder /app/examples/todo_sqlite/cexpress_demo /app/cexpress_demo
COPY --from=builder /app/examples/todo_sqlite/public /app/public

ENV PORT=8080
EXPOSE 8080

CMD ["/app/cexpress_demo"]
