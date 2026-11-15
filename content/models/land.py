from build123d import Align, Box

PARAMS = {"cafe_cut": (1.0, 0.0, 1.0)}
MATERIAL = "base.stone"
BINDINGS = {
    "zone": {
        "shape": {"kind": "box", "size": [128, 50, 30], "offset": [0, 0, 15.3]},
        "priority": 0,
        "ambient": "base.ambient_lux",
    }
}
REPLICATED = ["base.ambient_lux"]


def build(params):
    ground = Box(128, 50, 0.3, align=(Align.CENTER, Align.CENTER, Align.MIN))
    if params["cafe_cut"] > 0.5:
        # Ground is centered at world Y=25; the real floor fills this exact opening.
        opening = Box(16, 16, 1).translate((0, -15, 0.15))
        ground = ground - opening
    return ground
