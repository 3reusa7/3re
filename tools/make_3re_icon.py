#!/usr/bin/env python3
"""Builds the 3re brand kit from one construction: the isometric "three views" cube (Front, Side and
Top faces drawn apart) whose Front face dissolves into pixels: its top-left corner steps down like a
pixel stair and three pixel squares break away. A picture / pixel drawing turning into a 3D solid.
No background: the symbol is the icon.

Writes
  branding/svg/   masters: symbol (dark surfaces), light-surface symbol, black, white, brand one-colour,
                  small-size cuts, app icon (= the light-surface symbol, transparent), lockups (outlined)
  branding/png/   renders for documents, slides and the web
  apps/gui/resources/  3re.png (256), 3re-512.png (splash), 3re-mark.png (symbol for the dark UI),
                  3re.ico (16-32 px use the small-size cut)

Palette = the slate-blue family of apps/gui/theme.cpp. Geometry is plain Python; PNG/ICO need
cairosvg, the lockups need fontTools and the Lato Semibold font (SIL OFL 1.1, outlined: no live text).
usage: make_3re_icon.py [--font PATH] [--no-raster]
"""
import argparse
import math
import os
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SVG_DIR = os.path.join(ROOT, "branding", "svg")
PNG_DIR = os.path.join(ROOT, "branding", "png")
RES_DIR = os.path.join(ROOT, "apps", "gui", "resources")
DEFAULT_FONT = r"C:\Windows\Fonts\LatoWeb-Semibold.ttf"

H = math.sqrt(3) / 2
# Faces are the UI accent ramp (accent_hover / accent_pressed) plus a lit tint for the top.
COLOUR = {"top": "#B9C7E9", "left": "#7C93C4", "right": "#4F6593"}
# Same ramp, deepened: the app icon (no background, so it must hold on dark and light taskbars)
# and every white / light-grey surface (faces >= 2.2 / 3.3 / 5.3 : 1 against #F4F5F7).
LIGHT = {"top": "#9CAFDB", "left": "#6E87BD", "right": "#4F6593"}
TEXT_ON_DARK, TEXT_ON_LIGHT = "#E4E7EC", "#1B1E24"


# ---- geometry --------------------------------------------------------------------------------
def vsub(a, b): return (a[0] - b[0], a[1] - b[1])
def vadd(a, b): return (a[0] + b[0], a[1] + b[1])
def vmul(a, s): return (a[0] * s, a[1] * s)
def unit(a): n = math.hypot(*a); return (a[0] / n, a[1] / n)
def cross(a, b): return a[0] * b[1] - a[1] * b[0]
def dot(a, b): return a[0] * b[0] + a[1] * b[1]


def num(x):
    s = f"{x:.2f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def inset_convex(poly, d):
    """Every edge of a convex polygon moved inward by d (uniform seams between faces)."""
    n = len(poly)
    cx, cy = sum(p[0] for p in poly) / n, sum(p[1] for p in poly) / n
    lines = []
    for i in range(n):
        p, q = poly[i], poly[(i + 1) % n]
        e = unit(vsub(q, p))
        nrm = (-e[1], e[0])
        if (cx - (p[0] + q[0]) / 2) * nrm[0] + (cy - (p[1] + q[1]) / 2) * nrm[1] < 0:
            nrm = (-nrm[0], -nrm[1])
        lines.append(((p[0] + nrm[0] * d, p[1] + nrm[1] * d), e))

    def meet(l1, l2):
        (p, r), (q, s) = l1, l2
        t = cross(vsub(q, p), s) / cross(r, s)
        return (p[0] + r[0] * t, p[1] + r[1] * t)

    return [meet(lines[i - 1], lines[i]) for i in range(n)]


