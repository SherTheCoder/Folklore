#!/usr/bin/env python3

import argparse
import os
import struct
import sys

MAGIC = 0x4D53574B
VERSION = 2
LABEL_BYTES = 16
FLAG_PLACEHOLDER = 0x0001

OP_CONV = 1
OP_DWCONV = 2
OP_ADD = 3

INPUT = 0xFF
SLOTS = 3
ADD_LEFT_SHIFT = 20

HEADER_FMT = "<IHHHHHHfifiIII16s"
LAYER_FMT = "<BBBBBBBBHHHHHHHHHHiiiIIII"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_BIN = os.path.join(ROOT, "Appli", "Core", "Src", "kws", "kws_model.bin")
OUT_ASM = os.path.join(ROOT, "Appli", "Core", "Src", "kws", "kws_model_blob.S")

STEP_FRAMES = 3
MEL_BINS = 40
CHANNELS = 48
DILATIONS = (1, 1, 2, 2, 3, 3)

PLUMBING = {
    "VAR_HANDLE", "READ_VARIABLE", "ASSIGN_VARIABLE", "CALL_ONCE",
    "CONCATENATION", "STRIDED_SLICE", "SLICE", "RESHAPE", "EXPAND_DIMS",
    "SQUEEZE", "QUANTIZE", "DEQUANTIZE", "LOGISTIC", "PAD",
}


class Layer(object):
    """One entry in the firmware's layer table."""

    def __init__(self, name, op, src, dst, k=(1, 1), stride=(1, 1), dilation=1,
                 shape_in=(1, 1, 1), shape_out=(1, 1, 1), hist=0, pad_f=0,
                 relu=0, aux=0):
        self.name = name
        self.op = op
        self.src, self.dst, self.aux = src, dst, aux
        self.k = k
        self.stride = stride
        self.dilation = dilation
        self.shape_in = shape_in
        self.shape_out = shape_out
        self.hist = hist
        self.pad_f = pad_f
        self.relu = relu
        self.in_zero = 0
        self.out_zero = 0
        self.aux_zero = 0
        self.weights = b""
        self.bias = []
        self.ratios = []

    def weight_count(self):
        taps = self.k[0] * self.k[1]
        if self.op == OP_CONV:
            return taps * self.shape_in[2] * self.shape_out[2]
        if self.op == OP_DWCONV:
            return taps * self.shape_in[2]
        return 0

    def state_bytes(self):
        return self.hist * self.shape_in[1] * self.shape_in[2]


def architecture():
    """The streaming DS-CNN from the model card, as 23 firmware ops."""
    C = CHANNELS
    layers = [
        Layer("S1", OP_CONV, INPUT, 0, k=(3, 3), stride=(1, 2),
              shape_in=(STEP_FRAMES, MEL_BINS, 1), shape_out=(3, 20, 16),
              hist=2, pad_f=1, relu=1),
        Layer("S2a", OP_DWCONV, 0, 1, k=(3, 3), stride=(3, 2),
              shape_in=(3, 20, 16), shape_out=(1, 10, 16), pad_f=1, relu=1),
        Layer("S2b", OP_CONV, 1, 0, shape_in=(1, 10, 16), shape_out=(1, 10, 32),
              relu=1),
        Layer("S3", OP_CONV, 0, 1, shape_in=(1, 1, 320), shape_out=(1, 1, C),
              relu=1),
    ]
    for i, d in enumerate(DILATIONS):
        b = "B%d" % (i + 1)
        layers += [
            Layer(b + ".dw", OP_DWCONV, 1, 0, k=(5, 1), dilation=d,
                  shape_in=(1, 1, C), shape_out=(1, 1, C), hist=4 * d, relu=1),
            Layer(b + ".pw", OP_CONV, 0, 2, shape_in=(1, 1, C),
                  shape_out=(1, 1, C)),
            Layer(b + ".add", OP_ADD, 2, 1, aux=1, shape_in=(1, 1, C),
                  shape_out=(1, 1, C), relu=1),
        ]
    layers.append(Layer("H", OP_CONV, 1, 0, shape_in=(1, 1, C),
                        shape_out=(1, 1, 1)))
    return layers


def quantise_multiplier(scale):
    """Float ratio to the int32 multiplier and shift the firmware uses."""
    if scale <= 0.0:
        return 0, 0
    shift = 0
    while scale < 0.5:
        scale *= 2.0
        shift += 1
    while scale >= 1.0:
        scale /= 2.0
        shift -= 1
    mult = int(round(scale * (1 << 31)))
    if mult == (1 << 31):
        mult //= 2
        shift -= 1
    return mult, shift


