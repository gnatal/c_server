# Multi-stage build for lightweight Linux container image
FROM alpine:3.20 AS builder

# Install build toolchain
RUN apk add --no-cache gcc musl-dev make

WORKDIR /app

# Copy source code
COPY . .

# Compile static library, application, and test binaries on native Linux
RUN make clean && make && make test

# Minimal runtime image
FROM alpine:3.20

WORKDIR /app

# Copy compiled binary and static assets from builder
COPY --from=builder /app/build/bin/cexpress /app/cexpress
COPY --from=builder /app/app/public /app/app/public

ENV PORT=8080
EXPOSE 8080

CMD ["/app/cexpress"]
