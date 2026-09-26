#!/usr/bin/env python3

import html
import math
import os
import random

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "media")

RESULTS = {
    "stm32": {"name": "STM32N657", "tier": "premium",
              "cpu": 0.5, "ram": 150, "latency": 80},
    "esp32": {"name": "ESP32", "tier": "budget",
              "cpu": 4.0, "ram": 190, "latency": 90},
}

FONT = ("-apple-system, BlinkMacSystemFont, 'Segoe UI', 'Noto Sans', "
        "Helvetica, Arial, sans-serif")

THEMES = {
    "light": {
        "ink": "#1f2328", "ink2": "#59636e", "hair": "#d1d9e0",
        "grid": "#eaeef2", "box": "#f6f8fa", "surface": "#ffffff",
        "stm": "#2a78d6", "esp": "#eb6834",
        "model": "#4a3aa7", "net": "#1baf7a", "bad": "#d03b3b",
    },
    "dark": {
        "ink": "#f0f6fc", "ink2": "#9198a1", "hair": "#3d444d",
        "grid": "#21262d", "box": "#151b23", "surface": "#0d1117",
        "stm": "#3987e5", "esp": "#d95926",
        "model": "#9085e9", "net": "#199e70", "bad": "#d03b3b",
    },
}


class Svg(object):
    """A tiny SVG writer, just enough for these figures."""

    def __init__(self, width, height, title, desc, theme):
        self.w, self.h, self.t = width, height, theme
        self.parts = []
        self.title, self.desc = title, desc

    def text(self, x, y, s, size=13, fill=None, weight=400, anchor="start",
             spacing=None, opacity=None):
        extra = ""
        if spacing:
            extra += ' letter-spacing="%s"' % spacing
        if opacity is not None:
            extra += ' fill-opacity="%.2f"' % opacity
        self.parts.append(
            '<text x="%.1f" y="%.1f" font-size="%s" font-weight="%d" '
            'fill="%s" text-anchor="%s"%s>%s</text>'
            % (x, y, size, weight, fill or self.t["ink"], anchor, extra,
               html.escape(s)))

    def rect(self, x, y, w, h, fill, rx=0, stroke=None, opacity=None):
        extra = ""
        if stroke:
            extra += ' stroke="%s" stroke-width="1"' % stroke
        if opacity is not None:
            extra += ' fill-opacity="%.2f"' % opacity
        self.parts.append(
            '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%s" '
            'fill="%s"%s/>' % (x, y, w, h, rx, fill, extra))

    def bar(self, x0, y, x1, h, fill, r=4):
        r = min(r, (x1 - x0) / 2.0, h / 2.0)
        self.parts.append(
            '<path d="M%.1f %.1f H%.1f A%.1f %.1f 0 0 1 %.1f %.1f V%.1f '
            'A%.1f %.1f 0 0 1 %.1f %.1f H%.1f Z" fill="%s"/>'
            % (x0, y, x1 - r, r, r, x1, y + r, y + h - r,
               r, r, x1 - r, y + h, x0, fill))

    def line(self, x0, y0, x1, y1, stroke, width=1, arrow=False):
        tip = ' marker-end="url(#arrow)"' if arrow else ""
        self.parts.append(
            '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" '
            'stroke-width="%s" stroke-linecap="round"%s/>'
            % (x0, y0, x1, y1, stroke, width, tip))

    def polyline(self, points, stroke, width=1.5, arrow=False):
        tip = ' marker-end="url(#arrow)"' if arrow else ""
        pts = " ".join("%.1f,%.1f" % p for p in points)
        self.parts.append(
            '<polyline points="%s" fill="none" stroke="%s" stroke-width="%s" '
            'stroke-linejoin="round" stroke-linecap="round"%s/>'
            % (pts, stroke, width, tip))

    def circle(self, cx, cy, r, fill):
        self.parts.append('<circle cx="%.1f" cy="%.1f" r="%s" fill="%s"/>'
                          % (cx, cy, r, fill))

    def raw(self, s):
        self.parts.append(s)

    def save(self, path):
        head = (
            '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
            'viewBox="0 0 %d %d" role="img" aria-labelledby="t d" '
            'font-family="%s">\n<title id="t">%s</title>\n<desc id="d">%s</desc>\n'
            '<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" '
            'markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
            '<path d="M0 1 L9 5 L0 9 Z" fill="%s"/></marker></defs>\n'
            % (self.w, self.h, self.w, self.h, FONT, html.escape(self.title),
               html.escape(self.desc), self.t["ink2"]))
        with open(path, "w") as f:
            f.write(head + "\n".join(self.parts) + "\n</svg>\n")


