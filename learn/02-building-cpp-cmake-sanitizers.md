# 02 · How a C++ project is built: CMake, dependencies, sanitizers

**In one sentence:** CMake turns a short description of our project into real build commands, downloads the libraries we depend on, and lets us build the same code in "safe and slow" or "fast" modes.

**Why trading firms care:** Trading code has to be fast *and* correct. A memory bug that corrupts one price can lose real money. Firms build every change twice: once with bug-catching checks switched on, and once fully optimised for speed. We're doing the same thing.

## The everyday analogy

A recipe says "make a cake". CMake is the head chef who reads it and writes the step-by-step orders for *this particular kitchen* (your Mac, or a Linux server). **FetchContent** is the chef sending someone to the shop for ingredients (libraries) before cooking. A **preset** is a saved recipe variant: "the gluten-free version" or "the competition version".

## What we set up

| Preset | What it does | Why |
|---|---|---|
| `debug` | No optimisation, **ASan + UBSan** on | Catches memory and undefined-behaviour bugs the moment they happen |
| `tsan` | **ThreadSanitizer** on | Catches data races (two threads touching the same memory unsafely). We'll need this for the lock-free queue |
| `release` | `-O3` + **LTO** | As fast as possible. LTO = "link-time optimisation": the compiler optimises across files, not just inside one |

A **sanitizer** adds hidden checks around every memory access. It makes the program about 2–5× slower, but bugs become loud crashes with a line number instead of silent wrong answers.

Libraries we pull in, and why:
- **simdjson**: parses JSON using SIMD instructions (one CPU instruction handles 16+ bytes at once). This is our hot path.
- **IXWebSocket**: the WebSocket + TLS client. I picked it over Boost.Beast because it's small, builds through FetchContent with no Boost install, and has a simple callback API, so the networking code stays short and readable. TLS (the encryption in `wss://`) comes from **OpenSSL**, via Homebrew on Mac and the system package on Linux, so both platforms run the same code.
- **nlohmann/json**: easy to use but slow. It's only for the "naive" version we'll benchmark against.
- **GoogleTest** for tests, **Google Benchmark** for micro-benchmarks.

Warnings: our own code compiles with `-Wall -Wextra -Wpedantic -Wconversion` and more, plus `-Werror`, so any warning fails the build. Third-party code is excluded, because we can't fix their warnings.

## A real problem we hit (good interview story)

Apple's compiler (Xcode 16.0) has sanitizer runtimes that crash on start-up under macOS 26, even on an empty `int main(){}`. The fix: `cmake/MacLLVM.cmake` automatically uses Homebrew's LLVM (clang 23) on macOS, which ships matching runtimes. Lesson: **always check that your tools work on a trivial case before you trust them on real code.**

## Tiny worked example

```bash
cmake --preset debug          # configure: download deps, generate Ninja files
cmake --build --preset debug  # compile
ctest --preset debug          # run tests (under ASan + UBSan)
make check                    # all three presets + format check, one command
```

If you write `int a[3]; a[3] = 1;` in a test, the debug build stops with `AddressSanitizer: stack-buffer-overflow` and the exact line number. A release build might just carry on with corrupted memory.

## Interview questions you could be asked

1. **What does AddressSanitizer catch, and what does it cost?**
   Out-of-bounds reads and writes, use-after-free, double free, and leaks. It roughly doubles memory use and slows the program 2–3×, so you run it in tests, not in production.
2. **Why compile with `-O3` for benchmarks but test with `-O0` plus sanitizers?**
   `-O0` keeps code close to the source, so sanitizer reports are precise and debuggable. `-O3` is what you'd ship, so it's the only honest thing to time. Undefined behaviour can also change *only* under optimisation, which is why UBSan matters.
3. **What is a data race?**
   Two threads touch the same memory at the same time, at least one of them writes, and nothing orders the accesses (no lock, no atomic). In C++ that's undefined behaviour. ThreadSanitizer detects it at runtime.
