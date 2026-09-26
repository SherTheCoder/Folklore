# Streaming protocol

A persistent TCP connection carrying length-prefixed binary messages. The
device is always the client; the server never sends audio.

Defined in [`stream_proto.h`](../Appli/Core/Inc/stream/stream_proto.h),
implemented by [`stream_session.c`](../Appli/Core/Src/stream/stream_session.c),
and read by [`server/asr_gateway.py`](../server/asr_gateway.py).

## Message header

Four bytes on every message:

```
 0      1      2      3
+------+------+------+------+
| type |flags |  payload len|      length is big-endian
+------+------+------+------+
```

| type | name | direction |
|---|---|---|
| 1 | `START` | device → server |
| 2 | `AUDIO` | device → server |
| 3 | `END` | device → server |
| 4 | `RESULT` | server → device (optional) |
| 5 | `ERROR` | server → device (optional) |

Flags are only defined on `AUDIO`:

| bit | name | meaning |
|---|---|---|
| 0 | `PREROLL` | audio from before the trigger point |
| 1 | `FIRST` | first chunk of this utterance |

`PREROLL` exists so the receiver can tell the recogniser where the command
actually starts without having to guess from the audio. A server that ignores it
gets a slightly longer utterance, not a wrong one.

## START, 32 bytes

```c
uint8_t  version;          /* 1                                    */
uint8_t  channels;         /* 1                                    */
uint8_t  sample_bits;      /* 16                                   */
uint8_t  little_endian;    /* 1                                    */
uint32_t sample_rate;      /* 16000                                */
uint32_t utterance;        /* increments per activation            */
uint32_t preroll_samples;  /* how much precedes the trigger        */
char     keyword[16];      /* what was detected, NUL padded        */
```

Everything after the header is little-endian, including the audio, which is what
`little_endian` declares. The header's length field is the one big-endian value
in the protocol, because that is the convention for a framing length and getting
it wrong is the first thing a receiver checks.

## AUDIO

Payload is 320 signed 16-bit little-endian samples: 20 ms, 640 bytes. Chunks
are consecutive and never reordered — TCP guarantees that for as long as the
connection is healthy, and the sender never has two outstanding.

## END, 12 bytes

```c
uint32_t utterance;
uint32_t samples;          /* total sent, for the receiver to check */
uint32_t reason;           /* 0 silence, 1 timeout, 2 aborted       */
```

`END` closes the utterance. It does **not** close the connection — the next
keyword reuses it, which is what keeps the handshake off the latency path.

## Overhead

| | bytes |
|---|---|
| audio payload per chunk | 640 |
| header per chunk | 4 |
| framing overhead | **0.62%** |
| `START` + `END` per utterance | 52 |

Measured against the reference receiver over a two-second utterance the total
comes to 0.78% including `START` and `END`. That figure excludes TCP/IP and
Ethernet headers and any retransmission; `net_tcp_retransmits()` reports the
latter so the two can be quoted separately.

For comparison, a masked client WebSocket frame around the same 640-byte
payload is eight bytes, or 1.25%. The difference is 200 bytes a second, which
is not the reason to define a custom format — the reason is that the parser on
the device is a header struct and a length check instead of a library.

## What a receiver has to handle

TCP is a byte stream, so:

* a message can arrive split across several reads;
* several messages can arrive in one read;
* a header can be split down the middle.

`asr_gateway.py` buffers and re-parses from the front on every read, which is
the whole answer. It also feeds chunks onward as they arrive rather than waiting
for `END` — waiting would undo the point of streaming.

## Testing without a board

```sh
./server/asr_gateway.py --port 5555 --out captures/
./server/replay_client.py command.wav --realtime
```

`replay_client.py` sends a WAV using the same framing at the same 20 ms
cadence, so the server, the reassembly and the timing print can all be
exercised before the hardware is on the bench.

## Security

The connection is plaintext TCP. The architecture note this project works from
specifies TLS — NetX Secure on this part — and that is the right thing for a
deployment carrying speech. It is not in this tree: adding a TLS stack is a
material change to the RAM budget (handshake peak, not steady state) and it
would have to be measured, not assumed. The seam is `net_tcp_send()` and
`net_tcp_input()`; nothing above them knows how the bytes travel.
