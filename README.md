# miner-saya — Core Coin (XCB) RandomY Miner

Miner C++ pribadi untuk **Core Coin (XCB)** menggunakan algoritma **RandomY** (fork RandomX v1.2.1).  
Dibangun dari nol — **zero dev fee**, **zero dependensi eksternal** selain RandomY library.

---

## 📋 Hasil Buatan Sendiri

Seluruh kode ditulis manual, bukan fork/clone dari miner lain:

| Komponen | Asal | Detail |
|----------|------|--------|
| **StratumClient** | Tulis manual | ETHPROXY protocol (`eth_submitLogin` + `eth_getWork` + `eth_submitWork`) |
| **SHA3-512** | Tulis manual | Pure stack, zero heap alloc, keccak-f[1600] dari spec FIPS 202 |
| **Manage VM thread** | Tulis manual | randomx_create_vm per thread, cache sharing read-only |
| **Nonce distribution** | Tulis manual | Atomic global counter — tidak ada overlap, coverage 100% |
| **Config parser** | Tulis manual | Parse `~/xcb/pool.cfg` + CLI flags |
| **RandomY library** | Submodule | RandomY v1.1.17 (RandomX fork untuk Core Coin) — BSD license |

**Zero dev fee.** Tidak ada switch wallet, tidak ada mining untuk alamat lain, tidak ada hidden thread.  
Semua hash 100% ke wallet yang dikonfigurasi.

---

## 🚀 Cara Pakai

### Persyaratan

- Linux x86_64 dengan AES-NI
- CMake ≥ 3.10
- Compiler C++17 (g++ ≥ 8 atau clang ≥ 7)

### Build

```bash
git clone https://github.com/your/repo miner-saya
cd miner-saya
git submodule update --init --recursive
mkdir build && cd build
cmake .. -DARCH=native
make -j$(nproc)
```

### Konfigurasi Wallet & Pool

Buat file `~/xcb/pool.cfg` (otomatis terdeteksi):

```ini
wallet=alamat_wallet_anda
worker=pool
server[1]=sg.catchthatrabbit.com
port[1]=8008
server[2]=hk.catchthatrabbit.com
port[2]=8008
server[3]=de.catchthatrabbit.com
port[3]=8008
```

Atau via CLI (override file):

```bash
./miner-saya -o sg.catchthatrabbit.com:8008 -u alamat_wallet -t 4
```

### Usage

```bash
./miner-saya                        # auto 4 thread, light mode
./miner-saya -t 4                   # 4 thread
./miner-saya --light -t 1           # 1 thread light mode
./miner-saya -t 4 --light           # 4 thread light mode (default)

# Semua pool dari ~/xcb/pool.cfg otomatis; bisa override:
./miner-saya -o pool.lain.com:8008 -u wallet.worker
```

### Command Flags

| Flag | Fungsi |
|------|--------|
| `-o host:port` | Pool address (override config file) |
| `-u wallet[.worker]` | Wallet address |
| `-p password` | Pool password (default: x) |
| `-t N` | Jumlah thread |
| `--light` | Mode light dataset (256MB cache, init ~0.3s) |
| `--no-jit` | Non-aktifkan JIT compiler |

---

## ⚡ Kelebihan

| Aspek | miner-saya | Miner lain (coreminer, dll) |
|-------|-----------|---------------------------|
| **Dev fee** | **0%** — semua hash ke wallet kamu | 1-2% dev fee untuk developer |
| **Kode** | 100% buatan sendiri, transparan | Fork/clone, ribuan line, potensi backdoor |
| **SHA3-512** | Pure C++ stack, optimasi manual | picosha3 atau OpenSSL |
| **Memory** | Light ~256MB cache saja | Beberapa butuh 2GB+ dataset |
| **Protocol** | ETHPROXY (auto-detect) | Multi-protocol tapi kompleks |
| **Nonce** | Atomic counter, coverage penuh | Range-based, ada celah overlap |
| **Dependency** | Hanya RandomY + Threads + OpenSSL | Boost, libmicrohttpd, jsoncpp, dll |
| **Binary size** | ~215KB stripped | Ratusan MB dengan dependensi |
| **Startup** | 0.3s (light), 6s (full) | 10-30s untuk init dataset |

### Tech

- **RandomY** — RandomX fork untuk Core Coin, 256MB dataset, JIT compiler
- **ETHPROXY** — protocol pool catchthatrabbit via `eth_submitLogin` / `eth_getWork` / `eth_submitWork`
- **Huge Pages** — otomatis terdeteksi (RANDOMX_FLAG_LARGE_PAGES), TLB miss berkurang
- **CPU Affinity** — thread di-pin ke core masing-masing
- **Nice Priority** — otomatis set nice -10 untuk prioritas lebih tinggi

---

## 📊 Benchmark

| Thread | Mode | Hashrate | RAM |
|--------|------|----------|-----|
| 1 | Light | ~68 H/s | ~256MB |
| 2 | Light | ~122 H/s | ~256MB |
| 4 | Light | **~291 H/s** | ~256MB |

---

## 🔧 Optimasi untuk Performa Lebih

Untuk hasil maksimal di mesin sendiri:

```bash
# Huge pages (2MB) — kalau belum di-set
sudo sysctl vm.nr_hugepages=1280

# CPU performance governor (jarang diperlukan di VM/container)
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Realtime priority
sudo chrt -rr 1 ./miner-saya
```

---

## 🛠 Struktur Proyek

```
miner-saya/
├── CMakeLists.txt           # Build system
├── external/
│   └── RandomY/             # RandomY library (submodule)
├── src/
│   ├── main.cpp             # Entrypoint + signal handler
│   ├── Config.hpp/.cpp      # Config parser (file + CLI)
│   ├── StratumClient.hpp/.cpp # ETHPROXY protocol client
│   └── Miner.hpp/.cpp       # Thread pool, VM management, mining loop
└── build/
    └── miner-saya           # Binary executable
```

---

## 📝 Lisensi

Kode sendiri — bebas digunakan untuk keperluan pribadi.  
RandomY library © RandomX contributors — BSD 3-Clause.
