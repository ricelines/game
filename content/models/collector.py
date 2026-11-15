from build123d import Align, Box

MATERIAL = "base.steel"
PARAMS = {"width": (5.0, 0.5, 12), "depth": (4.0, 0.5, 12)}
SITES = {"terminal": {"position": [0, 0, 0.1]}}


def build(params):
    width, depth = params["width"], params["depth"]
    pieces = [
        Box(width, depth, 0.08, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
            (0, 0, 0.35)
        )
    ]
    for x in [-width * 0.4, width * 0.4]:
        for y in [-depth * 0.4, depth * 0.4]:
            pieces.append(
                Box(0.06, 0.06, 0.35, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
                    (x, y, 0)
                )
            )
    return pieces[0].fuse(*pieces[1:])
