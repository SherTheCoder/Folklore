#!/usr/bin/env python3

import argparse
import os
import socket
import struct
import subprocess
import sys
import time
import wave

HDR = struct.Struct("<BBH")
START = struct.Struct("<BBBBIII16s")
END = struct.Struct("<III")

MSG_START, MSG_AUDIO, MSG_END, MSG_RESULT, MSG_ERROR = 1, 2, 3, 4, 5
FLAG_PREROLL, FLAG_FIRST = 0x01, 0x02


class Utterance(object):
    def __init__(self, info, out_dir):
        self.info = info
        self.out_dir = out_dir
        self.pcm = bytearray()
        self.preroll_bytes = 0
        self.opened = time.monotonic()
        self.first_command_audio = None
        self.chunks = 0

    def add(self, flags, payload):
        self.pcm += payload
        self.chunks += 1
        if flags & FLAG_PREROLL:
            self.preroll_bytes += len(payload)
        elif self.first_command_audio is None:
            self.first_command_audio = time.monotonic()

    def write(self):
        if not os.path.isdir(self.out_dir):
            os.makedirs(self.out_dir)
        path = os.path.join(
            self.out_dir, "utterance-%04d.wav" % self.info["utterance"])
        with wave.open(path, "wb") as w:
            w.setnchannels(self.info["channels"])
            w.setsampwidth(self.info["sample_bits"] // 8)
            w.setframerate(self.info["sample_rate"])
            w.writeframes(bytes(self.pcm))
        return path


def parse_header(buf):
    """Returns None until the whole message is in the buffer."""
    if len(buf) < HDR.size:
        return None
    msg_type = buf[0]
    flags = buf[1]
    length = (buf[2] << 8) | buf[3]
    if len(buf) < HDR.size + length:
        return None
    payload = bytes(buf[HDR.size:HDR.size + length])
    del buf[:HDR.size + length]
    return msg_type, flags, payload


def decode_start(payload):
    version, channels, bits, little, rate, utt, preroll, keyword = \
        START.unpack_from(payload)
    return {
        "version": version,
        "channels": channels,
        "sample_bits": bits,
        "little_endian": bool(little),
        "sample_rate": rate,
        "utterance": utt,
        "preroll_samples": preroll,
        "keyword": keyword.split(b"\0")[0].decode("ascii", "replace"),
    }


def run_asr(command, path):
    try:
        out = subprocess.run(command.replace("{}", path), shell=True,
                             capture_output=True, timeout=120)
        return out.stdout.decode("utf-8", "replace").strip()
    except Exception as exc:                       # noqa: BLE001
        return "ASR failed: %s" % exc


def serve_one(conn, addr, args):
    print("connected: %s:%d" % addr)
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = bytearray()
    current = None
    total_payload = 0
    total_overhead = 0

    while True:
        data = conn.recv(65536)
        if not data:
            break
        buf += data

        while True:
            msg = parse_header(buf)
            if msg is None:
                break
            msg_type, flags, payload = msg
            total_overhead += HDR.size

            if msg_type == MSG_START:
                info = decode_start(payload)
                current = Utterance(info, args.out)
                total_overhead += len(payload)
                print("START  utterance %d  keyword %r  %d Hz  %d-bit  "
                      "pre-roll %d samples"
                      % (info["utterance"], info["keyword"],
                         info["sample_rate"], info["sample_bits"],
                         info["preroll_samples"]))

            elif msg_type == MSG_AUDIO and current is not None:
                current.add(flags, payload)
                total_payload += len(payload)

            elif msg_type == MSG_END and current is not None:
                utt, samples, reason = END.unpack_from(payload)
                total_overhead += len(payload)
                path = current.write()
                first = current.first_command_audio
                delay = (first - current.opened) * 1000.0 if first else float("nan")
                print("END    utterance %d  %d samples  %d chunks  reason %d"
                      % (utt, samples, current.chunks, reason))
                print("       first post-keyword audio %.1f ms after START"
                      % delay)
                print("       payload %d B  overhead %d B  (%.2f%%)"
                      % (total_payload, total_overhead,
                         100.0 * total_overhead /
                         max(1, total_payload + total_overhead)))
                print("       wrote %s" % path)
                if args.asr:
                    print("       transcript: %s" % run_asr(args.asr, path))
                current = None

            elif msg_type in (MSG_RESULT, MSG_ERROR):
                print("unexpected client message type %d" % msg_type)

    print("closed: %s:%d" % addr)


def main():
    ap = argparse.ArgumentParser(description='Receive Kikkar audio streams, save each command as a WAV and optionally run ASR.',
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--out", default="captures",
                    help="directory for the reassembled WAV files")
    ap.add_argument("--asr", default=None,
                    help="command to transcribe each WAV; {} is the path")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print("kikkar asr gateway listening on %s:%d" % (args.host, args.port))

    try:
        while True:
            conn, addr = srv.accept()
            try:
                serve_one(conn, addr, args)
            except ConnectionResetError:
                print("reset by %s:%d" % addr)
            finally:
                conn.close()
    except KeyboardInterrupt:
        print("\nstopped")
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
