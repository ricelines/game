from build123d import Align, Axis, Box, Cylinder

MATERIAL = "base.steel"
REPLICATED = ["base.on", "base.charge", "base.intensity"]
PARAMS = {"width": (0.4, 0.10, 1.2), "depth": (0.25, 0.1, 1), "height": (0.6, 0.1, 2)}


def build(params):
    width, depth, height = params["width"], params["depth"], params["height"]
    chassis = Box(width, depth, height, align=(Align.CENTER, Align.CENTER, Align.MIN))
    dial = (
        Cylinder(min(width, depth) * 0.14, 0.04)
        .rotate(Axis.X, 90)
        .translate((0, depth / 2, height * 0.7))
    )
    return chassis.fuse(dial)
