# Builds a native-arch `wrk` image for docker_stress_test.sh. Built from source rather than pulled
# from a third-party image (e.g. williamyeh/wrk, amd64-only): on an Apple Silicon host, a pulled
# amd64 image runs under QEMU emulation, which makes the *load generator* the bottleneck and
# understates the server's real throughput - exactly the measurement the whole point of running
# inside Docker (bypassing Docker Desktop's localhost proxy for accurate Linux-to-Linux numbers) is
# trying to get right. Compiling here instead builds for whatever platform `docker build` runs on
# (matching this repo's own vendored-dependency convention - see lib/vendor/), and Docker's layer
# cache keeps repeat runs fast: only the first build after a change pays the compile cost.
FROM alpine:3.20 AS builder
RUN apk add --no-cache build-base perl openssl-dev openssl-libs-static zlib-dev zlib-static git ca-certificates linux-headers
RUN git clone --depth 1 https://github.com/wg/wrk.git /wrk
WORKDIR /wrk
RUN make -j"$(nproc)"

FROM alpine:3.20
RUN apk add --no-cache libgcc
COPY --from=builder /wrk/wrk /usr/local/bin/wrk
ENTRYPOINT ["/usr/local/bin/wrk"]
