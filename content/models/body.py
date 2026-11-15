from build123d import Align, Box, Cylinder, Sphere

MATERIAL = "base.generic"
SITES = {"eye": {"position": [0, 0.08, 1.55]}, "grasp": {"position": [0, 0.35, 1.10]}}
REPLICATED = ["base.charge", "base.seated"]
BINDINGS = {
    "observer": {"eye": [0, 0.08, 1.55], "forward": [0, 1, 0], "dim_lux": "base.dim_lux"},
    "character": {
        "radius": 0.30,
        "height": 1.75,
        "mass": "base.mass",
        "max_push_force": 200,
        "max_speed": "base.walk_speed",
        "acceleration": 8,
        "max_step": 0.24,
        "max_slope": 0.7853981633974483,
        "floor_snap": 0.08,
    },
}


def block(width, depth, height, x, z):
    return Box(width, depth, height, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
        (x, 0, z)
    )


def build(params):
    pieces = [block(0.42, 0.25, 0.55, 0, 0.75)]
    for sign in [-1, 1]:
        pieces += [
            block(0.17, 0.30, 0.10, sign * 0.105, 0),
            block(0.15, 0.18, 0.72, sign * 0.105, 0.08),
            block(0.12, 0.17, 0.55, sign * 0.29, 0.76),
        ]
    pieces += [
        Cylinder(0.07, 0.12, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate((0, 0, 1.29)),
        Sphere(0.175).translate((0, 0, 1.575)),
    ]
    return pieces[0].fuse(*pieces[1:])
