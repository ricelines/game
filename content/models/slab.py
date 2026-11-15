from amb.shapes import slab

PARAMS = {"width": (1.0, 0.02, 256), "depth": (1.0, 0.02, 256), "thickness": (0.2, 0.01, 10)}
MATERIAL = "base.stone"


def build(params):
    return slab(params["width"], params["depth"], params["thickness"])