def signed(byte):
    return byte - 256 if byte > 127 else byte


def fold_input_offset(layer):
    """Moves the input zero point into the bias."""
    if layer.op == OP_ADD:
        return []
    out_c = layer.shape_out[2]
    taps = layer.k[0] * layer.k[1]
    folded = []
    for o in range(out_c):
        if layer.op == OP_DWCONV:
            total = sum(signed(layer.weights[t * out_c + o]) for t in range(taps))
        else:
            run = taps * layer.shape_in[2]
            total = sum(signed(v) for v in layer.weights[o * run:(o + 1) * run])
        folded.append(int(layer.bias[o]) - layer.in_zero * total)
    return folded


def add_ratios(in1_scale, in2_scale, out_scale):
    """Rescaling ratios for TFLite's int8 ADD: input 1, input 2, output."""
    twice_max = 2.0 * max(in1_scale, in2_scale)
    return [in1_scale / twice_max, in2_scale / twice_max,
            twice_max / ((1 << ADD_LEFT_SHIFT) * out_scale)]


def pad4(buf):
    while len(buf) % 4:
        buf.append(0)


def pack(layers, label, in_q, out_q, flags):
    header_size = struct.calcsize(HEADER_FMT)
    layer_size = struct.calcsize(LAYER_FMT)
    payload_off = header_size + layer_size * len(layers)

    slot = 0
    state = 0
    payload = bytearray()
    descriptors = bytearray()

    for layer in layers:
        slot = max(slot, layer.shape_out[0] * layer.shape_out[1] * layer.shape_out[2],
                   layer.shape_in[0] * layer.shape_in[1] * layer.shape_in[2])

        w_off = len(payload)
        payload += bytes(layer.weights)
        pad4(payload)

        b_off = len(payload)
        for value in fold_input_offset(layer):
            payload += struct.pack("<i", value)

        q_off = len(payload)
        pairs = [quantise_multiplier(r) for r in layer.ratios]
        for mult, _ in pairs:
            payload += struct.pack("<i", mult)
        for _, shift in pairs:
            payload += struct.pack("<b", shift)
        pad4(payload)

        descriptors += struct.pack(
            LAYER_FMT,
            layer.op, layer.relu,
            layer.src, layer.dst, layer.aux,
            layer.stride[0], layer.stride[1], layer.dilation,
            layer.k[0], layer.k[1],
            layer.shape_in[0], layer.shape_in[1], layer.shape_in[2],
            layer.shape_out[0], layer.shape_out[1], layer.shape_out[2],
            layer.hist, layer.pad_f,
            layer.in_zero, layer.out_zero, layer.aux_zero,
            state,
            w_off, b_off, q_off,
        )
        state += layer.state_bytes()

    slot = (slot + 15) // 16 * 16
    header = struct.pack(
        HEADER_FMT,
        MAGIC, VERSION, len(layers),
        STEP_FRAMES, MEL_BINS, flags, state,
        in_q[0], in_q[1],
        out_q[0], out_q[1],
        SLOTS * slot + state,
        payload_off, len(payload),
        label.encode("ascii")[:LABEL_BYTES],
    )
    return header + bytes(descriptors) + bytes(payload)


def placeholder(layers):
    """Untrained blob with the right shapes, so the firmware builds without one."""
    act, weight = 0.05, 0.01
    for layer in layers:
        layer.weights = bytes(layer.weight_count())
        if layer.op == OP_ADD:
            layer.ratios = add_ratios(act, act, act)
        else:
            layer.bias = [0] * layer.shape_out[2]
            layer.ratios = [act * weight / act] * layer.shape_out[2]
    return (0.1, 0), (0.1, 0)


