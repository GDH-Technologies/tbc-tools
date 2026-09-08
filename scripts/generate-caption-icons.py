"""Regenerate the closed-caption mimetype icons.

The SVGs and the PNG ladder under
src/ld-analyse/install/icons/hicolor/*/mimetypes/ are committed artwork; this
script is what produced them, kept so they stay editable rather than being
opaque binaries nobody can touch.

    python3 scripts/generate-caption-icons.py <output-dir>

Writes the three SVGs. Rasterise them to the committed sizes with any SVG
renderer -- they were rendered with QSvgRenderer at 32/48/64/128/256/512.

The sheet geometry and the wordmark-pill idiom are copied from the existing RF
sidecar icons so the caption icons read as part of the same family. Wordmark
letters are drawn as paths, never <text>: the shipped SVG has to render the
same on a machine with different fonts, or none.
"""
from pathlib import Path

# Sheet geometry lifted verbatim from application-x-rf-hifi-u8.svg so the
# caption icons sit in the same family.
SHEET = ('<path d="M34 8 H82 L100 26 V116 Q100 120 96 120 H34 Q30 120 30 116 '
         'V12 Q30 8 34 8 Z" fill="#f6f5f4" stroke="#c0bfbc" stroke-width="1.6"/>'
         '<path d="M82 8 L100 26 H86 Q82 26 82 22 Z" fill="#deddda" '
         'stroke="#c0bfbc" stroke-width="1.6" stroke-linejoin="round"/>')

# Blocky wordmark letters in a 7.8 x 12 box, drawn as paths (never <text>):
# the shipped SVG must render identically on a machine with no fonts.
BAR, STEM, GAP = 3.0, 3.3, 1.5

def _r(x, y, w, h):
    return f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" height="{h:.2f}"/>'

def letter(ch, x, y):
    """Return SVG for one glyph with its top-left at (x, y)."""
    W, H = 7.8, 12.0
    if ch == "S":
        return "".join([
            _r(x, y, W, BAR),                               # top bar
            _r(x, y + BAR, STEM, GAP),                      # upper-left stem
            _r(x, y + BAR + GAP, W, BAR),                   # middle bar
            _r(x + W - STEM, y + 2 * BAR + GAP, STEM, GAP), # lower-right stem
            _r(x, y + 2 * BAR + 2 * GAP, W, BAR),           # bottom bar
        ])
    if ch == "C":
        # Rounded, matching the C in the existing FLAC wordmark.
        return (f'<path d="M{x + W:.2f} {y:.2f} H{x + 2.2:.2f} '
                f'Q{x:.2f} {y:.2f} {x:.2f} {y + 2.2:.2f} V{y + H - 2.2:.2f} '
                f'Q{x:.2f} {y + H:.2f} {x + 2.2:.2f} {y + H:.2f} H{x + W:.2f} '
                f'V{y + H - 3.5:.2f} H{x + 3.5:.2f} V{y + 3.5:.2f} '
                f'H{x + W:.2f} Z"/>')
    if ch == "R":
        # The leg must be DIAGONAL. A vertical leg sits directly under the
        # bowl's right stem, making the whole right edge one unbroken column,
        # and the letter reads as an A at every size. Bars are also thinner
        # here than the shared rhythm so the counter is big enough to see.
        rb, rs = 2.6, 3.0
        return "".join([
            _r(x, y, rs, H),                        # stem
            _r(x, y, W, rb),                        # top bar
            _r(x + W - rs, y, rs, 8.0),             # bowl right
            _r(x, y + 5.4, W, rb),                  # bowl bottom
            (f'<path d="M{x + 3.0:.2f} {y + 7.6:.2f} '   # diagonal leg
             f'L{x + 5.4:.2f} {y + 7.6:.2f} '
             f'L{x + W:.2f} {y + H:.2f} '
             f'L{x + 4.6:.2f} {y + H:.2f} Z"/>'),
        ])
    if ch == "T":
        return "".join([
            _r(x, y, W, BAR),
            _r(x + (W - STEM) / 2, y + BAR, STEM, H - BAR),
        ])
    if ch == "V":
        return (f'<path d="M{x:.2f} {y:.2f} H{x + STEM:.2f} '
                f'L{x + W / 2:.2f} {y + H - 3.0:.2f} '
                f'L{x + W - STEM:.2f} {y:.2f} H{x + W:.2f} '
                f'L{x + W / 2 + 1.5:.2f} {y + H:.2f} '
                f'H{x + W / 2 - 1.5:.2f} Z"/>')
    raise ValueError(ch)

def wordmark(text, fill_pill, centre_x=65.0, top=96.5):
    pitch, lw = 9.2, 7.8
    span = (len(text) - 1) * pitch + lw
    x0 = centre_x - span / 2
    pad = 10.3
    pill = (f'<rect x="{x0 - pad:.2f}" y="{top - 4.5:.2f}" '
            f'width="{span + 2 * pad:.2f}" height="21" rx="10.5" '
            f'fill="{fill_pill}"/>')
    glyphs = "".join(letter(c, x0 + i * pitch, top) for i, c in enumerate(text))
    return pill + f'<g fill="#ffffff">{glyphs}</g>'

def _knockout_c(cx, cy, accent, r=10.0, ring=3.9, mouth=9.0):
    """One 'C' knocked out of the badge, drawn in painter's order.

    White disc, then an accent disc inside it to leave a ring, then an accent
    rectangle over the right side to open the ring into a C. No fill-rule or
    mask tricks, so it renders the same in every SVG implementation - the
    committed art has to survive whatever rasteriser a distro reaches for.
    """
    return (
        f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="#ffffff"/>'
        f'<circle cx="{cx}" cy="{cy}" r="{r - ring}" fill="{accent}"/>'
        f'<rect x="{cx + 1.5}" y="{cy - mouth / 2}" width="{r + 2}" '
        f'height="{mouth}" fill="{accent}"/>'
    )


def caption_glyph(accent):
    """The closed-caption badge: CC knocked out of a rounded box.

    NOT the Creative Commons roundel, which is a different organisation's mark
    and would read as a licence rather than as captions.
    """
    return (
        f'<rect x="36" y="43" width="58" height="35" rx="8" fill="{accent}"/>'
        + _knockout_c(54, 60.5, accent)
        + _knockout_c(76, 60.5, accent)
    )

ICONS = {
    "application-x-scenarist-cc": ("SCC", "#1c71d8", "#1a5fb4",
                                   "Scenarist closed captions"),
    "application-x-subrip":       ("SRT", "#2ec27e", "#26a269",
                                   "SubRip subtitles"),
    "text-vtt":                   ("VTT", "#9141ac", "#813d9c",
                                   "WebVTT subtitles"),
}

def svg(name):
    text, accent, pill, title = ICONS[name]
    return (
        '<?xml version="1.0"?>'
        '<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" '
        'viewBox="0 0 128 128">'
        f'<title>{title}</title>'
        f'{SHEET}{caption_glyph(accent)}{wordmark(text, pill)}'
        '</svg>'
    )

if __name__ == "__main__":
    import sys

    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    out.mkdir(parents=True, exist_ok=True)
    for name in ICONS:
        (out / f"{name}.svg").write_text(svg(name))
        print("wrote", name + ".svg")
