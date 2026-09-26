# Architecture

One microphone, three consumers, two priorities above the render loop.

```
                     PDM microphone
                           │
                    MDF ── GPDMA ── capture ring (48 kHz q15, 4 KiB)
                           │
        ┌──────────────────┴──────────────────┐
        │  GPDMA ISR, priority 0              │
        │  one masked store per sample,       │
        │  one register write per block       │
        └──────────────────┬──────────────────┘
                           │  pends PendSV
        ┌──────────────────┴──────────────────┐
        │  PendSV, priority 5                 │
        │                                     │
        │  flower_audio_task()                │   level, noise floor, 3 bands
        │  kikkar_app_audio_task()            │
        │    └ kws_pipeline_task()            │
        │        ├ kws_resample  48k → 16k    │   3:1 decimating FIR
        │        ├ kws_preroll   512 ms ring  │   also the sender's rewind buffer
        │        ├ kws_frontend  log-mel × 40 │   one 30 ms frame every 10 ms
        │        ├ opt_gate      silence?     │   skips the next stage
        │        ├ kws_model     int8 DS-CNN  │   one step per 30 ms, gate open
        │        └ kws_detector  3-step mean  │   at most one event per utterance
        └──────────────────┬──────────────────┘
                           │  latched event
        ┌──────────────────┴──────────────────┐
        │  thread mode, main loop             │
        │                                     │
        │  kikkar_app_tick()                  │
        │    ├ opt_link_task()                │   PHY, ARP, TCP timers
        │    ├ stream_session_pump()          │   pre-roll → queue → wire
        │    └ state machine                  │   bloom, wilt, recover
        │                                     │
        │  flower_anim_tick() + render        │   DMA2D, LTDC on VBLANK
        └─────────────────────────────────────┘
```

The priority split is the one the animation already established, and the
keyword spotter slots into it without changing it. The capture interrupt stays
O(1) and integer-only; every floating-point and int8 operation in the listening
path happens in PendSV, which cannot delay a microphone interrupt and cannot be
delayed by a long frame. The network never runs at interrupt priority at all.

## Why the capture ring is shared

The animation's DSP and the keyword front end want the same samples at
different rates. Rather than tapping the DMA callback twice, `kws_resample`
reads `g_flower_audio_ring` with its own cursor. Adding wake-word detection
therefore costs the microphone interrupt exactly nothing, and there is one
place in the firmware where captured audio lives.

## Why the pre-roll is also the feature source

`kws_preroll` holds 512 ms of 16 kHz audio. The front end reads analysis frames
out of it, and after a trigger the streaming session rewinds into it by
`KWS_LOOKBACK_MS` and starts sending from there. One buffer, two readers, both
addressing it by free-running absolute sample index so neither has to know where
the write cursor is.

This is what answers the problem the statement describes but does not name: the
detector cannot decide a keyword has ended until after it has ended. Ours
decides 60 ms later, and a command said in the same breath has already begun
by then. Sending from `trigger − 320 ms` recovers it.

## Why the model streams

The model is a streaming depthwise-separable CNN. It never sees a window of
audio; it sees one step, the three newest log-mel frames, every 30 ms. Each
layer that looks back in time keeps the few rows it needs from earlier steps in
a ring inside the model's arena: two frames for the stem, and 4, 8 or 12 steps
for the temporal blocks, depending on their dilation. That is 2,384 bytes of
state and about 1.5 s of context, and every step costs the same 45,872
multiply-accumulates, however long the model has been listening.

The rings live in the same shared arena as the activations, so when the
arena goes to the sender the history goes with it. That is fine: after an
utterance the model starts again from silence, which is exactly how every
training clip began.

## Module map

| Directory | What is in it |
|---|---|
| `Appli/Core/Src/kws/` | resampler, pre-roll, FFT, log-mel front end, int8 kernels, streaming model interpreter, detector, pipeline |
| `Appli/Core/Src/net/` | ETH1 MAC and RTL8211F PHY, ARP/IPv4/ICMP, one-socket TCP client |
| `Appli/Core/Src/stream/` | wire format, bounded chunk queue, utterance session |
| `Appli/Core/Src/opt/` | shared arena, silence gate, idle sleep, early link, metrics |
| `Appli/Core/Src/app/` | the state machine that connects them |
| `Appli/Core/Src/flower_*.c` | the animation, unchanged by this work |

Every tunable in all five directories is in
[`Appli/Core/Inc/app/kikkar_config.h`](../Appli/Core/Inc/app/kikkar_config.h).

## The three states

```
                  keyword confirmed
   LISTENING ─────────────────────────▶ STREAMING
       ▲                                    │
       │        END sent, arena returned     │
       └────────────────────────────────────┘
       ▲                                    │
       │    link came back    link lost      ▼
       └──────────────── RECOVERING ◀────────┘
```

`LISTENING` runs the front end and the model, keeps the TCP connection up and
leaves the panel dark. `STREAMING` blooms the flower, pauses inference and hands
the arena to the sender. `RECOVERING` is what happens when a trigger arrives
with no usable socket: the session is abandoned, the failure is counted, and
listening continues rather than the firmware wedging.

## What runs where, and what it costs

| Stage | Where | When |
|---|---|---|
| sample capture | GPDMA ISR | every sample |
| level, floor, bands | PendSV | every 5.3 ms hop |
| resample 48 k → 16 k | PendSV | every hop |
| log-mel frame | PendSV | every 10 ms |
| model step | PendSV | every 30 ms, **only while the gate is open** |
| trigger policy | PendSV | after each step |
| link service | thread | every tick |
| audio send | thread | every tick, while streaming |
| composite and present | thread | every frame, only while blooming |

The last column is the CPU argument. During silence the model does not run at
all, and `DORMANT` submits no GPU work — the core spends the gap between hops in
`WFI`.
