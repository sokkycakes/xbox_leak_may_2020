# Sticker template for the SD card case label. Geometry mirrors label_cuts()
# in ../sd_card_case.scad (defaults). Writes SVG + PDF (1:1) and a 600 dpi PNG.
import math, sys, os
from reportlab.pdfgen import canvas
from reportlab.lib.units import mm
from reportlab.lib.pagesizes import letter, A4
from PIL import Image, ImageDraw, ImageFont

case_w, case_h, case_t = 72, 68, 3.2
margin, spine_h, label_r = 2.2, 31, 2.5
inset = 0.2          # sticker slightly smaller than the recess
bleed, safe = 1.0, 1.5

ax = margin + 8.9
lr = label_r + 1
top = case_h - margin
y0 = top - spine_h
right = case_w - margin - 0.6
bottom = margin + 0.8

# label coords: x from the spine's outer edge, y down from the label top
def L(x, y): return (x + case_t, top - y)
W = right + case_t
H = top - bottom

def outline(off):
    """Cut outline grown by off (negative = inside). Returns list of points (label coords, y down)."""
    i = inset - off
    r = max(lr - i, 0.3)
    pts = []
    def arc(cx, cy, a0, a1):
        for k in range(17):
            a = math.radians(a0 + (a1 - a0) * k / 16)
            pts.append(L(cx + r * math.cos(a), cy + r * math.sin(a)))
    pts.append(L(-case_t + i, y0 + i))
    pts.append(L(-case_t + i, top - i))
    arc(right - i - r, top - i - r, 90, 0)
    arc(right - i - r, bottom + i + r, 0, -90)
    arc(ax + i + r, bottom + i + r, 270, 180)
    pts.append(L(ax + i, y0 + i))
    return pts

cut, bl, sf = outline(0), outline(bleed), outline(-safe)
fold = [L(0, y0 + inset), L(0, top - inset)]
zones = [  # (label, x, y) in label coords
    ("EDGE", case_t / 2, (top - y0) / 2),
    ("SPINE", case_t + ax / 2, (top - y0) / 2),
    ("COVER ART", case_t + ax + (right - ax) / 2, H / 2),
]

def svg():
    def p(pts): return " ".join(f"{x:.3f},{y:.3f}" for x, y in pts)
    o = 2
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W+2*o}mm" height="{H+2*o}mm" viewBox="{-o} {-o} {W+2*o} {H+2*o}">',
         '<!-- 1:1 scale. Put artwork on the "art" layer covering the bleed (red). Cut on black. Fold on dashed blue. Keep text inside green. -->',
         '<g id="art"></g>',
         f'<polygon id="bleed" points="{p(bl)}" fill="none" stroke="#e0245e" stroke-width="0.15"/>',
         f'<polygon id="cut" points="{p(cut)}" fill="none" stroke="#000" stroke-width="0.2"/>',
         f'<polygon id="safe" points="{p(sf)}" fill="none" stroke="#2a9d4b" stroke-width="0.12" stroke-dasharray="0.8,0.6"/>',
         f'<line id="fold" x1="{fold[0][0]}" y1="{fold[0][1]}" x2="{fold[1][0]}" y2="{fold[1][1]}" stroke="#2563eb" stroke-width="0.2" stroke-dasharray="1.2,0.8"/>',
         f'<line id="fold-spine" x1="{case_t+ax}" y1="0" x2="{case_t+ax}" y2="{top-y0}" stroke="#999" stroke-width="0.1" stroke-dasharray="0.4,0.6"/>']
    for t, x, y in zones:
        s.append(f'<text x="{x}" y="{y}" font-family="sans-serif" font-size="{1.6 if t=="EDGE" else 2.4}" fill="#999" text-anchor="middle"'
                 + (f' transform="rotate(-90 {x} {y})"' if t != "COVER ART" else "") + f'>{t}</text>')
    s.append('</svg>')
    return "\n".join(s)

def pdf(path, pagesize, cols, rows):
    c = canvas.Canvas(path, pagesize=pagesize)
    pw, ph = pagesize
    gx, gy = 6 * mm, 6 * mm
    tw, th = cols * W * mm + (cols - 1) * gx, rows * H * mm + (rows - 1) * gy
    x0, y0p = (pw - tw) / 2, (ph + th) / 2
    c.setFont("Helvetica", 7)
    c.drawString(x0, ph - 0.45 * 72, "SD card case labels, print at 100% / Actual size (no fit-to-page). Black = cut, blue dashed = fold over the left edge.")
    c.drawString(x0, ph - 0.45 * 72 - 9, "Check: the 50 mm bar below must measure 50 mm.")
    c.line(x0, ph - 0.45 * 72 - 15, x0 + 50 * mm, ph - 0.45 * 72 - 15)
    for r in range(rows):
        for k in range(cols):
            ox, oy = x0 + k * (W * mm + gx), y0p - r * (H * mm + gy)
            T = lambda pt: (ox + pt[0] * mm, oy - pt[1] * mm)
            for pts, col, w, dash in ((cut, (0, 0, 0), 0.5, None), (sf, (0.16, 0.6, 0.3), 0.3, (2, 2))):
                c.setStrokeColorRGB(*col); c.setLineWidth(w)
                c.setDash(*dash) if dash else c.setDash()
                path = c.beginPath(); path.moveTo(*T(pts[0]))
                for q in pts[1:]: path.lineTo(*T(q))
                path.close(); c.drawPath(path)
            c.setDash(3, 2); c.setStrokeColorRGB(0.15, 0.39, 0.92); c.setLineWidth(0.5)
            c.line(*T(fold[0]), *T(fold[1])); c.setDash()
    c.showPage(); c.save()

def png(path, dpi=600):
    s = dpi / 25.4; o = 3
    im = Image.new("RGB", (round((W + 2 * o) * s), round((H + 2 * o) * s)), "white")
    d = ImageDraw.Draw(im)
    T = lambda pts: [((x + o) * s, (y + o) * s) for x, y in pts]
    d.polygon(T(bl), outline=(224, 36, 94), width=3)
    d.polygon(T(cut), outline=(0, 0, 0), width=5)
    d.polygon(T(sf), outline=(42, 157, 75), width=3)
    (fx0, fy0), (fx1, fy1) = T(fold)
    fy0, fy1 = sorted((fy0, fy1))
    for k in range(0, int(fy1 - fy0), 40): d.line([(fx0, fy0 + k), (fx0, min(fy0 + k + 24, fy1))], fill=(37, 99, 235), width=5)
    for t, x, y in zones:
        f = ImageFont.load_default(size=int((1.6 if t == "EDGE" else 2.4) * s))
        lab = Image.new("RGBA", (int(len(t) * 2.0 * s) + 10, int(3 * s)), (0, 0, 0, 0))
        ImageDraw.Draw(lab).text((0, 0), t, font=f, fill=(150, 150, 150, 255))
        lab = lab.crop(lab.getbbox())
        if t != "COVER ART": lab = lab.rotate(90, expand=True)
        cx, cy = (x + o) * s, (y + o) * s
        im.paste(lab, (int(cx - lab.width / 2), int(cy - lab.height / 2)), lab)
    im.save(path, dpi=(dpi, dpi))

out = os.path.dirname(os.path.abspath(__file__))
open(os.path.join(out, "label_template.svg"), "w").write(svg())
pdf(os.path.join(out, "label_sheet_letter.pdf"), letter, 2, 3)
pdf(os.path.join(out, "label_sheet_a4.pdf"), A4, 2, 3)
png(os.path.join(out, "label_template_600dpi.png"))
print(f"label {W:.1f} x {H:.1f} mm")
