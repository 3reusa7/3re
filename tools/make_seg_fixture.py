#!/usr/bin/env python3
"""Synthetic segmentation fixture (tests/fixtures/S_segmentation): a shaded, painted-looking
object (ball on a pedestal) over a soft, noisy studio background, with its exact mask.
No third-party artwork; deterministic. Needs Pillow.

Outputs: subject.png (RGB, what SAM sees), ball_mask.png / pedestal_mask.png /
subject_mask.png (L, 255 = inside), clicks.txt (one click per part, used by the IoU test).
usage: make_seg_fixture.py OUT_DIR
"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

W, H = 640, 480


def main(out):
    os.makedirs(out, exist_ok=True)
    rnd = random.Random(20261020)
    bg = Image.new("RGB", (W, H))
    px = bg.load()
    for y in range(H):
        for x in range(W):
            t = y / H
            base = (int(205 - 60 * t), int(198 - 55 * t), int(186 - 45 * t))
            n = rnd.randint(-6, 6)
            px[x, y] = tuple(max(0, min(255, c + n)) for c in base)
    bg = bg.filter(ImageFilter.GaussianBlur(1.2))

    cx, cy, r = 320, 200, 120
    ball = Image.new("L", (W, H), 0)
    ImageDraw.Draw(ball).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    pedestal = Image.new("L", (W, H), 0)
    ImageDraw.Draw(pedestal).polygon([(260, 300), (380, 300), (410, 420), (230, 420)], fill=255)
    pedestal.paste(0, (0, 0), ball)  # the ball is in front
    mask = Image.new("L", (W, H), 0)
    mask.paste(255, (0, 0), ball)
    mask.paste(255, (0, 0), pedestal)

    obj = Image.new("RGB", (W, H))
    op = obj.load()
    lx, ly, lz = -0.45, -0.6, 0.66
    for y in range(H):
        for x in range(W):
            dx, dy = (x - cx) / r, (y - cy) / r
            if dx * dx + dy * dy <= 1.0:
                dz = math.sqrt(max(0.0, 1 - dx * dx - dy * dy))
                d = max(0.0, dx * lx + dy * ly + dz * lz)
                spec = max(0.0, d) ** 24
                c = (int(40 + 150 * d + 60 * spec), int(70 + 110 * d + 60 * spec), int(150 + 90 * d + 50 * spec))
            else:
                t = (y - 300) / 120.0
                c = (int(120 - 40 * t), int(80 - 25 * t), int(50 - 15 * t))
            op[x, y] = tuple(max(0, min(255, v)) for v in c)
    img = bg.copy()
    img.paste(obj, (0, 0), mask)
    img.save(os.path.join(out, "subject.png"))
    mask.save(os.path.join(out, "subject_mask.png"))
    ball.save(os.path.join(out, "ball_mask.png"))
    pedestal.save(os.path.join(out, "pedestal_mask.png"))
    with open(os.path.join(out, "clicks.txt"), "w") as f:
        f.write("ball 300 180\npedestal 320 390\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/S_segmentation")
