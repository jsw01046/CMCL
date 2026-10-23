"""Generate the CMCL application icon.

The icon is a soft blue rounded tile (matching the WebView UI) with an
isometric Minecraft-style crafting table.  Requires Pillow.
"""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
S = 1024
K = S / 256.0


def p(x: float, y: float) -> tuple[float, float]:
    return (x * K, y * K)


def poly(points: list[tuple[float, float]], **kwargs) -> list[tuple[float, float]]:
    return [p(x, y) for x, y in points]


def lerp(a: str, b: str, t: float) -> tuple[int, int, int, int]:
    a = a.lstrip("#")
    b = b.lstrip("#")
    ar, ag, ab = int(a[0:2], 16), int(a[2:4], 16), int(a[4:6], 16)
    br, bg, bb = int(b[0:2], 16), int(b[2:4], 16), int(b[4:6], 16)
    return (
        round(ar + (br - ar) * t),
        round(ag + (bg - ag) * t),
        round(ab + (bb - ab) * t),
        255,
    )


def build_master() -> Image.Image:
    # Diagonal blue gradient, clipped to a rounded square.
    gradient = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(gradient)
    for i in range(2 * S):
        t = i / (2 * S - 1)
        gd.line([(i, 0), (0, i)], fill=lerp("#2F7CF6", "#59A6FF", t), width=2)

    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, S - 1, S - 1), radius=round(48 * K), fill=255
    )
    image = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    image.paste(gradient, (0, 0), mask)

    d = ImageDraw.Draw(image, "RGBA")
    d.rounded_rectangle(
        (3, 3, S - 4, S - 4),
        radius=round(46 * K),
        outline=(255, 255, 255, 55),
        width=round(2 * K),
    )

    # Soft shadow under the table.
    d.ellipse([p(40, 176), p(216, 206)], fill=(11, 63, 145, 55))

    # Isometric crafting-table body.
    top_back = (128, 46)
    top_right = (208, 86)
    top_front = (128, 126)
    top_left = (48, 86)
    depth = 68
    left_bottom = (48, 86 + depth)
    front_bottom = (128, 126 + depth)
    right_bottom = (208, 86 + depth)

    d.polygon(poly([top_left, top_front, front_bottom, left_bottom]), fill="#8C5A2E")
    d.polygon(poly([top_front, top_right, right_bottom, front_bottom]), fill="#6E4523")

    # Plank seams on both sides.
    for t in (0.34, 0.68):
        d.line(
            [p(48 + (128 - 48) * t, 86 + (126 - 86) * t),
             p(48 + (128 - 48) * t, 86 + (126 - 86) * t + depth)],
            fill=(88, 52, 25, 150), width=round(1.4 * K),
        )
        d.line(
            [p(128 + (208 - 128) * t, 126 + (86 - 126) * t),
             p(128 + (208 - 128) * t, 126 + (86 - 126) * t + depth)],
            fill=(74, 44, 21, 160), width=round(1.4 * K),
        )

    # Front edge and top rim highlights.
    d.line(poly([top_left, top_front, top_right]),
           fill="#D39A63", width=round(2.2 * K), joint="curve")
    d.line(poly([top_front, front_bottom]),
           fill="#4B2D17", width=round(2.4 * K))
    d.line(poly([top_right, right_bottom]),
           fill="#593619", width=round(2.0 * K))
    d.line(poly([top_left, left_bottom]),
           fill="#6B4222", width=round(2.0 * K))

    # Top face base and 3x3 crafting grid.
    d.polygon(poly([top_back, top_right, top_front, top_left]), fill="#5A371C")

    u = ((top_right[0] - top_back[0]) / 3.0,
         (top_right[1] - top_back[1]) / 3.0)
    v = ((top_left[0] - top_back[0]) / 3.0,
         (top_left[1] - top_back[1]) / 3.0)
    palette = [
        ["#D9A066", "#C98B53", "#D6A36D"],
        ["#C48249", "#D9A066", "#BF7C45"],
        ["#D29A62", "#BE7C46", "#CF9259"],
    ]

    for i in range(3):
        for j in range(3):
            corners = []
            for di, dj in ((0, 0), (1, 0), (1, 1), (0, 1)):
                ci = i + di
                cj = j + dj
                corners.append((
                    top_back[0] + u[0] * ci + v[0] * cj,
                    top_back[1] + u[1] * ci + v[1] * cj,
                ))
            cx = sum(x for x, _ in corners) / 4.0
            cy = sum(y for _, y in corners) / 4.0
            inset = 0.10
            corners = [
                (x + (cx - x) * inset, y + (cy - y) * inset)
                for x, y in corners
            ]
            d.polygon(poly(corners), fill=palette[i][j])

    # Small warm highlight on the near edge of the top.
    d.line(poly([top_left, top_front]), fill=(255, 226, 184, 90),
           width=round(1.2 * K))

    return image


def main() -> None:
    master = build_master()

    web = ROOT / "web"
    ui = ROOT / "src" / "ui"
    web.mkdir(parents=True, exist_ok=True)
    ui.mkdir(parents=True, exist_ok=True)

    master.resize((512, 512), Image.Resampling.LANCZOS).save(web / "icon.png")
    master.resize((64, 64), Image.Resampling.LANCZOS).save(web / "favicon.png")
    master.save(
        ui / "app.ico",
        format="ICO",
        sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40),
               (48, 48), (64, 64), (128, 128), (256, 256)],
    )
    print("wrote web/icon.png, web/favicon.png and src/ui/app.ico")


if __name__ == "__main__":
    main()
