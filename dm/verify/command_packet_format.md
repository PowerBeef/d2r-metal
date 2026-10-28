# Client Command Packet Sequence Format

中文：[command_packet_format.zh-CN.md](command_packet_format.zh-CN.md)

Internally the game is split into two halves, client and server: the client turns the player's actions into "command packets" and hands them to the server to process. Recording the command packets of a run in order into a file lets a later **host** run (`dmhost` from `dm/host`) replay them as-is, so that the server plays out the same run again: `replay.py` and `golden.py replay` record a route once and replay it on later builds. `dm/tools/c2s.py` labels a recording by packet name (`dm/verify/c2s_opcodes.tsv`).

## Capture point

- The client command dispatch function `main + 0x6e5b40(G, unit, packet, length)`. At entry:
  `x0` = G (the game object, the same base address as in [tick_state_format.md](tick_state_format.md)), `x1` = the unit sending the packet (may be 0), `x2` → the packet bytes (packet number = first byte, must be < 0x67), `w3` = length.
- The handler table `main + 0x20a4190` (0x67 entries; an empty entry means an illegal packet number).
- **System-class messages** take another path: `main + 0x6310b0(buf, packet length)` (jump table `0x1daf73c`), buf = u32 client number + packet. The packet receive pump `0x5d3960` first drains the system-class queue each tick, then processes command packets.

  | Packet no. | Length | Meaning | Replay handling |
  |---|---|---|---|
  | 0x67 | | Join | Let through as usual at game entry (the host's own is used) |
  | 0x6c | | Character data chunk | Injected from the file |
  | 0x6d | 13 | Heartbeat: compares +0x6c4 and +0x6c0 of the client structure, and if they differ replies with a sync message and updates | Injected from the file |
  | 0x6f | 5 | Progress: the progress value reported by the client is written to +0x6c0 of the client structure (e.g. "new area finished loading" after a teleport) | Injected from the file |
  | 0x6a / 0x6b / 0x6e | | Discarded by default | — |

Every command packet's meaning and exact length, in our own words, is in [c2s_opcodes.tsv](c2s_opcodes.tsv) (read by `dm/tools/c2s.py`): the dispatcher checks no length; each handler checks its own and refuses (or stops the game on) a mismatch. Movement (0x01–0x04) carries a path in this build: 210 bytes, 214 with a target unit.

## One line of JSON per packet (in call order)

```
{"tick":57,"unit":1,"len":5,"pkt":"0300120034"}
```

| Field | Meaning |
|---|---|
| tick | `u32 [G+0x170]` at call time (the server's current tick number) |
| unit | `u32 [x1+0x8]` (unit id; null when x1 = 0) |
| len | `w3` |
| pkt | Lowercase hex of the first min(len, 512) bytes of the packet |
| q | Present only for system-class messages, value `"sys"`; in that case unit is always null, tick is the current tick number (`[G+0x170]` at the most recent tick entry), len = packet length, and pkt **does not include** the leading 4-byte client number |

Example of a system-class entry: `{"tick":201,"q":"sys","unit":null,"len":5,"pkt":"6f…"}`.

The host records with `DM_C2S_LOG=<path>`.

## Two layers of criteria

1. **Server layer**: the host takes no controller input; at the tick whose number = tick, in file order, it hands each packet as-is to `0x6e5b40` (`DM_C2S_REPLAY=<file>`, which also turns off commands the client would issue on its own; for testing only), and the 6-slot state is required to match the recorded run tick by tick.
   - **System-class messages must be recorded and injected tick by tick.** The tick number and value with which `0x6f` arrives determine at which tick the server inserts a sync message (in several hundred places the server compares +0x6c4 / +0x6c0 before sending a message and resends as needed), which depends on how fast the client is and differs between runs, so it is timing-dependent input.
   - During replay, the host's own system-class messages are all discarded from the file's first tick on (the `0x67` join before game entry is let through as usual), and instead the recorded original bytes are injected at their original tick numbers; within one tick, system-class messages are injected first and command packets dispatched afterwards, matching the order of the packet receive pump.
   - After the file's last tick, the messages the host client issues on its own are let through again.
2. **Client layer**: under the same key script, the command packet sequences two runs produce are compared **by content and order only** (tick is ignored), and in addition the UI message streams must match ([ui_message_stream.md](ui_message_stream.md)).
