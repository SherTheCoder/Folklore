# The keyword model

The trained model lives on Hugging Face, not in this repository. Download it
into this directory:

```sh
curl -L -o model/kikkar_kws_int8.tflite \
  https://huggingface.co/SherTheCoder/TeamFolklore_Model/resolve/main/kikkar_kws_int8.tflite
```

or

```sh
huggingface-cli download SherTheCoder/TeamFolklore_Model kikkar_kws_int8.tflite \
  --local-dir model/
```

Then:

```sh
make model     # re-exports Appli/Core/Src/kws/kws_model.bin
make           # links it into .kws_model
```

`make model` picks up the first `*.tflite` it finds here. With none present it
writes an untrained placeholder instead, which has the right shapes and links
cleanly, so the firmware builds and runs before the model arrives and says so
on the serial port rather than pretending:

```
  model              39120 B in flash, 23 ops, 31472 int8 weights, "Utho Kikkar", untrained placeholder
```

## What the model is

A streaming depthwise-separable CNN, trained from scratch for "Utho Kikkar":
32,849 parameters, 45,872 multiply-accumulates per 30 ms step, about 44 KB as
a `.tflite`. The model card on Hugging Face has the full story; this is what
the firmware needs to know.

```
step input   3 frames x 40 mel bands                       int8
S1   stem    conv 3x3, stride (1,2), 1 -> 16      -> 3 x 20 x 16   keeps 2 frames
S2a  DS      depthwise 3x3, stride (3,2)          -> 1 x 10 x 16
S2b  DS      pointwise 16 -> 32                   -> 1 x 10 x 32
S3   collapse 320 -> 48                           -> 48
B1..B6       depthwise over time, k 5, dilation 1,1,2,2,3,3,
             pointwise 48 -> 48, residual add     -> 48            keeps 4d steps
H    head    pointwise 48 -> 1, sigmoid in float  -> 1
```

The layers that look back in time keep their history between steps: 80 bytes
for the stem and 192 to 576 bytes for each temporal block, 2,384 bytes in all.
On the board that history sits in rings next to three activation slots, and the
whole model needs a 5,264-byte arena. It shares that arena with the sender, so
the history starts again from silence after every utterance, which is also how
the model was trained to begin.

## What the firmware expects

| | |
|---|---|
| Input | 3 × 40 `int8` per step, per-tensor quantised |
| Output | one `int8` logit, turned into a probability with a float sigmoid |
| Operators | `CONV_2D`, `DEPTHWISE_CONV_2D`, `FULLY_CONNECTED`, `ADD`, plus the state and shape operators the exporter skips |
| Quantisation | full-integer `int8`, symmetric per-channel weights, `int32` biases |
| Trigger | the last 3 steps averaged, threshold `KWS_THRESHOLD` (0.7), then 1 s of refractory |

`tools/export_kws_model.py` walks the `.tflite`, matches its arithmetic
operators to the architecture above one by one, and refuses the file if a layer
is missing, out of place or the wrong size. The `READ_VARIABLE`,
`CONCATENATION` and `ASSIGN_VARIABLE` operators that carry the streaming state
inside the `.tflite` are skipped; the firmware keeps that state itself.

`kws_model_load()` checks the step and the number of mel bands against
`kikkar_config.h` at start-up, and every layer against the arena it will run in,
so an export that does not fit fails on the first boot rather than in the
accuracy figures.

## What the front end does before the model sees anything

These have to match the training pipeline exactly. They are set in
`Appli/Core/Inc/app/kikkar_config.h`, and the model card has the same front end
in Python:

```
sample rate      16000 Hz, mono
window           480 samples (30 ms), Hann
hop              160 samples (10 ms)
FFT              512 points, power spectrum
mel bands        40, 20 Hz .. 7600 Hz, HTK triangles
compression      natural log, floored at -80 dB
normalisation    running mean per band subtracted, 3 s time constant,
                 started from the first 0.5 s after boot
step             3 frames, every 30 ms
```

## The threshold

`KWS_THRESHOLD` is 0.7, and it belongs to the int8 model. Quantisation moves
the operating point, so pick it on the DET curve of the `.tflite` you are
flashing, never from the float model it came from, and quote it, together with
the 3-step smoothing and the 1 s refractory, next to any false-activation
figure.

## Training

Training happens on a workstation and is not part of this repository. What the
firmware needs is the `.tflite` above and the provenance to go with it: about
2,000 real recordings split by speaker, several thousand synthetic renditions
from Indian text-to-speech engines, the confusable negatives and the
hard-negative rounds, all described on the model card. `docs/measurements.md`
has the reporting template.

The model satisfies both constraints from the problem statement:

* it is trained for the project's own keyword, not adapted from a pretrained
  model for a commercial assistant's wake word;
* it is a 44 KB depthwise-separable CNN trained from scratch, nowhere near a
  heavy or uncompressed pretrained transformer.
