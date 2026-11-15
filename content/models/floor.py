from amb.shapes import slab

MATERIAL = "base.wood"
REPLICATED = ["base.ambient_lux"]
BINDINGS = {
    "zone": {
        "ambient": "base.ambient_lux",
        "priority": 10,
        "shape": {"kind": "box", "size": [16, 16, 6], "offset": [0, 0, 3]},
    }
}


def build(params):
    return slab(16, 16, 0.3)
