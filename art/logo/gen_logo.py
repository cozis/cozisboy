#!/usr/bin/env python3
"""Generate the "cozisboy" pixel-art logo as SVG.

Usage:
  gen_logo.py [--icon mug|cat|star|none] [--style NAME]
  gen_logo.py --compare-icons      # compare.svg: every icon, current style
  gen_logo.py --compare-styles     # styles.svg: every style, wordmark only

Outputs (next to this script):
  cozisboy.svg         wordmark + icon, transparent background
  cozisboy-splash.svg  640x480 boot splash (render to PNG with rsvg-convert)
"""
import argparse
import os

# Glyphs on a grid: rows 0-1 ascender, 2-6 x-height, 7-8 descender.
GLYPHS = {
    "c": ["", "", ".####", "##...", "##...", "##...", ".####"],
    "o": ["", "", ".###.", "##.##", "##.##", "##.##", ".###."],
    "z": ["", "", "#####", "...##", "..##.", ".##..", "#####"],
    "i": ["##", "", "##", "##", "##", "##", "##"],
    "s": ["", "", ".####", "##...", ".###.", "...##", "####."],
    "b": ["##...", "##...", "####.", "##.##", "##.##", "##.##", "####."],
    "y": ["", "", "##.##", "##.##", "##.##", ".####", "...##", "...##", "####."],
}
WORD = "cozisboy"
ROWS = 9
ITALIC = 0.3  # cells of horizontal shift per row for italic styles

# Icons. Each row is a string of colour codes (see ICON_COLOURS); "." is empty.
# Codes in NO_OUTLINE (e.g. steam) skip the outline and 3D extrusion.
ICONS = {
    "mug": [
        "..w...w...w...",
        "...w...w...w..",
        "...w...w...w..",
        "..w...w...w...",
        "..............",
        "SSSSSSSSSSS...",
        "BBBBBBBBBBbbb.",
        "BBBBBBBBBBb.bb",
        "BBBRRBRRBBb.bb",
        "BBBRRRRRBBb.bb",
        "BBBBRRRBBBbbb.",
        "BBBBBRBBBBb...",
        "BBBBBBBBBBb...",
        ".bbbbbbbbb....",
    ],
    "cat": [
        "BB.......BB",
        "BPB.....BPB",
        "BPPBBBBBPPB",
        "BBBBBBBBBBB",
        "BBBBBBBBBBB",
        "BBDDBBBDDBB",
        "BBDWBBBDWBB",
        "BBBBBRBBBBB",
        "RRBBDBDBBRR",
        "BBBBBBBBBBB",
        ".BBBBBBBBB.",
        "..bbbbbbb..",
    ],
    "star": [
        "......S......",
        "......S......",
        ".....SSS.....",
        ".....SSS.....",
        "....SSSSS....",
        "..SSSSSSSSS..",
        "SSSSSSWSSSSSS",
        "..SSSSSSSSS..",
        "....SSSSS....",
        ".....SSS.....",
        ".....SSS.....",
        "......S......",
        "......S......",
    ],
    "none": None,
}
ICON_COLOURS = {
    "B": "#d9d3f0", "b": "#a79fd0", "P": "#ff8fb0", "R": "#ff4f7b",
    "D": "#2b2240", "W": "#fff6d8", "w": "#e8e2ff", "S": "url(#icongrad)",
}
NO_OUTLINE = "w"

