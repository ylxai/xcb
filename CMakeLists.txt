cmake_minimum_required(VERSION 3.10)
project(miner-saya VERSION 1.0.0 LANGUAGES CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

# ============================================================
# RandomY library (dari submodule)
# ============================================================
add_subdirectory(external/RandomY EXCLUDE_FROM_ALL)

# Tambahkan include path RandomY
include_directories(external/RandomY/src)

# ============================================================
# Cari dependency
# ============================================================
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# ============================================================
# Source files miner-saya
# ============================================================
set(MINER_SOURCES
    src/main.cpp
    src/StratumClient.cpp
    src/Miner.cpp
    src/Config.cpp
)

set(MINER_HEADERS
    src/StratumClient.hpp
    src/Miner.hpp
    src/Config.hpp
)

# ============================================================
# Executable
# ============================================================
add_executable(miner-saya ${MINER_SOURCES} ${MINER_HEADERS})

target_link_libraries(miner-saya
    PRIVATE
        randomx               # RandomY library
        pthread
        OpenSSL::SSL
        OpenSSL::Crypto
)

# Optimasi portabel per arsitektur CPU
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(miner-saya PRIVATE -march=x86-64-v3 -O3 -flto)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_options(miner-saya PRIVATE -march=armv8-a+crypto -O3 -flto)
else()
    target_compile_options(miner-saya PRIVATE -O3 -flto)
endif()
target_link_options(miner-saya PRIVATE -flto -s)

install(TARGETS miner-saya DESTINATION bin)
