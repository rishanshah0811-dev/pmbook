# 03 · Parsing messages and why prices are integers

**In one sentence:** We turn each JSON text message from Polymarket into a small C++ struct, and we store every price and size as an exact whole number, never a floating-point number.

**Why trading firms care:** Parsing runs on every single message, so it's often the biggest chunk of a feed handler's time. And a price that's off by 0.0000000001 is a *different* price: it won't match the level you're trying to update, and your book quietly goes wrong.

## The analogy

Shops don't keep prices as "£0.1 of a pound". They count **pennies**: 10p. Pennies are whole numbers, so 10p + 20p is always exactly 30p. That's **fixed point**: pick a small unit and count whole units of it.

Computers store `double` (floating point) in binary. In binary, 0.1 is a repeating fraction, like 1/3 = 0.333… in decimal. So `0.1 + 0.2 == 0.3` is **false** in C++. (There's a test that proves it: `FixedPoint.WhyNotFloats`.)

Our units:
- **Price**: `int32`, 1 unit = 0.0001. `"0.08"` → **800**. Prices sit in 0–1, so the max is 10,000.
- **Size**: `int64`, 1 unit = 0.000001 shares (a "micro-unit"). `"33343.4"` → **33,343,400,000**.

Bonus: a price of 800 can be used directly as an **array index**. That's the trick behind the fast order book in the next milestone.

If a string has more decimals than we can hold (`"0.00005"`), we **reject** it instead of rounding. Silent rounding is how books drift.

## How the parser works

- **simdjson On-Demand**: it doesn't build a full tree of the JSON. It walks the text once, and we grab fields as we pass them. Under the hood it uses SIMD to scan 32–64 bytes per instruction.
- **Zero-copy strings**: `asset_id` and `hash` are `std::string_view`s, a pointer plus a length into the parser's buffer. No copying, but they're only valid until the next `parse()`.
- **No allocations once warm**: levels go into vectors inside `ParsedFrame` that are cleared, not freed, so their memory is reused for every frame.
- **`std::span`** is like `string_view` for any type: `frame.bids(msg)` hands back a view of that message's bid levels.
- **RAII**: the parser's padded buffer is a `unique_ptr<char[]>`, which frees itself when the parser is destroyed. You never call `delete`.

What we learned from real data (not in the docs):
1. The first snapshots arrive as a **JSON array** of books, not one object.
2. Real `book` messages carry `tick_size` and `last_trade_price` too.
3. Levels aren't sorted in any guaranteed order (the docs example has bids going *up*).

## Tiny worked example (real numbers from our benchmark)

Release build on an Apple M5 with a warm cache, run on our 3 captured `price_change` frames and 1 frame holding 4 book snapshots:

| | simdjson + fixed point | nlohmann (just building the JSON tree) |
|---|---|---|
| 3 price_change frames | **788 ns** (≈263 ns each) | 10,057 ns |
| 4-book snapshot frame | **7.2 µs** | 60.3 µs |

That's roughly 8–13× faster, *even though* the nlohmann side does less work: it doesn't convert anything to fixed point. Treat these as early micro-benchmark numbers. The honest end-to-end comparison comes in Milestone 8.

## Interview questions you could be asked

1. **Why not use `double` for prices?**
   Binary floats can't represent most decimals exactly, so equality checks and map lookups on prices break, and repeated adds drift. Integers in a fixed unit (ticks, cents, 1e-4) are exact, fast, and can index arrays.
2. **What's the danger of a `string_view` into a parser buffer?**
   It doesn't own the memory. If the buffer gets reused (on the next parse) or freed, the view dangles and reading it is undefined behaviour. You either copy what you need or follow a strict lifetime rule. AddressSanitizer catches many violations.
3. **How do you test a parser properly?**
   Real examples (docs plus captured live data), edge cases (empty book, size 0, nulls), malformed input that must be rejected, every truncated prefix of a valid message (it must never crash), and a cross-check against an independent parser on the same data.