# Style treatments for the wordmark.
#   grad      vertical gradient stops for the letter fill
#   letters   flat per-letter colours instead of grad
#   outline   colour of the pixel outline; outline2 an extra outer ring
#   extrude   3D depth colours, front to back; dir its direction (dx, dy)
#   italic    shear rows into a pixel italic
#   hollow    letters drawn as outline only; echo = offset ghost copy
#   glow      soft blur behind the letters
#   highlight top-edge shine; scan = scanline colour
STYLES = {
    "sunset": dict(
        bg=("#140b26", "#2a0f33"),
        grad=[(0, "#ffe27a"), (.45, "#ffab2e"), (.8, "#ff6a2b"), (1, "#e8366a")],
        outline="#1b1030", extrude=["#5a1e4a", "#45173d", "#331230"],
        highlight="#fff3c4", scan="#7a1f3d"),
    "candy": dict(
        bg=("#fff1f7", "#f3e4ff"),
        grad=[(0, "#ffe3f1"), (.5, "#ff9fd0"), (1, "#b98cff")],
        outline="#ffffff", outline2="#5b2f8f",
        extrude=["#7c4fc0", "#6d43ad", "#5e399b", "#50308a"],
        highlight="#ffffff"),
    "neon": dict(
        bg=("#05040c", "#110822"), hollow=True, outline="#3ff3ff",
        echo="#ff3fd2", glow="#3ff3ff"),
    "chrome": dict(
        bg=("#090a2a", "#2b0d3d"),
        grad=[(0, "#ffffff"), (.48, "#8cc8ff"), (.5, "#ffffff"),
              (.53, "#d08a3a"), (1, "#fff0b8")],
        outline="#0a0a3a",
        extrude=["#3a5bd8", "#2d4bbd", "#223b9c", "#182d7d"],
        italic=True, highlight="#ffffff"),
    "mint": dict(
        bg=("#0d1b2a", "#13283a"),
        grad=[(0, "#e9fff4"), (.5, "#7af0c4"), (1, "#22b8a8")],
        outline="#0b1622",
        extrude=["#1f5f7a", "#1b526b", "#17465c", "#133a4d", "#0f2f3f"],
        dir=(-1, 1), italic=True, highlight="#ffffff", scan="#0b4a4a"),
    "arcade": dict(
        bg=("#111111", "#1d1d1d"),
        letters=["#ff4a4a", "#ff9a2e", "#ffd93d", "#5ee05e",
                 "#3fc8ff", "#6a7dff", "#c56bff", "#ff6bc1"],
        outline="#ffffff", outline2="#000000",
        extrude=["#000000", "#000000"], dir=(1, 1)),
}


def word_cells(style):
    """Return ([(x, y, letter_index, unsheared_x)], width_in_cells)."""
    cells, x = [], 0
    for li, ch in enumerate(WORD):
        rows = GLYPHS[ch]
        w = max(len(r) for r in rows)
        for y, row in enumerate(rows):
            for dx, c in enumerate(row):
                if c == "#":
                    cells.append((x + dx, y, li))
        x += w + 1
    width = x - 1
    if style.get("italic"):
        # shear by a fraction of a cell per row so letter shapes survive
        cells = [(cx + (ROWS - 1 - y) * ITALIC, y, li, cx) for cx, y, li in cells]
        width += (ROWS - 1) * ITALIC
    else:
        cells = [(cx, y, li, cx) for cx, y, li in cells]
    return cells, width


def rects(cells, px, ox, oy, fill=None, grow=0, extra=""):
    f = f' fill="{fill}"' if fill else ""
    return "".join(
        f'<rect x="{ox + c[0] * px - grow}" y="{oy + c[1] * px - grow}" '
        f'width="{px + 2 * grow}" height="{px + 2 * grow}"{f}{extra}/>'
        for c in cells)


