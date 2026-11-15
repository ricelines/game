from build123d import Align, Box, Matrix, Sphere

PARAMS = {"radius": (12.0, 4.0, 24.0), "height": (8.0, 4.0, 16.0), "wall": (0.06, 0.04, 0.2)}
MATERIAL = "base.glass"


def ellipsoid(radius, height):
    return Sphere(1).transform_geometry(
        Matrix([[radius, 0, 0, 0], [0, radius, 0, 0], [0, 0, height, 0], [0, 0, 0, 1]])
    )


def build(params):
    radius, height, wall = params["radius"], params["height"], params["wall"]
    upper = Box(
        2 * radius + 2, 2 * radius + 2, height + 1, align=(Align.CENTER, Align.CENTER, Align.MIN)
    )
    shell = (ellipsoid(radius, height) & upper) - ellipsoid(radius - wall, height - wall)
    doorway = Box(3, 5, 2.8, align=(Align.CENTER, Align.CENTER, Align.MIN)).translate(
        (0, -radius + 1.5, 0)
    )
    return shell - doorway
