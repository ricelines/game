from build123d import Align, Box

MATERIAL = "base.steel"
PARAMS = {"length": (4.0, 0.5, 20), "height": (1.0, 0.8, 1.2)}


def build(params):
    length, height = params["length"], params["height"]
    count = int(length / 0.8) + 1
    solids = [Box(length, 0.07, 0.07).translate((0, 0, height - 0.035))]
    solids.append(Box(length, 0.04, 0.04).translate((0, 0, height * 0.48)))
    for index in range(count + 1):
        x = -length / 2 + index * length / count
        solids.append(
            Box(0.05, 0.05, height, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
                (x, 0, 0)
            )
        )
    return solids[0].fuse(*solids[1:])
