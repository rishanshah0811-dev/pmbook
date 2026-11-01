# 05 · The live feed: reconnects, heartbeats and recording

**In one sentence:** The feed client keeps a WebSocket connection alive no matter what happens to the network, and the recorder saves every message to disk so we can replay any session later.

**Why trading firms care:** Connections *will* drop: Wi-Fi blips, server restarts, cloud load balancers. A feed handler that crashes, or worse, keeps trading on a stale book after a reconnect, loses money. And recorded data is how firms test, debug, and research. "Can you replay yesterday exactly?" is a standard question.

## The analogy

You're on a phone call with a friend reading you live football scores.
- **Heartbeat**: every 10 seconds you say "still there?" (`PING`) and they say "yep" (`PONG`). If you hear *nothing at all* for 30 seconds, the line is dead even if your phone says "connected". Hang up and call back.
- **Exponential backoff**: if they don't pick up, wait 0.5 s, then 1 s, 2 s, 4 s… up to 30 s. Calling back every millisecond would just annoy them (and could get you rate-limited or banned).
- **Jitter**: add a random extra 0–20% to each wait, so a thousand clients that dropped at the same moment don't all call back at exactly the same instant.
- **Resubscribe + distrust**: after reconnecting you ask for the scores again. Anything you heard *before* the drop might be out of date, so you throw it away until you get the fresh full scoreboard (a new `book` snapshot).

## How it's built

- **Separation**: `FeedClient` only does networking and knows nothing about order books. It hands each frame plus a receive timestamp to a callback.
- **Our own retry policy**: IXWebSocket can reconnect by itself, but we turn that off. Our supervisor thread owns the whole lifecycle, so the policy is visible and testable.
- **The timestamp is taken first**, before any parsing: `steady_clock` (never jumps) in nanoseconds. Honest caveat: TLS decryption and WebSocket framing have already happened inside IXWebSocket by then, so our latency numbers start *after* that.
- **Recorder**: gzip JSON Lines, `{"ts_recv_ns":…,"raw":"…"}`, one file per UTC hour. It does a **sync flush** every second, so if the process crashes, everything up to the last second is still readable. (You saw this: reading the file while it was still being written worked, minus the gzip end marker.)

## Tiny worked example (from our tests)

Fake exchange on localhost, backoff 50 ms → 400 ms max, server switched off. One run measured retry gaps of **70, 111, 224, 421 ms**: the planned 50 → 100 → 200 → 400 ms, plus jitter and connect time. The test checks the third gap is more than 2.5× the first and that it never exceeds the cap.

Other offline tests: the server drops us → we resubscribe with the identical message and receive a fresh snapshot; the server goes silent (no PONG) → we detect it after the stale timeout and reconnect. All of them pass 10 times in a row under ThreadSanitizer.

## Interview questions you could be asked

1. **Why exponential backoff with jitter rather than retrying immediately?**
   Immediate retries hammer a server that's already struggling and can get you rate-limited or banned. Exponential backoff spreads retries out. Jitter stops many clients from retrying in sync (the "thundering herd").
2. **Your socket says "connected" but no data arrives. What do you do?**
   Use an application heartbeat plus a staleness timeout. A TCP connection can be half-dead (e.g. a NAT box forgot it) with no error for minutes. If nothing has arrived, not even a PONG, within the timeout, close it and reconnect.
3. **After a reconnect, what's wrong with continuing to apply deltas?**
   You may have missed deltas while disconnected, so your book is silently wrong. Mark every book stale, resubscribe, and only trust each book again once a fresh snapshot arrives.