def rounded_path(pts, radii):
    """Closed polygon with per-vertex corner radii (arcs; 0 = sharp)."""
    n = len(pts)
    if not isinstance(radii, (list, tuple)):
        radii = [radii] * n
    segs = []
    for i in range(n):
        p0, v, p1 = pts[i - 1], pts[i], pts[(i + 1) % n]
        u1, u2 = unit(vsub(p0, v)), unit(vsub(p1, v))
        ang = math.acos(max(-1.0, min(1.0, dot(u1, u2))))
        r = radii[i]
        if r <= 0 or ang > math.pi - 1e-6:
            segs.append((v, v, 0, 0))
            continue
        t = r / math.tan(ang / 2)
        sweep = 1 if cross(vsub(v, p0), vsub(p1, v)) > 0 else 0
        segs.append((vadd(v, vmul(u1, t)), vadd(v, vmul(u2, t)), r, sweep))
    d = [f"M{num(segs[0][1][0])} {num(segs[0][1][1])}"]
    for i in list(range(1, n)) + [0]:
        a, b, r, sw = segs[i]
        d.append(f"L{num(a[0])} {num(a[1])}")
        if r:
            d.append(f"A{num(r)} {num(r)} 0 0 {sw} {num(b[0])} {num(b[1])}")
    return "".join(d) + "Z"


def three_faces(R, g, rc, cx=128.0):
    """Top, left and right face polygons (edge length R, seam g), vertically centred on 128."""
    def build(cy):
        dx = H * R
        t, ur, lr = (cx, cy - R), (cx + dx, cy - R / 2), (cx + dx, cy + R / 2)
        b, ll, ul, c = (cx, cy + R), (cx - dx, cy + R / 2), (cx - dx, cy - R / 2), (cx, cy)
        raw = {"top": [t, ur, c, ul], "left": [ul, c, b, ll], "right": [c, ur, lr, b]}
        return {k: inset_convex(v, g / 2) for k, v in raw.items()}
    f0 = build(128.0)
    # extremes after rounding: the 120 degree top apex and the 60 degree bottom corners
    top_y = f0["top"][0][1] + rc * (1 / math.sin(math.radians(60)) - 1)
    bottom_y = f0["left"][2][1] - rc * (2 * math.cos(math.radians(30)) - 1)
    return build(128.0 + 128.0 - (top_y + bottom_y) / 2)


