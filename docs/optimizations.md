# System-level optimisations

Five of them, each in its own file under
[`Appli/Core/Src/opt/`](../Appli/Core/Src/opt/), each with a switch in
`kikkar_config.h` so it can be turned off and the difference measured rather
than argued about.

| Switch | File | Targets |
|---|---|---|
| `OPT_GATED_INFERENCE` | `opt_gate.c` | idle CPU |
| `OPT_SHARED_ARENA` | `opt_arena.c` | peak RAM |
| `OPT_IDLE_SLEEP` | `opt_sleep.c` | idle CPU, power |
| `OPT_EARLY_LINK` | `opt_link.c` | handoff latency |
| `OPT_MVE_KERNELS` | `kws_kernels.c` | active CPU |

---

## 1. Gated inference — `opt_gate.c`

This is the audio event detector. A model step is 45,872 multiply-accumulates,
33 times a second. The level estimate in front of it is one subtraction and one
compare, and the level itself is already being computed for the animation, once
per 5.3 ms hop, by `flower_audio`.

So the gate is free, and what it gates is not. During silence the listening path
is capture, a 3:1 FIR, one log-mel frame per 10 ms and nothing else. The front
end keeps running on purpose: it is cheap, and its running mean has to keep
following the room even while nobody is talking.

A streaming model needs one more thing from a gate. Its layers remember the
last 1.5 s, and a gate that simply skipped steps would leave that memory full of
whatever it heard before the silence. So `kws_pipeline` keeps the last ten
steps, 300 ms of features in 1.2 KB, and when the gate opens it resets the model
to silence and replays them before carrying on live. The syllable that opened
the gate is heard in full, and the model starts from the same blank history
every training clip started from.

The hang time (`KWS_GATE_HANG_MS`, 300 ms) is what stops the gate clipping the
tail of a word: once speech is detected the model keeps running for a third of a
second after the level drops, so a quiet final syllable is still classified.

The gate never decides whether a keyword occurred. It decides whether that
decision is worth computing. `opt_gate_duty_pct()` reports the fraction of
steps that reached the model, which is the multiplier between the two CPU
figures.

## 2. The shared arena — `opt_arena.c`

Listening and streaming have disjoint lifetimes. Listening stops the moment a
keyword is confirmed; the sender stops the moment the utterance ends. One
19 KiB block serves both:

| Side | What is in the block | Bytes |
|---|---|---:|
| Listening | model: three activation slots and 2,384 B of streaming state | 5,264 |
| | front end: Hann window, mel filterbank, frame and spectrum buffers | 8,768 |
| | FFT workspace and twiddle tables | 4,112 |
| | gate look-back, the last 10 steps | 1,200 |
| | **listening total** | **19,344** |
| Streaming | audio queue, 29 chunks of 20 ms | **18,560** |

Only the running mean and the band edges, 320 bytes, stay outside, because
they describe the room and have to survive the utterance. Everything else on
the listening side is scratch or can be rebuilt:

```
kikkar_app.c, enter_streaming()          leave_streaming()

  kws_pipeline_set_inference(0)            stream_session_end()
  opt_arena_release(OPT_ARENA_KWS)         opt_arena_release(OPT_ARENA_STREAM)
  stream_session_begin()                   opt_arena_claim(OPT_ARENA_KWS)
    └ opt_arena_claim(OPT_ARENA_STREAM)    kws_pipeline_attach()
    └ stream_txq_bind()                    kws_pipeline_set_inference(1)
```

Peak becomes the larger of the two rather than their sum: 19 KiB instead of
19 KiB + 18 KiB, so the board does the same work in **18 KB less RAM**. The
start-up budget prints the saving, and `opt_arena_saved_bytes()` confirms it
from the real claims once an utterance has gone through. When the block comes
back, `kws_pipeline_attach()` rebuilds the window, the filterbank and the
twiddles, which takes a fraction of a millisecond, and fills every ring in the
model with silence, which is how every training clip began.

The owner tag is not decoration. A claim against a live owner fails and
increments `opt_arena_conflicts()`, so a lifetime mistake shows up as a number
in the metrics line instead of as corrupted audio. That counter must stay zero.

## 3. Idle sleep and duty accounting — `opt_sleep.c`

