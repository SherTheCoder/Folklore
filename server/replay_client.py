#!/usr/bin/env python3

import argparse
import socket
import struct
import sys
import time
import wave

HDR = struct.Struct("<BBH")
START = struct.Struct("<BBBBIII16s")
END = struct.Struct("<III")

MSG_START, MSG_AUDIO, MSG_END = 1, 2, 3
FLAG_PREROLL, FLAG_FIRST = 0x01, 0x02

CHUNK_SAMPLES = 320


def frame(msg_type, flags, payload):
    return bytes([msg_type, flags, (len(payload) >> 8) & 0xFF,
                  len(payload) & 0xFF]) + payload


def main():
    ap = argparse.ArgumentParser(description='Send a WAV file to asr_gateway.py the way the board does.',
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--utterance", type=int, default=1)
    ap.add_argument("--preroll-ms", type=int, default=320,
                    help="how much of the file to mark as pre-keyword audio")
    ap.add_argument("--realtime", action="store_true",
                    help="pace chunks at one per 20 ms, as the board does")
    ap.add_argument("--keyword", default="Utho Kikkar")
    args = ap.parse_args()

    with wave.open(args.wav, "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            sys.exit("expected mono 16-bit PCM")
        rate = w.getframerate()
        pcm = w.readframes(w.getnframes())

    preroll_samples = args.preroll_ms * rate // 1000
    chunk_bytes = CHUNK_SAMPLES * 2

    sock = socket.create_connection((args.host, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    sock.sendall(frame(MSG_START, 0, START.pack(
        1, 1, 16, 1, rate, args.utterance, preroll_samples,
        args.keyword.encode("ascii")[:16].ljust(16, b"\0"))))

    sent = 0
    offset = 0
    while offset < len(pcm):
        payload = pcm[offset:offset + chunk_bytes]
        if len(payload) < chunk_bytes:
            payload = payload + b"\0" * (chunk_bytes - len(payload))
        flags = 0
        if sent == 0:
            flags |= FLAG_FIRST
        if offset < preroll_samples * 2:
            flags |= FLAG_PREROLL
        sock.sendall(frame(MSG_AUDIO, flags, payload))
        offset += chunk_bytes
        sent += 1
        if args.realtime:
            time.sleep(CHUNK_SAMPLES / float(rate))

    sock.sendall(frame(MSG_END, 0, END.pack(args.utterance,
                                            sent * CHUNK_SAMPLES, 0)))
    print("sent %d chunks (%d samples) as utterance %d"
          % (sent, sent * CHUNK_SAMPLES, args.utterance))
    time.sleep(0.2)
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
