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

# Init submodule RandomY — tidak bergantung .git di build context
# (bekerja juga saat build dari archive/ZIP tanpa folder .git)
RUN if [ -d external/RandomY/.git ] || [ -f external/RandomY/src/randomx.h ]; then \
        git submodule update --init --recursive; \
    else \
        git clone https://github.com/core-coin/RandomY.git external/RandomY && \
        git -C external/RandomY checkout c9d185a055b7604e8d58059031e0e33dfe577cc5; \
    fi

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

# Fallback config (multi-pool failover) kalau env vars di-unset
# pool.cfg dapat berisi banyak pool; miner pindah otomatis bila pool mati
COPY pool.cfg .

RUN chown -R miner:miner /miner

USER miner

# Default env vars (override at runtime via docker-compose or Akash)
ENV WALLET=cb23d6d8557e776f5ff9ab6a7fb7f59a3d385245fa7a
ENV POOL=sg.catchthatrabbit.com:8008
ENV WORKER=pool
ENV THREADS=
# FULL_MEM tidak diset -> auto-detect dari RAM container (>=3.5GiB = full)
ENV LARGE_PAGES=0

ENTRYPOINT ["./miner-saya"]