"Under 10% CPU while idling in continuous listening mode" is a ratio, and
firmware that cannot state both halves of it cannot claim it. `opt_sleep`
accumulates busy cycles reported by the listening pipeline and divides by
elapsed cycles over a one-second window, both from `DWT->CYCCNT`, so the ratio
survives a change of core frequency.

What it counts is everything the listening state does — capture handling,
resampling, the front end, the gate, inference when it runs, and the link
service — not just the model. Measuring the network alone, or the model alone,
would not be the number the statement asks for.

With nothing to draw, the main loop no longer spins on `HAL_Delay`; it services
the network and then waits in `WFI` for the next capture interrupt.

## 4. Early link — `opt_link.c`

Link negotiation, ARP and the TCP three-way handshake all happen while the
device is idle, and the connection is kept across utterances. By the time a
keyword is confirmed the only thing left to do is put bytes on the wire.

The model is trained to stay quiet until the whole phrase is over, so it cannot
give an early hint itself. The audio event detector can: the moment it opens,
`kws_pipeline` posts a speech event and the main loop calls
`opt_link_prepare()`, about a second before "Utho Kikkar" can be confirmed. On a
radio-based build this is where power saving would be left —
`esp_wifi_set_ps(WIFI_PS_NONE)` in the ESP32 form of this design. On Ethernet
there is no sleep state to leave, so it verifies the socket instead and
reconnects early if it has gone stale. Same hook, same place in the sequence.

Speech is not an activation. Nothing is transmitted and nothing is shown on
screen until the keyword is confirmed. `opt_link_prepares()` will be much
larger than `kws_detector_fire_count()`, and that is the intended behaviour.

## 5. Helium int8 kernels — `kws_kernels.c`

The dot product over a channel run is three quarters of the model's cost: the
collapse layer, the pointwise mixes and the head together are 34,352 of the
45,872 multiply-accumulates in a step. `kws_dot_s8()` is the only routine with
a vector path, and it is one accumulating instruction per sixteen taps:

```
vldrbt.u8   q3, [r3], #16
vldrbt.u8   q2, [r1], #16
vmlavat.s8  r0, q3, q2
```

Tail handling is predication (`vctp8q`), not a scalar epilogue, so a channel
count that is not a multiple of sixteen costs nothing extra.

The input zero point is folded into each layer's bias by
`tools/export_kws_model.py`, which is what makes the inner loop a plain signed
dot product in the first place. Padded taps are charged their zero-point
contribution separately, on the border pixels only.

The stem, whose input has a single channel, and the depthwise filters, a few
hundred taps a step, stay scalar, as do the requantisation and the residual
adds. Vectorising them would cost more in code than it saved in cycles.

---

## Where the RAM goes

Printed at start-up by `opt_metrics_report_budget()`:

| | bytes |
|---|---|
| capture ring, 48 kHz | 4,096 |
| pre-roll, 512 ms at 16 kHz | 16,384 |
| front end: running mean and band edges | 320 |
| shared arena: model, front end, FFT and look-back **or** the audio queue | 19,456 |
| Ethernet rings, 4 rx + 4 tx | 12,416 |
| IP frame staging + one TCP segment | 2,996 |
| sender: one audio message, one chunk | 1,284 |
| **voice path total** | **56,952** |

Against the statement's 256 KB that leaves the rest of the budget for stacks,
the C library and the application's own `.bss`. The flower's atlas and
framebuffers are counted separately and belong to the display, not to the voice
path — see `docs/measurements.md` for how the two are reported.

## Optimisations that were considered and left out

**Opus or another codec.** It would cut the 32 kB/s payload substantially. It
would also add encoder RAM, encoder CPU and algorithmic delay, all three of
which are the budgets under pressure, and it changes what the recogniser hears.
PCM first, measured; compression only if the bandwidth turns out to matter.

**TLS.** Correct for deployment, and its handshake peak is a RAM cost that has
to be measured rather than assumed. See the note at the end of
`docs/streaming-protocol.md`.

**UDP or RTP.** Worth investigating only if measured TCP retransmission delay
turns out to dominate, which it will not on a switched LAN. Reliable ordered
delivery of a short command is what this application wants.

**Deep sleep between hops.** The capture DMA has to keep running, so the core
can idle but the audio path cannot stop. `WFI` is the right depth; anything
deeper breaks capture, which is the same conclusion the ESP32 form of this
design reached about light sleep and I2S.
