from amb.shapes import slab

MATERIAL = "base.concrete"
REPLICATED = ["base.ambient_lux"]
BINDINGS = {
    "zone": {
        "ambient": "base.ambient_lux",
        "priority": 0,
        "shape": {"kind": "box", "size": [128, 14, 30], "offset": [0, 0, 15]},
    }
}


def build(params):
    return slab(128, 14, 0.3)
