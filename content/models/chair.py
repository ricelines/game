from build123d import Align, Box

MATERIAL = "base.wood"
SITES = {"seat": {"position": [0, 0, 0.46], "rotation": [0, 0, 0, 1]}}


def build(params):
    pieces = [
        Box(0.5, 0.5, 0.055, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate((0, 0, 0.43))
    ]
    for x in [-0.19, 0.19]:
        for y in [-0.19, 0.19]:
            pieces.append(
                Box(0.045, 0.045, 0.43, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
                    (x, y, 0)
                )
            )
    pieces.append(
        Box(0.5, 0.055, 0.45, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
            (0, -0.22, 0.48)
        )
    )
    return pieces[0].fuse(*pieces[1:])