def draw_icon(rows, ix, iy, px):
    out = []
    icon_cells = {(x, y): c for y, row in enumerate(rows)
                  for x, c in enumerate(row) if c != "."}
    solid = [xy for xy, c in icon_cells.items() if c not in NO_OUTLINE]
    for k, col in reversed(list(enumerate(["#5a1e4a", "#45173d", "#331230"], 1))):
        out.append(rects(solid, px, ix + k * px // 4, iy + k * px // 4, col))
    out.append(rects(solid, px, ix, iy, "#1b1030", grow=px // 3))
    for (x, y), c in icon_cells.items():
        op = ' opacity="0.7"' if c in NO_OUTLINE else ""
        out.append(rects([(x, y)], px, ix, iy, ICON_COLOURS[c], extra=op))
    return "".join(out)


def logo_group(ox, oy, px, icon="none", style_name="sunset", uid=""):
    """Return (defs, body, width, height) for icon + wordmark at origin."""
    st = STYLES[style_name]
    rows = ICONS[icon]
    out = []
    word_h = ROWS * px
    ix, iy = ox, oy
    if rows:
        icon_h = len(rows) * px
        icon_w = max(len(r) for r in rows) * px + 2 * px
        out.append(draw_icon(rows, ix, iy, px))
    else:
        icon_h, icon_w = word_h, 0

    cells, wcells = word_cells(st)
    xy = [(x, y) for x, y, _, _ in cells]
    wx = ix + icon_w
    wy = oy + (icon_h - word_h) // 2 + px // 2
    g = px // 3  # outline thickness

    if st.get("glow"):
        out.append(f'<g filter="url(#glow{uid})" opacity="0.8">'
                   f'{rects(xy, px, wx, wy, st["glow"], grow=g)}</g>')
    if st.get("echo"):
        out.append(f'<g opacity="0.85">{rects(xy, px, wx + px // 2, wy + px // 2, st["echo"], grow=g)}'
                   f'{rects(xy, px, wx + px // 2, wy + px // 2, st["bg"][0])}</g>')

    dx, dy = st.get("dir", (1, 1))
    step = px // 4
    for k, col in reversed(list(enumerate(st.get("extrude", []), 1))):
        out.append(rects(xy, px, wx + dx * k * step, wy + dy * k * step, col, grow=g))
    if st.get("outline2"):
        out.append(rects(xy, px, wx, wy, st["outline2"], grow=g + px // 4))
    out.append(rects(xy, px, wx, wy, st["outline"], grow=g))

    if st.get("hollow"):
        out.append(rects(xy, px, wx, wy, st["bg"][0]))
    elif st.get("letters"):
        cols = st["letters"]
        out.append("".join(rects([(x, y)], px, wx, wy, cols[li]) for x, y, li, _ in cells))
    else:
        out.append(f'<g fill="url(#amber{uid})">{rects(xy, px, wx, wy)}</g>')

    if st.get("scan"):
        out.append(f'<g clip-path="url(#wordclip{uid})" fill="{st["scan"]}" opacity="0.18">'
                   + "".join(f'<rect x="{wx}" y="{wy + i}" width="{wcells * px}" '
                             f'height="{max(1, px // 8)}"/>'
                             for i in range(0, word_h, max(2, px // 4)))
                   + "</g>")
    if st.get("highlight"):
        orig = {(ux, y) for _, y, _, ux in cells}
        hl = [(x, y) for x, y, _, ux in cells if (ux, y - 1) not in orig]
        out.append(f'<g fill="{st["highlight"]}" opacity="0.9">' + "".join(
            f'<rect x="{wx + x * px}" y="{wy + y * px}" width="{px}" '
            f'height="{max(2, px // 5)}"/>' for x, y in hl) + "</g>")

    grad = st.get("grad", [(0, "#fff"), (1, "#fff")])
    defs = (
        f'<linearGradient id="amber{uid}" gradientUnits="userSpaceOnUse" '
        f'x1="0" y1="{wy}" x2="0" y2="{wy + word_h}">'
        + "".join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in grad)
        + "</linearGradient>"
        f'<linearGradient id="icongrad{uid}" gradientUnits="userSpaceOnUse" '
        f'x1="0" y1="{iy}" x2="0" y2="{iy + icon_h}">'
        '<stop offset="0" stop-color="#ffe27a"/>'
        '<stop offset="1" stop-color="#ff6a2b"/></linearGradient>'
        f'<clipPath id="wordclip{uid}">{rects(xy, px, wx, wy)}</clipPath>'
        f'<filter id="glow{uid}" x="-20%" y="-50%" width="140%" height="200%">'
        f'<feGaussianBlur stdDeviation="{px * 0.8}"/></filter>'
    )
    body = "".join(out).replace("url(#icongrad)", f"url(#icongrad{uid})")
    extra = max(st.get("dir", (1, 1))[1], 0) * len(st.get("extrude", [])) * step
    width = wx - ox + wcells * px + px + len(st.get("extrude", [])) * step
    height = max(icon_h, wy - oy + word_h + extra) + px
    return defs, body, width, height


def write(path, svg):
    with open(path, "w") as f:
        f.write(svg)
    print("wrote", path)


def svg(W, H, inner):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
            f'viewBox="0 0 {W} {H}" shape-rendering="crispEdges">{inner}</svg>\n')


def compare(here, fname, items):
    """Stack several (label, icon, style) variants, each on its own background."""
    px, pad, parts, y, W = 8, 28, [], 0, 0
    blocks = []
    for i, (label, icon, style) in enumerate(items):
        defs, body, w, h = logo_group(pad + px, 0, px, icon, style, uid=str(i))
        blocks.append((label, style, defs, body, w, h))
        W = max(W, w + 2 * pad + 2 * px)
    for label, style, defs, body, w, h in blocks:
        bh = h + 2 * pad
        b0, b1 = STYLES[style]["bg"]
        parts.append(
            f'<g transform="translate(0 {y})"><defs>{defs}'
            f'<linearGradient id="bg{label}" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="{b0}"/><stop offset="1" stop-color="{b1}"/>'
            f'</linearGradient></defs>'
            f'<rect width="{W}" height="{bh}" fill="url(#bg{label})"/>'
            f'<g transform="translate(0 {pad})">{body}</g>'
            f'<text x="{W - 10}" y="{bh - 10}" text-anchor="end" fill="#888" '
            f'font-family="monospace" font-size="14">{label}</text></g>')
        y += bh
    write(os.path.join(here, fname), svg(W, y, "".join(parts)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--icon", default="none", choices=ICONS)
    ap.add_argument("--style", default="arcade", choices=STYLES)
    ap.add_argument("--compare-icons", action="store_true")
    ap.add_argument("--compare-styles", action="store_true")
    a = ap.parse_args()

    if a.compare_icons:
        return compare(here, "compare.svg",
                       [(n, n, a.style) for n in ICONS])
    if a.compare_styles:
        return compare(here, "styles.svg",
                       [(s, a.icon, s) for s in STYLES])

    st = STYLES[a.style]

    # Transparent logo
    px, pad = 16, 24
    defs, body, w, h = logo_group(pad, pad, px, a.icon, a.style)
    W, H = w + 2 * pad, h + 2 * pad
    write(os.path.join(here, "cozisboy.svg"), svg(W, H, f"<defs>{defs}</defs>{body}"))

    # 640x480 splash
    SW, SH, px = 640, 480, 8
    _, _, w, h = logo_group(0, 0, px, a.icon, a.style)
    ox, oy = (SW - w) // 2, (SH - h) // 2 - 16
    defs, body, w, h = logo_group(ox, oy, px, a.icon, a.style)
    b0, b1 = st["bg"]
    stars = "".join(
        f'<rect x="{(i * 197 + 31) % SW}" y="{(i * 113 + 17) % SH}" '
        f'width="{2 + i % 2 * 2}" height="{2 + i % 2 * 2}" fill="#fff" '
        f'opacity="{0.25 + (i % 3) * 0.2:.2f}"/>' for i in range(40))
    dark = int(b0[1:3], 16) < 0x80
    write(os.path.join(here, "cozisboy-splash.svg"), svg(SW, SH,
          f'<defs>{defs}<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
          f'<stop offset="0" stop-color="{b0}"/><stop offset="1" stop-color="{b1}"/>'
          f'</linearGradient></defs>'
          f'<rect width="{SW}" height="{SH}" fill="url(#bg)"/>{stars if dark else ""}{body}'
          f'<rect x="{SW // 2 - 60}" y="{oy + h + 24}" width="120" height="4" '
          f'fill="{st.get("grad", [(0, st["outline"])])[-1][1]}" opacity="0.6"/>'))


if __name__ == "__main__":
    main()
