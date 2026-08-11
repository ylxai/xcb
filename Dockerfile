# ============================================================
# Stage 1: Build miner-saya from source
# ============================================================
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    libssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

# ============================================================
# Stage 2: Minimal runtime image
# ============================================================
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -r -s /bin/false miner

WORKDIR /miner

# Copy binary from builder
COPY --from=builder /build/build/miner-saya .

RUN chown -R miner:miner /miner

USER miner

# Default env vars (override at runtime via docker-compose or Akash)
ENV WALLET=cb23d6d8557e776f5ff9ab6a7fb7f59a3d385245fa7a
ENV POOL=sg.catchthatrabbit.com:8008
ENV WORKER=pool
ENV THREADS=
ENV FULL_MEM=
ENV LARGE_PAGES=0

ENTRYPOINT ["./miner-saya"]
