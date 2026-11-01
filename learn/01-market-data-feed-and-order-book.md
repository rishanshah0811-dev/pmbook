# 01 · What is a market data feed and an order book?

**In one sentence:** An order book is the list of everyone currently waiting to buy or sell, and a market data feed is the live stream of messages that tells you how that list is changing.

**Why trading firms care:** You can't trade well on a picture of the market that's out of date. Every firm's first piece of software is the thing that keeps an exact, current copy of the book. If that copy is wrong, every decision built on top of it is wrong too.

## The everyday analogy

Picture a noticeboard at a farmers' market for one item, say a concert ticket.

- On the left, people pin notes: "I'll **buy** one for $40", "I'll buy for $38".
- On the right: "I'll **sell** one for $45", "I'll sell for $47".

That board is the **order book**. The left side holds the **bids** (buyers). The right side holds the **asks** (sellers, also called offers).

- **Best bid** = the highest buyer ($40). **Best ask** = the cheapest seller ($45).
- **Spread** = best ask − best bid = $5. Nobody trades until someone closes that gap.
- A **price level** is one price and the total quantity waiting at it, e.g. "3 tickets wanted at $40".

You can't stand at the board all day. So a friend texts you each time a note goes up or comes down. Those texts are the **market data feed**. Usually it's one big photo of the whole board first (a **snapshot**), then small updates after it (**deltas**): "someone added 2 at $41", "the $47 seller left".

## Polymarket specifically

Polymarket sells shares in yes/no questions ("Will X happen by date Y?"). A YES share pays $1.00 if it happens and $0 if not. So every price sits between **0.00 and 1.00** and works like a probability: 0.62 means the market thinks there's roughly a 62% chance.

Each outcome (YES, NO) has its own order book. Each book is identified by a **token ID** (Polymarket's docs also call it `asset_id`). It's a 77-digit number. That's too big for a normal 64-bit integer, so we'll keep it as text.

## Tiny worked example

The feed sends a snapshot (a `book` message) for one token:

```
bids: 0.61 × 500,  0.60 × 1200
asks: 0.63 × 300,  0.64 × 900
```
Best bid 0.61, best ask 0.63, spread 0.02 (2 cents, or 2 "ticks" if the tick size is 0.01).

Then a delta (a `price_change` message): `BUY 0.62 size 400`. Someone new wants to buy 400 at 0.62. The new best bid is **0.62**, and the spread shrinks to 0.01.

Then: `SELL 0.63 size 0`. Size 0 means "this level is now empty, remove it". The new best ask is **0.64**.

Our C++ engine does exactly this, thousands of times a second, and it has to get it right every single time.

## Interview questions you could be asked

1. **What's the difference between a snapshot and a delta feed?**
   A snapshot is the full book at one moment. Deltas are only the changes. Deltas are much smaller and faster, but if you miss one your book is silently wrong. So you need a way to spot gaps and re-fetch a snapshot.
2. **What is the spread, and why does a market maker care?**
   Best ask minus best bid. A market maker earns roughly the spread by buying at the bid and selling at the ask, so the spread is their potential profit per round trip, before fees and adverse selection.
3. **If you see best bid ≥ best ask in your local book, what happened?**
   That's a "crossed book". On a real exchange those orders would already have traded. So it almost always means your copy is out of date or a message was applied wrong. You should flag it and resync from a fresh snapshot.
