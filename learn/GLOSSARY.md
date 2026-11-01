# Glossary

One line per term, alphabetical.

- **AddressSanitizer (ASan)**: compiler add-on that crashes loudly the moment your program reads or writes memory it shouldn't.
- **Ask / offer**: a resting order to sell at a given price.
- **asset_id / token ID**: Polymarket's ID for one outcome's order book (a 77-digit number, stored as a string).
- **Best bid / best ask**: highest buy price / lowest sell price currently in the book.
- **Bid**: a resting order to buy at a given price.
- **CMake**: a tool that reads a description of your project and generates the real build instructions for your machine.
- **Compiler**: program that turns C++ source code into machine code the CPU runs.
- **Crossed book**: best bid ≥ best ask. On a real exchange this can't last, so locally it means a bug or missed update.
- **Data race**: two threads access the same memory without ordering, and at least one writes. Undefined behaviour in C++.
- **Delta**: a message describing only what changed in the book.
- **Dependency**: someone else's library our code uses (e.g. simdjson).
- **FetchContent**: CMake feature that downloads a dependency's source at configure time and builds it with our project.
- **LTO (link-time optimisation)**: optimising across all source files at the final link step.
- **Market data feed**: live stream of messages describing changes to the market.
- **OpenSSL**: library that implements TLS encryption.
- **Order book**: all resting buy and sell orders for one instrument, grouped by price.
- **Preset**: a named, saved set of CMake build settings (e.g. `debug`, `release`).
- **Price level**: one price plus the total size resting at it.
- **Sanitizer**: compiler option that adds runtime checks to catch a class of bugs (memory, undefined behaviour, data races).
- **SIMD**: "single instruction, multiple data": one CPU instruction processes many bytes at once.
- **Snapshot**: a message containing the full book at one moment.
- **Spread**: best ask − best bid.
- **ThreadSanitizer (TSan)**: compiler add-on that detects data races at runtime.
- **Tick / tick size**: the smallest allowed price step (Polymarket uses 0.01, 0.001 or 0.0001 depending on the market).
- **TLS**: the encryption layer behind `https://` and `wss://`.
- **Undefined behaviour (UB)**: code the C++ standard gives no meaning to (e.g. signed overflow). It may "work" today and break under optimisation.
- **UndefinedBehaviorSanitizer (UBSan)**: compiler add-on that reports undefined behaviour while the program runs.
- **WebSocket**: a long-lived two-way connection over the internet. The server can push messages to you without you asking each time.