def from_tflite(path, layers):
    """Matches the .tflite's arithmetic ops to architecture() and copies them."""
    try:
        import numpy as np
        import tensorflow as tf
    except ImportError:
        sys.exit("tensorflow is needed to read a .tflite file; "
                 "use --placeholder to build without one")

    interp = tf.lite.Interpreter(model_path=path)
    interp.allocate_tensors()
    tensors = {t["index"]: t for t in interp.get_tensor_details()}

    def quant(index):
        q = tensors[index]["quantization_parameters"]
        return float(q["scales"][0]), int(q["zero_points"][0])

    ops = []
    for op in interp._get_ops_details():
        name = op["op_name"]
        if name in PLUMBING:
            continue
        if name not in ("CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED", "ADD"):
            sys.exit("unsupported operator %s -- add a kernel for it in "
                     "kws_kernels.c before exporting" % name)
        ops.append(op)

    if len(ops) != len(layers):
        sys.exit("expected %d arithmetic operators, found %d -- is this the "
                 "streaming model from the model card?" % (len(layers), len(ops)))

    previous_out = None
    for layer, op in zip(layers, ops):
        name = op["op_name"]
        inputs, output = list(op["inputs"]), op["outputs"][0]
        want = {OP_CONV: ("CONV_2D", "FULLY_CONNECTED"),
                OP_DWCONV: ("DEPTHWISE_CONV_2D",), OP_ADD: ("ADD",)}[layer.op]
        if name not in want:
            sys.exit("%s: expected %s, found %s" % (layer.name, want[0], name))

        out_scale, layer.out_zero = quant(output)

        if layer.op == OP_ADD:
            branch = previous_out if previous_out in inputs else inputs[0]
            residual = [i for i in inputs if i != branch][0]
            s1, layer.in_zero = quant(branch)
            s2, layer.aux_zero = quant(residual)
            layer.ratios = add_ratios(s1, s2, out_scale)
        else:
            in_scale, layer.in_zero = quant(inputs[0])
            w = interp.get_tensor(inputs[1]).astype(np.int8)
            if w.size != layer.weight_count():
                sys.exit("%s: %d weights, the architecture says %d"
                         % (layer.name, w.size, layer.weight_count()))
            w_scales = tensors[inputs[1]]["quantization_parameters"]["scales"]
            if len(w_scales) == 1:
                w_scales = [w_scales[0]] * layer.shape_out[2]
            layer.weights = w.reshape(-1).tobytes()
            layer.bias = (interp.get_tensor(inputs[2]).astype(np.int64).tolist()
                          if len(inputs) > 2 and inputs[2] >= 0
                          else [0] * layer.shape_out[2])
            layer.ratios = [in_scale * float(s) / out_scale for s in w_scales]
        previous_out = output

    first = interp.get_input_details()[0]["index"]
    return quant(first), quant(ops[-1]["outputs"][0])


def write_assembly(path):
    with open(path, "w") as f:
        f.write(
            '/* Generated by tools/export_kws_model.py. Do not edit. */\n'
            '#ifndef KWS_MODEL_BIN\n'
            '#define KWS_MODEL_BIN "../Core/Src/kws/kws_model.bin"\n'
            '#endif\n'
            '\n'
            '    .section .kws_model, "a", %progbits\n'
            '    .global  kws_model_blob\n'
            '    .global  kws_model_blob_end\n'
            '    .type    kws_model_blob, %object\n'
            '    .balign  8\n'
            'kws_model_blob:\n'
            '    .incbin  KWS_MODEL_BIN\n'
            'kws_model_blob_end:\n'
            '    .size    kws_model_blob, kws_model_blob_end - kws_model_blob\n')


def main():
    ap = argparse.ArgumentParser(
        description="Convert the int8 streaming .tflite into the firmware's model blob.")
    ap.add_argument("tflite", nargs="?", help="trained int8 .tflite file")
    ap.add_argument("--placeholder", action="store_true",
                    help="emit an untrained blob with the right shapes")
    ap.add_argument("--label", default="Utho Kikkar", help="wake word the model detects")
    args = ap.parse_args()

    layers = architecture()
    if args.placeholder or not args.tflite:
        in_q, out_q = placeholder(layers)
        flags = FLAG_PLACEHOLDER
        note = "placeholder, untrained"
    else:
        in_q, out_q = from_tflite(args.tflite, layers)
        flags = 0
        note = os.path.basename(args.tflite)

    blob = pack(layers, args.label, in_q, out_q, flags)

    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    with open(OUT_BIN, "wb") as f:
        f.write(blob)
    write_assembly(OUT_ASM)

    weights = sum(len(l.weights) for l in layers)
    state = sum(l.state_bytes() for l in layers)
    print("kws model: %s" % note)
    print("  %d ops in 5 stages, keyword \"%s\"" % (len(layers), args.label))
    print("  %d int8 weights, %d B of streaming state" % (weights, state))
    print("  %d B total -> .kws_model" % len(blob))
    print("  input %dx%d int8 per 30 ms step, scale %g zero %d"
          % (STEP_FRAMES, MEL_BINS, in_q[0], in_q[1]))


if __name__ == "__main__":
    main()
