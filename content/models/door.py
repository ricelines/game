from build123d import Align, Box, Compound

MATERIAL = "base.steel"
PARTS = {
    "leaf": {
        "solid": 1,
        "origin": [0, -0.25, 0],
        "material": "base.glass",
        "bindings": {"motion": {"mode": "kinematic"}},
    }
}
REPLICATED = ["base.open_fraction"]
BINDINGS = {"motion": {"axis": [1, 0, 0], "travel": 2.55, "speed": 1.2}}


def build(params):
    left = Box(0.12, 0.22, 2.6, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
        (-1.3, 0, 0)
    )
    right = left.translate((2.6, 0, 0))
    lintel = Box(2.72, 0.22, 0.12, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
        (0, 0, 2.48)
    )
    frame = left.fuse(right, lintel)
    # The leaf runs on the exterior track; its whole sweep clears the curved shell.
    leaf = Box(2.4, 0.08, 2.4, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
        (0, -0.25, 0)
    )
    return Compound(children=[frame, leaf])
