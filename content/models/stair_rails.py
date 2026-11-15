import math

from build123d import Align, Axis, Box, Compound

MATERIAL = "base.steel"


def build(params):
    run, rise, steps, height = 0.30, 0.16, 20, 1.0
    depth, ascent = run * steps, rise * steps
    length = math.hypot(depth, ascent)
    rails = []
    for side in (-1, 1):
        x = side * 1.25
        handrail = (
            Box(0.07, length, 0.07)
            .rotate(Axis.X, math.degrees(math.atan2(ascent, depth)))
            .translate((x, 0, height + ascent / 2))
        )
        posts = [
            Box(0.05, 0.05, height, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
                (x, -depth / 2 + index * run, index * rise)
            )
            for index in range(steps + 1)
        ]
        rails.append(handrail.fuse(*posts))
    return Compound(children=rails)
