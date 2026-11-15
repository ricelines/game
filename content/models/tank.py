from amb.shapes import hollow_box

MATERIAL = "base.glass"
PARAMS = {"width": (4.0, 1.0, 8.0), "depth": (2.0, 0.5, 4.0), "height": (2.0, 0.5, 4.0)}
REPLICATED = ["aq.oxygen", "base.liquid"]
BINDINGS = {"container": {}}


def build(params):
    width, depth, height = params["width"], params["depth"], params["height"]
    inner_height = height - 0.03
    BINDINGS["container"]["shape"] = {
        "kind": "box",
        "size": [width - 0.04, depth - 0.04, inner_height],
        "offset": [0, 0, 0.03 + inner_height / 2],
    }
    return hollow_box(params["width"], params["depth"], params["height"], 0.02, 0.03)
