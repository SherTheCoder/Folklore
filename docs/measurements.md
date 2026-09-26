# Measuring this thing honestly

Four metrics, four different denominators. This file is the protocol for each
and the template for reporting them. **The results tables are empty on
purpose** — they are for numbers taken from hardware, and nothing in this
repository should be read as a measurement until they are filled in.

What the firmware gives you is the instrumentation, not the answer.

---

## 1. Model size and firmware footprint

These are three separate numbers and quoting one for another is the most common
way to make a submission look better than it is.

```sh
make size
```

Reports, from the current build:

| Section | Bytes | What it is |
|---|---|---|
| `.kws_model` | 39,120 | the exported model: 23 operations, 31,472 int8 weights |
| `.text` + `.rodata` | 94,816 | firmware code and constants |
| `.kikkar_ram` | 38,848 | the voice path's buffers, NOLOAD |
| `.noncacheable` | 13,952 | DMA-visible buffers, including the Ethernet rings |
| `.flower_atlas` | 1,352,384 | the animation's textures — **not** part of the voice path |

`.kws_model` has its own linker section for exactly this reason: `size -A`
reports what the model costs separately from the firmware that runs it. It is
smaller than the 44 KB `.tflite` because the FlatBuffer structure is gone and
the mel filterbank and Hann window are built in RAM at start-up.

## 2. Peak RAM

The accounting boundary this project uses:

> Peak edge RAM = everything simultaneously resident in the listening,
> activation and streaming phases, counted once.

`opt_metrics_report_budget()` prints it at start-up. The shared arena appears
once, not once per phase, because that is what it is.

To check the claim rather than trust it:

```
arena conflicts   must be 0   (opt_arena_conflicts)
queue high water  ≤ capacity  (stream_txq_high_water / _capacity)
hop overruns      must be 0   (g_flower_hop_overrun)
```

A non-zero conflict count means the two phases overlapped and the overlay is not
safe; the budget figure is then wrong and the fix is a lifetime bug, not a
bigger buffer.

| | Measured |
|---|---|
| Listening, steady state | |
| During TCP connect | |
| During streaming | |
| Peak across all three | |

## 3. Idle CPU

The statement says *under* 10%, strictly. Report the denominator with the
number.

```
device        STM32N657X0HXQ, Cortex-M55, single core
clock         600 MHz (FLOWER_SET_CPU_CLOCK)
included      capture ISR, PendSV DSP, resampler, front end, gate,
              inference when the gate opens, link service, main loop
window        1 s, DWT->CYCCNT for both numerator and denominator
```

The live line reports it every `KIKKAR_REPORT_MS`:

```
cpu=1.8% peak=7.4% | gate=6% infer=214 skip=3352 fe=310us nn=0us | ...
```

`cpu` is the last completed window, `peak` the worst since reset. `gate` is the
proportion of 30 ms steps that reached the model, which is the multiplier
between idle and active cost.

Test under three conditions, not one — processing cost changes with acoustic
activity and an average taken in a silent room proves nothing:

| Condition | `cpu` mean | `cpu` p95 | `gate` duty |
|---|---|---|---|
| Digital silence | | | |
| Quiet room | | | |
| Background speech (radio, no keyword) | | | |
| Keyword and command | | | |

Also confirm no audio deadlines were missed while averaging looked fine:
`g_flower_hop_overrun` must be zero throughout.

## 4. Recall and false activations

One spoken keyword must count as one event. The trigger policy that makes that
true is `KWS_SMOOTH_STEPS`, `KWS_THRESHOLD` and `KWS_REFRACTORY_MS`, and all
three have to be quoted alongside any figure below. The threshold must be the
one picked on the int8 model's DET curve, not the float model's.

| | Count | Denominator | Rate |
|---|---|---|---|
| True positives | | utterances spoken | |
| False rejects | | utterances spoken | |
| False activations | | hours of negative audio | |

Report the speakers, distances, noise conditions and the duration of the
negative audio. A long refractory period lowers the false-activation rate and
also prevents legitimate back-to-back interactions, so disclose it. This build
uses a threshold of 0.7 on the average of 3 steps, and a 1000 ms refractory
period.

Two kinds of negative matter more than the rest for this model: half the phrase
(*utho* or *kikkar* alone) and the phrase reversed (*kikkar utho*). Both must
produce zero activations, and each deserves its own row.

Keep evaluation speakers out of the training set, and keep augmented copies of a
recording in the same split as the original.

## 5. Handoff latency

The metric is *keyword end → cloud ASR receiving the audio stream*. The device
can only measure its own half. Both halves are instrumented; combining them is
only valid if the two clocks share a reference.

**Device half** — `stream_session_first_audio_us()`, printed after every
utterance:

```
handoff: utterance 3 | trigger->first audio 412 us | 21440 samples |
         payload 42880 B overhead 268 B (0.6%) | queue peak 3 of 29 | ...
```

That is `t_detect → first audio byte accepted by TCP`. It excludes
`t_end → t_detect`, which is detection delay, and it excludes the network.

**Server half** — `asr_gateway.py` prints when the first non-`PREROLL` chunk
arrived, relative to `START`:

```
END    utterance 3  21440 samples  67 chunks  reason 0
       first post-keyword audio 38.2 ms after START
```

**Detection delay** is the piece neither end measures directly. It is
`t_detect − t_end`, and the design puts it at 60 ms: up to 30 ms for the step
that holds the end of the phrase to finish, then one more 30 ms step. The
model's labels start one step before the phrase ends, so by then two of the
three steps in the `KWS_SMOOTH_STEPS` average are already high. To measure it
rather than trust it, play a recording with an annotated keyword end and
compare the annotation with `kws_detector_trigger_pos()`, which is an absolute
sample index in the same audio.

Note that this delay is *why the pre-roll exists*, and that the audio the server
receives first is from before the trigger — so first-arrival and
first-post-keyword-audio are different events and the table needs both.

| Trial | `t_detect − t_end` | device half | server half | total |
|---|---|---|---|---|
| median | | | | |
| p95 | | | | |
| failures | | | | |

Report already-connected and reconnecting trials separately. A handshake, an
empty packet or the `START` message is not delivery of command audio.

## 6. Network efficiency

```
payload bytes     stream_session_payload_bytes()
protocol bytes    stream_session_overhead_bytes()
retransmissions   net_tcp_retransmits()
dropped chunks    stream_txq_drops()
```

Three different quantities: useful audio, protocol overhead, and total bytes on
the wire. Quote all three. `stream_txq_drops()` must be zero — a non-zero value
means audio was discarded because the sender could not keep up, and any latency
figure taken from that run describes a lossy stream.

## What conditions to declare

Whatever is measured, state:

* board, silicon revision, core clock, and whether a debugger was attached
  (it changes the LCD pin configuration, and the reds it
  costs are cosmetic but the extra bus traffic is not);
* the exact firmware build and the model file's provenance;
* the network path — a switch on a bench is not an internet round trip and
  should not be reported as one;
* which optimisation switches were on.
