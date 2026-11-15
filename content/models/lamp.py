from build123d import Align, Cylinder, Sphere

MATERIAL = "base.steel"
REPLICATED = ["base.on", "base.intensity"]
BINDINGS = {
    "light": {
        "intensity": "base.intensity",
        "site": [0, 0, 2.45],
        "range": 12,
        "source_radius_m": 0.08,
        "color": [1, 0.78, 0.47],
    },
    "render": [{"term": "base.intensity", "property": "emission", "scale": 0.003}],
}


def build(params):
    base = Cylinder(0.22, 0.08, align=(Align.CENTER, Align.CENTER, Align.MIN))
    pole = Cylinder(0.045, 2.4, align=(Align.CENTER, Align.CENTER, Align.MIN))
    shade = Sphere(0.22).translate((0, 0, 2.4))
    return base.fuse(pole, shade)
