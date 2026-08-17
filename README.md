# miner-saya — Core Coin (XCB) RandomY Miner

Miner C++ untuk **Core Coin (XCB)** menggunakan algoritma **RandomY** (fork RandomX v1.2.1).
Dibangun dari nol — **zero dev fee**, **zero dependensi eksternal** selain RandomY library.

- **Zero dev fee** — semua hash 100% ke wallet yang dikonfigurasi, tidak ada switch wallet
- **Protocol robust** — single event loop, routing response by id, support `eth_getWork` polling + `mining.notify` push (ETHPROXY / stratum style), reconnect backoff otomatis
- **Light mode** (256MB cache) atau **full mode** (~2.6GB dataset), JIT compiler, **huge pages auto-detect** (fallback otomatis)
- **CPU affinity + nice priority** — thread di-pin per core
- **Multi-pool config file** (`pool.cfg`) — failover antar server
- Flags arsitektur diambil dari `randomx_get_flags()` (ARGON2_AVX2/SSSE3) — di atas default upstream

---

## Docker Image

Image resmi di **Docker Hub**: [`ylxai/xcb`](https://hub.docker.com/r/ylxai/xcb)

```bash
docker pull ylxai/xcb:v1

# Jalankan cepat (env dari image default: wallet + pool sg)
docker run --rm ylxai/xcb:v1

# Jalankan dengan override penuh
docker run --rm -d --name xcb \
  -e WALLET=<alamat_wallet> \
  -e POOL=sg.catchthatrabbit.com:8008 \
  -e WORKER=myworker \
  -e THREADS=4 \
  -e FULL_MEM=0 \
  -e LARGE_PAGES=0 \
  ylxai/xcb:v1
```

> **Tips:** `LARGE_PAGES` kini **auto-detect** — jika huge pages tidak tersedia (mis. container tanpa mlock), miner otomatis fallback ke normal pages tanpa crash. Set `LARGE_PAGES=0` hanya untuk memaksa nonaktif.
> FULL_MEM kini **auto-detect dari RAM**: `>= 3.5 GiB` → full dataset, di bawahnya light (256MB).
> Set `FULL_MEM=0` (light) atau `FULL_MEM=1` (full) untuk override, atau flag `--light`/`--full`.
> Full mode butuh ~2.6GB RAM + inisialisasi dataset ~56s.

---

## Environment Variables

Semua env vars dibaca langsung (precedence ≥ config file & CLI):

| Variable | Default | Deskripsi |
|----------|---------|-----------|
| `WALLET` | *(dari image)* | Alamat wallet Core Coin (XCB) |
| `POOL` | `sg.catchthatrabbit.com:8008` | Host pool `host:port` |
| `WORKER` | `pool` | Nama worker (ditampilkan pool sebagai `wallet.worker`) |
| `THREADS` | *(kosong)* | Jumlah thread. **Kosong = auto (jumlah CPU cores)** |
| `FULL_MEM` | *(auto)* | `1`/`true` = full 2.6GB, `0`/`false` = light 256MB, kosong/unset = auto dari RAM (>=3.5GiB) |
| `LARGE_PAGES` | *(auto)* | `1` = paksa huge pages, `0` = nonaktif, kosong = auto-detect (fallback normal pages) |
| `LOG_SHARES` | *(false)* | `1` = print setiap share found/accepted (default quiet — pool difficulty rendah sangat noisy) |
| `SUBMIT_INTERVAL_MS` | `0` | Min. jarak antar submit dalam ms (anti-ban pool ketat; `0` = unlimited). CLI: `--submit-interval-ms N` |

---

## Build dari Source (Lokal)

### Persyaratan
- Linux x86_64 dengan **AES-NI + AVX2** (build memakai `-march=x86-64-v3`) atau aarch64 dengan crypto extensions
- CMake ≥ 3.10
- Compiler C++17 (g++ ≥ 8 atau clang ≥ 7)
- OpenSSL dev headers

### Build
```bash
git clone <url-repo> xcb
cd xcb
git submodule update --init --recursive   # Wajib: tarik RandomY

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# Binary: build/miner-saya (~215KB stripped)
```

---

## Cara Pakai (Local Binary)

```bash
./miner-saya                        # auto threads, lihat pool.cfg / env
./miner-saya -t 4                   # 4 thread
./miner-saya --light -t 1           # 1 thread light mode
./miner-saya --full -t 2            # force full dataset (default: auto by RAM)
./miner-saya -o pool.lain.com:8008 -u wallet.worker   # override pool+wallet
```

### Konfigurasi `pool.cfg`
File `pool.cfg` (cwd, `/miner/pool.cfg`, atau `~/xcb/pool.cfg` — dicek otomatis) mendukung multi-server failover:

```ini
wallet=alamat_wallet_anda
worker=pool
server[1]=sg.catchthatrabbit.com
port[1]=8008
server[2]=hk.catchthatrabbit.com
port[2]=8008
server[3]=de.catchthatrabbit.com
port[3]=8008
threads=4
light=true
```

> Precedence: **env vars > pool.cfg > CLI flags**. `WALLET`+`POOL` env aktif = pool.cfg diabaikan.
> Commands flag: `-o host:port`, `-u wallet[.worker]`, `-p password`, `-t N`, `--light`, `--full`, `--no-jit`, `-h`.

---

## Protocol Stratum

Client protocol ditulis ulang (Fase 1) menjadi **single event loop** yang robust:

- **Routing by id** — setiap respons dipetakan ke permintaan asalnya (`eth_getWork`, login, submit) atau ke notifikasi (`mining.notify`, `mining.set_difficulty`)
- **Tidak pernah blocking** pada satu respons — `eth_getWork` polling tiap 3 detik (fire-and-forget), tidak ada window yang menelan respons share/notifikasi
- **`mining.notify` push** didukung penuh (format 3 elemen `[header, seed, target]` dan 4 elemen `[jobid, header, seed, target]`, hex auto-detect `0x`)
- **Tidak disconnect pada respons tak dikenal** — error/edge case hanya dicatat
- **Reconnect backoff** eksponensial (2s → 30s) + clean shutdown
- **`m_msgId` atomic** — aman dipakai thread network + worker bersamaan
- Submit share memakai header job tempat share ditemukan (bukan header global terakhir)

---

## Docker (Build Image Sendiri)

```bash
cd xcb
git submodule update --init --recursive
docker build -t ylxai/xcb:v1 .
docker run --rm ylxai/xcb:v1
```

Image docker **multi-stage** (builder → runtime ~32MB), jalan sebagai user non-root `miner`, entrypoint `./miner-saya`.
`pool.cfg` ikut di-copy ke `/miner/pool.cfg` sebagai fallback. Docker build tidak lagi bergantung folder `.git` submodule di build context (fallback: clone + checkout pinned commit).

---

## Kubernetes / Akash

### Akash SDL
```yaml
version: "2.0"
services:
  service-1:
    image: ylxai/xcb:v1
    env:
      - WALLET=<alamat_wallet>
      - POOL=sg.catchthatrabbit.com:8008
      - WORKER=akash
      - THREADS=16        # harus ≤ cpu.units
      - FULL_MEM=0
      - LARGE_PAGES=0
    expose:
      - port: 80
        as: 80
        to:
          - global: true
profiles:
  compute:
    service-1:
      resources:
        cpu:
          units: 16
        memory:
          size: 8Gi
        storage:
          - size: 5Gi
  placement:
    dcloud:
      pricing:
        service-1:
          denom: uact
          amount: 100000
deployment:
  service-1:
    dcloud:
      profile: service-1
      count: 1
```

> `expose` port 80 sebenarnya tidak dipakai (tidak ada web server) — boleh dihapus.

### Kubernetes (Deployment + Secret)
Contoh lengkap ada di [`k8s/deployment.yaml`](k8s/deployment.yaml):
```bash
kubectl create namespace mining
kubectl apply -f k8s/deployment.yaml
kubectl logs -n mining deploy/xcb-miner -f
```
Wallet disimpan sebagai **Secret** (tidak di-env image). **`THREADS` wajib = `limits.cpu`**
(default di kode = `nproc` NODE, bukan limit pod — salah set akan oversubscribe).

---

## Benchmark

### VPS 16 core / 8GB (full dataset)
| Thread | Mode | Hashrate | RAM |
|--------|------|----------|-----|
| 1 | Light | ~68 H/s | ~256MB |
| 2 | Light | ~122 H/s | ~256MB |
| 4 | Light | **~291 H/s** | ~256MB |
| 16 | Light | ~1.26 KH/s | ~256MB |
| 16 | **Full** | **~7.9 KH/s** | ~2.6GB |

> Full mode ±6x hashrate light mode (diverifikasi di VPS 16 core, 8GB RAM).

### Sandbox 2-core / 4GB (aes+avx2, verifikasi live pool)
| Thread | Mode | Hashrate | RAM | Catatan |
|--------|------|----------|-----|---------|
| 1 | Light | ~95 H/s | ~256MB | dataset init ~0.5s |
| 2 | **Full** | **~1223 H/s** | ~2.6GB | dataset init ~56s; ~610 H/s/thread — **±8x light** |

> Verifikasi: 815 shares accepted / 0 rejected (full, 2 thread, pool live sg.catchthatrabbit.com).

---

## Optimasi untuk Mesin Sendiri

```bash
# Huge pages (2MB)
sudo sysctl vm.nr_hugepages=1280

# CPU performance governor
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Realtime priority
sudo chrt -rr 1 ./miner-saya
```

---

## Logging

- **Blok `=== HASHRATE ===`** setiap 5 detik (total + per-worker)
- **Shares accepted** — ringkasan tiap 50 share (atau per-share jika `LOG_SHARES=1`)
- **Rejected** — selalu ditampilkan
- **Job baru dari pool** — auto-detect header/target

---

## Roadmap

- [x] **Fase 1 — Protocol rewrite**: event loop, routing by id, `mining.notify`, atomic msg id, reconnect backoff, no blocking getWork
- [x] **LARGE_PAGES auto-detect** + honor `useJIT`/`hardAES` config
- [x] **Fix env parsing** — env vars tidak ditimpa config file; aman terhadap string kosong
- [x] **Fase 2 — Correctness**: submit dengan header job milik share ✅, target compare dual-mode (full 256-bit & ethproxy 64-bit MSB) ✅, `--selftest` (20 checks) ✅
  - Worker melakukan snapshot job (header+target) per batch; share di-submit dengan header job yang benar-benar di-hash (tanpa data race)
  - Target compare: stratum 64-hex = big-endian 32-byte; ethproxy 60-hex = 64-bit MSB (persis validasi pool)
  - `--selftest`: verifikasi parse target (valid/invalid/ganjil), compare less/equal/greater, blob header+nonce LE, nonce BE — exit code 0/1
- [x] **Fase 3 — Performance**: auto FULL_MEM dari RAM ✅, AVX-512 evaluation (RandomY tidak punya jalur AVX-512) ✅, benchmark vs upstream ✅
  - Benchmark full 2 thread (sandbox 2 vCPU): **miner-saya 1530.4 H/s vs coreminer official 1483.7 H/s (+3.1%)**
  - coreminer official gagal negosiasi dengan pool catchthatrabbit (243x Invalid response, 0 share); miner-saya stabil 1696 accepted / 0 rejected
- [x] **Fase 4 — Ops**: failover pool list ✅, `eth_submitHashrate` ✅, Docker polish ✅
  - Failover: pool.cfg multi-pool (`server[N]`/`port[N]`) — 3x login gagal → pindah pool otomatis; teruji: pool mati → failover ke pool 2 → mining normal
  - `eth_submitHashrate` dikirim tiap 60s (hashrate + worker id) — pool ethproxy dapat melihat hashrate
  - Dockerfile: `FULL_MEM` kini auto (bukan force 0), komentar multi-pool
- [x] **Fase 5 — Submit engine & perf**: job versioning + stale guard ✅, rate-limit submit ✅, nonce allocation review ✅
  - Job versioning: `m_jobSeq` naik per job; share untuk job > 3 versi di-drop (counter `stale`), worker berhenti hashing header yang sudah kedaluwarsa — tidak membuang hash & tidak flood pool dengan share basi
  - Rate-limit submit (anti-ban): `--submit-interval-ms N` / config `submit-interval-ms` (default `0` = unlimited; pool menerima semua, teruji 1105 share/60s tanpa reject)
  - Nonce allocation: sudah non-overlap via `fetch_add` atomik; di-review tanpa perubahan (batch 32, worker re-snapshot per batch)
  - Warmup: JIT compiled hash pertama = 2-3 ms (diukur) — tidak ada ruang optimasi

---

## Troubleshooting

| Gejala | Penyebab | Solusi |
|--------|----------|--------|
| `cache alloc failed` | Huge pages tidak tersedia di container | Tidak perlu — v1+ auto-fallback ke normal pages; atau set `LARGE_PAGES=0` |
| `Invalid getWork response` + disconnect loop | Client lama memblokir getWork dan menelan respons lain | Sudah diperbaiki di Fase 1 (event loop) — upgrade binary |
| Crash `stoul` / `Config` gagal | Env var kosong (`-e THREADS=`) | Biarkan env kosong atau isi nilai valid — sudah ditangani |
| Cuma 1 thread padahal banyak core | Default kode lama `threads=1` | Set `THREADS=<n>` atau gunakan versi terbaru (auto = semua cores) |
| Hashrate ~50% dari harapan | Mode light (FULL_MEM=0) | `FULL_MEM=1` + RAM cukup |
| Worker stuck (0.0x H/s) | Core sibuk/di luar cpuset provider (CPU affinity) | Cek `lscpu`/`Cpus_allowed_list`; kurangi THREADS |
| Build gagal `RandomY` tidak ditemukan | Submodule belum di-init | `git submodule update --init --recursive` |
| Log penuh ribuan share/detik | Pool difficulty rendah + log per-share | Default quiet; verbose hanya untuk debug (`LOG_SHARES=1`) |

---

## Struktur Proyek

```
xcb/
├── CMakeLists.txt           # Build system (-march=x86-64-v3 / armv8-a+crypto, -O3, LTO, stripped)
├── Dockerfile               # Multi-stage: builder → runtime ~32MB, user non-root
├── .gitlab-ci.yml           # CI Puzl RunMyJob (test 2m saat push, mine 55m/jam saat schedule)
├── k8s/
│   └── deployment.yaml      # Deployment + Secret contoh
├── external/
│   └── RandomY/             # RandomY library (submodule, BSD 3-Clause)
├── pool.cfg                 # Multi-server config (wallet + 3 pool failover)
└── src/
    ├── main.cpp             # Entrypoint + signal handler
    ├── Config.hpp/.cpp      # Config parser (env vars + file + CLI)
    ├── StratumClient.hpp/.cpp # Protocol client — single event loop, routing by id
    ├── Miner.hpp/.cpp       # Thread pool, VM management, mining loop, stats
    └── picosha3.h           # SHA3-512 (header-only, stack-based)
    ├── encoding.hpp         # hex <-> byte + target parsing (shared Miner/selftest)
    ├── selftest.cpp/.h      # --selftest: 20 checks target compare & encoding
```

---

## Lisensi

Kode sendiri — bebas digunakan untuk keperluan pribadi.
RandomY library © RandomX contributors — BSD 3-Clause.