def contrast(a, b):
    """WCAG contrast ratio between two hex colours."""
    def lum(c):
        c = c.lstrip("#")
        ch = [int(c[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
        ch = [v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
              for v in ch]
        return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2]
    la, lb = lum(a), lum(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def label_ink(fill):
    return "#ffffff" if contrast(fill, "#ffffff") >= contrast(fill, "#1f2328") \
        else "#1f2328"


def metrics(theme, mode):
    s = Svg(880, 396,
            "Kikkar on STM32N657 and ESP32: idle CPU, RAM and latency",
            "STM32N657 premium: 0.5% idle CPU, 150 KB RAM, 80 ms latency. "
            "ESP32 budget: 4% idle CPU, 190 KB RAM, 90 ms latency. "
            "Limits: under 10% idle CPU and under 256 KB RAM.", theme)
    boards = [("stm32", theme["stm"]), ("esp32", theme["esp"])]

    lx = 124
    for key, colour in boards:
        r = RESULTS[key]
        s.rect(lx, 9, 11, 11, colour, rx=2)
        label = "%s, %s" % (r["name"], r["tier"])
        s.text(lx + 18, 19, label, size=13, fill=theme["ink2"])
        lx += 18 + 8 * len(label) + 28

    panels = [
        ("cpu", "Idle CPU while listening",
         "Lower is better. The problem statement asks for under 10%.",
         12.0, 10.0, "limit 10%", lambda v: "%g%%" % v),
        ("ram", "RAM used",
         "Lower is better. The limit is 256 KB.",
         300.0, 256.0, "limit 256 KB", lambda v: "%d KB" % v),
        ("latency", "Handoff latency",
         "From the end of the keyword to audio reaching the ASR server. "
         "Lower is better.",
         100.0, None, None, lambda v: "%d ms" % v),
    ]

    x0, span = 124.0, 700.0
    top = 34
    for i, (key, title, sub, domain, limit, limit_text, fmt) in enumerate(panels):
        y = top + i * 120
        s.text(0, y + 22, title, size=15, weight=600)
        s.text(0, y + 41, sub, size=12.5, fill=theme["ink2"])

        rows = [y + 56, y + 86]
        if limit is not None:
            lxp = x0 + span * limit / domain
            s.line(lxp, y + 49, lxp, y + 110, theme["ink2"], width=1.5)
            s.text(lxp + 6, y + 58, limit_text, size=11.5, fill=theme["ink2"])

        s.line(x0, rows[0] - 4, x0, rows[1] + 24, theme["hair"])
        for (board, colour), ry in zip(boards, rows):
            v = RESULTS[board][key]
            x1 = x0 + span * v / domain
            s.text(x0 - 12, ry + 15, RESULTS[board]["name"], size=12.5,
                   fill=theme["ink2"], anchor="end")
            s.bar(x0, ry, x1, 20, colour)
            s.text(x1 + 8, ry + 15, fmt(v), size=13, weight=600)

        if i < len(panels) - 1:
            s.line(0, y + 120, 880, y + 120, theme["grid"])

    s.save(os.path.join(OUT, "metrics-%s.svg" % mode))


def pipeline(theme, mode):
    s = Svg(880, 240,
            "How Kikkar works",
            "Always on, on the board: microphone, audio event detection, "
            "resample to 16 kHz, log-mel features every 10 ms, a streaming int8 "
            "DS-CNN every 30 ms, trigger. After the "
            "keyword: the flower blooms, and the pre-roll buffer feeds a sender "
            "that rewinds 320 ms, over an already-open TCP link to the cloud ASR.",
            theme)
    w, gap, left = 136, 12, 2
    xs = [left + i * (w + gap) for i in range(6)]
    cx = [x + w / 2.0 for x in xs]
    r1, r2, bh = 44, 158, 62

    def box(col, y, title, sub):
        lines = title.split("\n")
        s.rect(xs[col], y, w, bh, theme["box"], rx=8, stroke=theme["hair"])
        ty = y + (22 if len(lines) > 1 else 27)
        for line in lines:
            s.text(xs[col] + 11, ty, line, size=13, weight=600)
            ty += 15
        s.text(xs[col] + 11, ty + 3, sub, size=11, fill=theme["ink2"])

    s.text(left, 28, "ALWAYS ON, ON THE BOARD", size=11, weight=600,
           fill=theme["ink2"], spacing="0.06em")
    s.text(left, r2 + 27, "ONLY AFTER", size=11, weight=600,
           fill=theme["ink2"], spacing="0.06em")
    s.text(left, r2 + 43, "THE KEYWORD", size=11, weight=600,
           fill=theme["ink2"], spacing="0.06em")

    stages = [("Microphone", "48 kHz, always on"),
              ("Audio Event\nDetection", "is anyone talking?"),
              ("Resample", "48 to 16 kHz"),
              ("Log-mel\nfeatures", "40 bands / 10 ms"),
              ("Streaming\nDS-CNN", "int8, 44 KB"),
              ("Trigger", "one event per word")]
    for i, (t, sub) in enumerate(stages):
        box(i, r1, t, sub)
        if i:
            s.line(xs[i - 1] + w + 1, r1 + bh / 2, xs[i] - 3, r1 + bh / 2,
                   theme["ink2"], width=1.5, arrow=True)

    after = [(1, "Flower blooms", "on the STM32 panel"),
             (2, "Pre-roll", "last 512 ms kept"),
             (3, "Sender", "rewinds 320 ms"),
             (4, "TCP link", "already open"),
             (5, "Cloud ASR", "English, Hindi, Punjabi")]
    for col, t, sub in after:
        box(col, r2, t, sub)
    for a, b in ((2, 3), (3, 4), (4, 5)):
        s.line(xs[a] + w + 1, r2 + bh / 2, xs[b] - 3, r2 + bh / 2,
               theme["ink2"], width=1.5, arrow=True)

    bus = r1 + bh + 24
    s.polyline([(cx[5], r1 + bh + 1), (cx[5], bus), (cx[1], bus),
                (cx[1], r2 - 3)], theme["ink2"], arrow=True)
    s.line(cx[3], bus, cx[3], r2 - 3, theme["ink2"], width=1.5, arrow=True)
    s.circle(cx[3], bus, 4, theme["ink"])
    s.text(cx[4], bus - 7, "keyword confirmed", size=11.5, fill=theme["ink2"],
           anchor="middle")

    s.line(cx[2], bus - 6, cx[2], bus + 6, theme["surface"], width=8)
    s.line(cx[2], r1 + bh + 1, cx[2], r2 - 3, theme["ink2"], width=1.5,
           arrow=True)

    s.save(os.path.join(OUT, "pipeline-%s.svg" % mode))


def first_syllable(theme, mode):
    s = Svg(880, 266,
            "Why Kikkar rewinds before it streams",
            "Utho Kikkar ends, and the detector is sure 60 ms later. Said in one "
            "breath, the command has already begun by then. Sending from the "
            "trigger loses its start; Kikkar rewinds 320 ms into its 512 ms "
            "pre-roll and loses nothing.", theme)

    def X(t):
        return 60 + t * 400.0

    words = [(0.10, 0.20), (0.24, 0.40), (0.46, 0.60), (0.64, 0.80),
             (0.81, 1.01), (1.05, 1.17), (1.20, 1.30), (1.35, 1.77)]
    rng = random.Random(7)
    mid, amp = 86, 22
    t = 0.0
    while t <= 2.0:
        env = 0.0
        for a, b in words:
            if a <= t <= b:
                env = math.sin(math.pi * (t - a) / (b - a)) ** 0.7
        h = max(1.0, amp * env * (0.45 + 0.55 * rng.random()))
        s.line(X(t), mid - h, X(t), mid + h, theme["ink2"], width=1.6)
        t += 3 / 400.0

    s.text(X(0.45), 52, "“Utho Kikkar”", size=13, weight=600,
           anchor="middle")
    s.text(X(1.29), 52, "“turn on the lights”", size=13, weight=600,
           anchor="middle")

    t_end, t_det, t_cmd, t_rew = 0.80, 0.86, 0.81, 0.54
    for tx in (t_end, t_det):
        s.line(X(tx), 16, X(tx), 226, theme["hair"])
    s.text(X(t_end) - 5, 26, "wake word ends", size=11.5, fill=theme["ink2"],
           anchor="end")
    s.text(X(t_det) + 5, 26, "detector is sure", size=11.5,
           fill=theme["ink2"])

    s.text(60, 140, "Sending from the trigger", size=13, weight=600)
    s.rect(X(t_cmd), 150, X(t_det) - X(t_cmd) - 2, 16, theme["bad"], rx=3)
    s.rect(X(t_det) + 2, 150, X(2.0) - X(t_det) - 2, 16, theme["ink2"], rx=3)
    s.text(X(t_cmd) - 8, 162, "start of “turn” lost", size=12,
           fill=theme["ink2"], anchor="end")

    s.text(60, 196, "Kikkar: rewind 320 ms into the pre-roll, then stream live",
           size=13, weight=600)
    s.rect(X(t_rew), 206, X(t_det) - X(t_rew) - 2, 16, theme["ink2"], rx=3,
           opacity=0.45)
    s.rect(X(t_det) + 2, 206, X(2.0) - X(t_det) - 2, 16, theme["ink2"], rx=3)
    s.text((X(t_rew) + X(t_det)) / 2.0, 218, "from pre-roll", size=11,
           fill=theme["ink"], anchor="middle")
    s.text(X(t_rew) - 8, 218, "nothing lost", size=12, fill=theme["ink2"],
           anchor="end")

    s.line(X(0), 238, X(2.0), 238, theme["hair"])
    for k in range(5):
        tick = k * 0.5
        s.line(X(tick), 238, X(tick), 242, theme["hair"])
        s.text(X(tick), 256, ("%g s" % tick), size=11, fill=theme["ink2"],
               anchor="middle")

    s.save(os.path.join(OUT, "first-syllable-%s.svg" % mode))


def shared_arena(theme, mode):
    s = Svg(880, 274,
            "RAM sharing between the neural network and the network stack",
            "Without sharing, the neural network's RAM, the base RAM that keeps "
            "the connection alive and separate streaming buffers are all "
            "reserved the whole time, which costs 18 KB extra. With sharing, "
            "the base connection RAM stays, and once the keyword is detected "
            "the network reuses the neural network's RAM for its streaming "
            "buffers, saving 18 KB.", theme)
    a, b, c, d = 160.0, 390.0, 590.0, 720.0
    lane_h = 26
    neutral = theme["hair"]

    s.text(b, 14, "keyword detected", size=11.5, weight=600, fill=theme["ink"],
           anchor="middle")
    s.text(c, 14, "command ends", size=11.5, weight=600, fill=theme["ink"],
           anchor="middle")
    for p0, p1, name in ((a, b, "listening for the wake word"),
                         (b, c, "streaming your command"),
                         (c, d, "listening")):
        s.text((p0 + p1) / 2.0, 36, name, size=12, fill=theme["ink2"],
               anchor="middle")
    for px in (b, c):
        s.line(px, 20, px, 268, theme["grid"])

    def used(x0, x1, y, colour, text):
        s.rect(x0 + 1, y, x1 - x0 - 2, lane_h, colour, rx=4)
        if text:
            s.text((x0 + x1) / 2.0, y + 17.5, text, size=11.5, weight=600,
                   fill=label_ink(colour), anchor="middle")

    def idle(x0, x1, y, colour, text):
        s.raw('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="4" '
              'fill="none" stroke="%s" stroke-width="1.5"/>'
              % (x0 + 1.75, y + 0.75, x1 - x0 - 3.5, lane_h - 1.5, colour))
        if text:
            s.text((x0 + x1) / 2.0, y + 17.5, text, size=11.5,
                   fill=theme["ink2"], anchor="middle")

    def lane_label(y, text):
        s.text(0, y + 17.5, text, size=12.5, fill=theme["ink2"])

    y = 70
    s.text(0, y - 8, "Without RAM sharing", size=14, weight=600)
    lane_label(y, "Neural network")
    used(a, b, y, theme["model"], "detecting the wake word")
    idle(b, c, y, theme["model"], "reserved, doing nothing")
    used(c, d, y, theme["model"], "detecting")
    y += 32
    lane_label(y, "Link kept alive")
    used(a, d, y, neutral, "base RAM that keeps the connection open")
    y += 32
    lane_label(y, "Streaming buffers")
    idle(a, b, y, theme["net"], "reserved, doing nothing")
    used(b, c, y, theme["net"], "sending your audio")
    idle(c, d, y, theme["net"], "")
    s.text(740, y + 12, "+18 KB", size=20, weight=600)
    s.text(740, y + 29, "extra RAM", size=12, fill=theme["ink2"])

    y = 206
    s.text(0, y - 8, "With RAM sharing, as in Kikkar", size=14, weight=600)
    s.text((b + c) / 2.0, y - 8, "RAM handed to the network", size=11.5,
           fill=theme["ink2"], anchor="middle")
    lane_label(y, "Shared block")
    used(a, b, y, theme["model"], "neural network")
    used(b, c, y, theme["net"], "streaming buffers")
    used(c, d, y, theme["model"], "neural network")
    y += 32
    lane_label(y, "Link kept alive")
    used(a, d, y, neutral, "base RAM that keeps the connection open")
    s.text(740, 226, "18 KB", size=20, weight=600)
    s.text(740, 243, "saved", size=12, fill=theme["ink2"])

    s.save(os.path.join(OUT, "shared-arena-%s.svg" % mode))


def bloom_banner():
    src = os.path.join(ROOT, "Kikkar_UI_Assets", "out")
    stages = ["0.00", "0.50", "1.00"]
    crop = (120, 30, 680, 450)
    fw, fh = crop[2] - crop[0], crop[3] - crop[1]
    pad = 40
    width, height = pad * 2 + fw * len(stages), fh + 60
    canvas = Image.new("RGBA", (width, height), (38, 20, 19, 255))
    for k, stage in enumerate(stages):
        frame = Image.open(os.path.join(
            src, "reconstructed_bloom_%s.png" % stage)).convert("RGBA")
        frame = frame.crop(crop)
        canvas.alpha_composite(frame, (pad + k * fw, 30))

    mask = Image.new("L", canvas.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, width - 1, height - 1],
                                           radius=28, fill=255)
    canvas.putalpha(mask)
    canvas.save(os.path.join(OUT, "bloom-banner.png"), optimize=True)


def main():
    os.makedirs(OUT, exist_ok=True)
    for mode, theme in THEMES.items():
        metrics(theme, mode)
        pipeline(theme, mode)
        first_syllable(theme, mode)
        shared_arena(theme, mode)
    bloom_banner()
    for name in sorted(os.listdir(OUT)):
        print("  %-28s %7d B" % (name, os.path.getsize(os.path.join(OUT, name))))


if __name__ == "__main__":
    main()
