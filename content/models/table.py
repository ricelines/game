from build123d import Align, Box

MATERIAL = "base.wood"


def build(params):
    pieces = [
        Box(1.2, 0.75, 0.06, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate((0, 0, 0.72))
    ]
    for x in [-0.48, 0.48]:
        for y in [-0.27, 0.27]:
            pieces.append(
                Box(0.055, 0.055, 0.72, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
                    (x, y, 0)
                )
            )
    return pieces[0].fuse(*pieces[1:])