def mix(h, other, t):
    a = [int(h[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(other[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(a[i] + (b[i] - a[i]) * t):02X}" for i in range(3))


# ---- the mark --------------------------------------------------------------------------------
SYMBOL = dict(R=132.0, g=18.5, rc=13.9, seam=6.0)    # master: fills the canvas with ~6 % padding
REVERSED = dict(R=132.0, g=21.0, rc=13.9, seam=6.5)  # white on dark looks heavier (irradiation): thinned
SMALL = dict(R=132.0, g=22.0, rc=13.9, seam=8.0)     # 16-32 px: wider seams, one pixel square only
STEP, PIXEL_CORNER = 3, 2.0                          # the Front face is a 3 x 3 grid; crisp pixel corners


def mark_shapes(fills, R, g, rc, seam, small=False):
    """[(path, fill)]: Top and Right faces, the Front face with a stair-stepped top-left corner, and the
    three pixel squares that broke off it (only the corner one in the small-size cut)."""
    f = three_faces(R, g, rc)
    ul, c, b, ll = f["left"]
    P = lambda u, v: (ul[0] + (c[0] - ul[0]) * u + (ll[0] - ul[0]) * v, ul[1] + (c[1] - ul[1]) * u + (ll[1] - ul[1]) * v)
    k, rn = float(STEP), PIXEL_CORNER
    eu = (seam / 2) / abs(c[0] - ul[0])                                    # half a seam, in face units
    ev = (seam / 2) / (abs(ll[1] - ul[1]) * math.cos(math.radians(30)))
    stair = [(2 / k + eu, 0), (1, 0), (1, 1), (0, 1), (0, 2 / k + ev), (1 / k + eu, 2 / k + ev),
             (1 / k + eu, 1 / k + ev), (2 / k + eu, 1 / k + ev)]
    out = [(rounded_path(f["top"], rc), fills["top"]), (rounded_path(f["right"], rc), fills["right"]),
           (rounded_path([P(*p) for p in stair], [rn, rc, rc, rc, rn, rn, rn, rn]), fills["left"])]
    squares = [((0, 0, 1 / k - eu, 1 / k - ev), mix(fills["left"], "#FFFFFF", 0.34)),
               ((1 / k + eu, 0, 2 / k - eu, 1 / k - ev), mix(fills["left"], "#FFFFFF", 0.18)),
               ((0, 1 / k + ev, 1 / k - eu, 2 / k - ev), mix(fills["left"], "#FFFFFF", 0.18))]
    for (u0, v0, u1, v1), tone in squares[:1] if small else squares:
        # the corner square keeps no soft corner: pixels are crisp
        out.append((rounded_path([P(u0, v0), P(u1, v0), P(u1, v1), P(u0, v1)], rn), tone))
    return out


def solid(colour): return {k: colour for k in ("top", "left", "right")}


def path_svg(shapes, indent="    "):
    return "\n".join(f'{indent}<path fill="{f}" d="{d}"/>' for d, f in shapes)


def svg_doc(title, body, viewbox="0 0 256 256"):
    w, h = (float(v) for v in viewbox.split()[2:])
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{viewbox}" width="{num(w)}" height="{num(h)}" '
            f'role="img" aria-labelledby="t">\n  <title id="t">{title}</title>\n{body}\n</svg>\n')


def symbol_svg(name, fills, small=False, **geom):
    shapes = mark_shapes(fills, small=small, **geom)
    return svg_doc(name, f'  <g id="symbol">\n{path_svg(shapes)}\n  </g>')


# ---- lockups ---------------------------------------------------------------------------------
def wordmark(font_path, size):
    """('3re' outlined at font size `size`) -> (path data at origin baseline 0, bounds)."""
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.ttLib import TTFont
    font = TTFont(font_path)
    glyphs, cmap, k = font.getGlyphSet(), font.getBestCmap(), size / font["head"].unitsPerEm
    d, x, bounds = [], 0.0, BoundsPen(glyphs)
    for ch in "3re":
        g = glyphs[cmap[ord(ch)]]
        pen = SVGPathPen(glyphs, ntos=num)
        g.draw(TransformPen(pen, (k, 0, 0, -k, x, 0)))
        g.draw(TransformPen(bounds, (k, 0, 0, -k, x, 0)))
        d.append(pen.getCommands())
        x += g.width * k
    return "".join(d), bounds.bounds


def lockups(font_path):
    """Horizontal and stacked lockups, on dark and on light. Returns {file name: svg text}."""
    faces = three_faces(SYMBOL["R"], SYMBOL["g"], SYMBOL["rc"])
    xs = [p[0] for f in faces.values() for p in f]
    ys = [p[1] for f in faces.values() for p in f]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    sym_h = y1 - y0
    d, (wx0, wy0, wx1, wy1) = wordmark(font_path, 1000.0)
    k = (0.46 * sym_h) / -wy0                    # numeral height = 46 % of the symbol
    tw, th = (wx1 - wx0) * k, -wy0 * k
    out = {}
    for tone, text, palette in (("dark", TEXT_ON_DARK, COLOUR), ("light", TEXT_ON_LIGHT, LIGHT)):
        shapes = mark_shapes(palette, **SYMBOL)
        pad, gap = 20.0, 0.30 * sym_h
        w = pad + (x1 - x0) + gap + tw + pad
        tx, ty = pad + (x1 - x0) + gap - wx0 * k, 128 + th / 2
        body = (f'  <g id="symbol" transform="translate({num(pad - x0)} 0)">\n{path_svg(shapes)}\n  </g>\n'
                f'  <path id="wordmark" fill="{text}" transform="translate({num(tx)} {num(ty)}) scale({num(k)})" d="{d}"/>')
        out[f"3re-lockup-horizontal-{tone}.svg"] = svg_doc("3re logo, horizontal", body, f"0 0 {num(w)} 256")
        gap2, w2 = 0.16 * sym_h, max(x1 - x0, tw) + 2 * pad
        h2 = pad + sym_h + gap2 + th + pad
        sx, sy = (w2 - (x1 - x0)) / 2 - x0, pad - y0
        tx2, ty2 = (w2 - tw) / 2 - wx0 * k, pad + sym_h + gap2 + th
        body2 = (f'  <g id="symbol" transform="translate({num(sx)} {num(sy)})">\n{path_svg(shapes)}\n  </g>\n'
                 f'  <path id="wordmark" fill="{text}" transform="translate({num(tx2)} {num(ty2)}) scale({num(k)})" d="{d}"/>')
        out[f"3re-lockup-stacked-{tone}.svg"] = svg_doc("3re logo, stacked", body2, f"0 0 {num(w2)} {num(h2)}")
    return out


# ---- output ----------------------------------------------------------------------------------
def write(folder, name, text):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, name), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def write_ico(path, frames):
    """frames: [(size, png bytes)] -> .ico with PNG-compressed entries."""
    head = struct.pack("<HHH", 0, 1, len(frames))
    offset, entries, data = 6 + 16 * len(frames), b"", b""
    for size, png in frames:
        wh = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", wh, wh, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    with open(path, "wb") as fh:
        fh.write(head + entries + data)


def render(svg_text, width, height=None):
    import cairosvg
    return cairosvg.svg2png(bytestring=svg_text.encode("utf-8"), output_width=width, output_height=height or width)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", default=DEFAULT_FONT, help="Lato Semibold .ttf for the lockups")
    ap.add_argument("--no-raster", action="store_true", help="write the SVG masters only")
    args = ap.parse_args()

    svgs = {
        "3re-symbol.svg": symbol_svg("3re symbol", COLOUR, **SYMBOL),
        "3re-symbol-light.svg": symbol_svg("3re symbol, for light surfaces", LIGHT, **SYMBOL),
        "3re-symbol-black.svg": symbol_svg("3re symbol, black", solid("#000000"), **SYMBOL),
        "3re-symbol-white.svg": symbol_svg("3re symbol, white", solid("#FFFFFF"), **REVERSED),
        "3re-symbol-brand.svg": symbol_svg("3re symbol, brand one-colour", solid(COLOUR["right"]), **SYMBOL),
        "3re-symbol-small.svg": symbol_svg("3re symbol, small-size cut", COLOUR, small=True, **SMALL),
        "3re-symbol-small-light.svg": symbol_svg("3re symbol, small-size cut for light surfaces", LIGHT, small=True, **SMALL),
        # no tile: the app icon is the symbol itself, in the ramp that holds on dark and light taskbars
        "3re-app-icon.svg": symbol_svg("3re app icon", LIGHT, **SYMBOL),
        "3re-app-icon-small.svg": symbol_svg("3re app icon, small-size cut", LIGHT, small=True, **SMALL),
    }
    if os.path.exists(args.font):
        svgs.update(lockups(args.font))
    else:
        print(f"font not found ({args.font}): lockups skipped", file=sys.stderr)
    for name, text in svgs.items():
        write(SVG_DIR, name, text)
    print(f"{len(svgs)} SVG masters -> {SVG_DIR}")
    if args.no_raster:
        return 0
    try:
        import cairosvg  # noqa: F401
    except ImportError:
        print("cairosvg missing: PNG/ICO not written (pip install cairosvg)", file=sys.stderr)
        return 1

    icon, small = svgs["3re-app-icon.svg"], svgs["3re-app-icon-small.svg"]
    os.makedirs(RES_DIR, exist_ok=True)
    for name, data in (("3re.png", render(icon, 256)), ("3re-512.png", render(icon, 512)),
                       ("3re-mark.png", render(svgs["3re-symbol.svg"], 128))):
        with open(os.path.join(RES_DIR, name), "wb") as fh:
            fh.write(data)
    write_ico(os.path.join(RES_DIR, "3re.ico"), [(s, render(small if s <= 32 else icon, s)) for s in (16, 24, 32, 48, 64, 128, 256)])

    os.makedirs(PNG_DIR, exist_ok=True)
    png = {"3re-app-icon-1024.png": render(icon, 1024)}
    for key in ("symbol", "symbol-black", "symbol-white", "symbol-light"):
        png[f"3re-{key}-512.png"] = render(svgs[f"3re-{key}.svg"], 512)
    for name, text in svgs.items():
        if name.startswith("3re-lockup"):
            w = float(text.split('width="')[1].split('"')[0])
            h = float(text.split('height="')[1].split('"')[0])
            png[name[:-4] + ".png"] = render(text, 1200, round(1200 * h / w))
    for name, data in png.items():
        with open(os.path.join(PNG_DIR, name), "wb") as fh:
            fh.write(data)
    write_ico(os.path.join(PNG_DIR, "favicon.ico"), [(s, render(small, s)) for s in (16, 32, 48)])
    print(f"rasters -> {RES_DIR} and {PNG_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
