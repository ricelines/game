from amb.shapes import cylinder

MATERIAL = "base.steel"
PARAMS = {"radius": (0.12, 0.05, 0.4), "height": (3.0, 1.0, 8.0)}


def build(params):
    return cylinder(params["radius"], params["height"])
