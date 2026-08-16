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

# Init submodule (context harus berisi .gitmodules + .git dari submodule)
RUN git submodule update --init --recursive

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
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -r -s /bin/false miner

WORKDIR /miner

# Copy binary from builder
COPY --from=builder /build/build/miner-saya .

# Fallback config TANPA wallet (wallet wajib via env/secret —
# jangan pernah bake secret ke image; siapa pun yang pull image
# tidak boleh tahu wallet kamu).
COPY pool.cfg.example /miner/pool.cfg

RUN chown -R miner:miner /miner

USER miner

# Default env vars — TANPA WALLET (wajib di-set runtime via
# docker run -e WALLET=... atau Akash/k8s secret).
ENV POOL=sg.catchthatrabbit.com:8008
ENV WORKER=pool
ENV THREADS=
ENV FULL_MEM=
ENV LARGE_PAGES=0
ENV LOG_LEVEL=1

ENTRYPOINT ["./miner-saya"]