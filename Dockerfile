# syntax=docker/dockerfile:1

# ---- Build stage -------------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY web ./web

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCODESCOPE_BUILD_TESTS=OFF \
        -DCODESCOPE_BUILD_BENCH=OFF \
 && cmake --build build \
 && strip build/codescope

# ---- Runtime stage -----------------------------------------------------------
FROM ubuntu:24.04

RUN useradd --system --create-home --uid 10001 codescope \
 && mkdir -p /data \
 && chown codescope /data

COPY --from=build /src/build/codescope /usr/local/bin/codescope
COPY web /app/web

USER codescope
EXPOSE 8080
VOLUME ["/data"]

# Mount the repository to search at /repo (read-only is fine); the index is
# saved in /data so restarts only re-index changed files.
ENTRYPOINT ["codescope"]
CMD ["serve", "/repo", "--host", "0.0.0.0", "--port", "8080", "--index", "/data/index.bin", "--web", "/app/web"]
