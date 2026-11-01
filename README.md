# pmbook: low-latency Polymarket feed & order-book engine (C++20)

Work in progress. See `learn/00-START-HERE.md` for the design and lessons.

## Build

Requirements: CMake ≥ 3.24, Ninja, OpenSSL, zlib. On macOS:
`brew install cmake ninja openssl@3 llvm clang-format` (Homebrew LLVM is used
automatically for working sanitizers).

```bash
make test     # debug build (ASan + UBSan) + tests
make release  # -O3 + LTO
make check    # debug, tsan and release builds, all tests, format check
```

Find token IDs for markets: `python3 tools/list_markets.py --limit 5`
