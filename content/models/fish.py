from math import cos, pi, sin

from build123d import Face, Shell, Solid, Wire

MATERIAL = "base.living"
REPLICATED = ["aq.health", "aq.alive"]
BINDINGS = {
    "face_motion": True,
    "physics": {"collision_solid": False},
    "render": [
        {
            "term": "aq.health",
            "property": "color",
            "range": [0, 1],
            "colors": [[0.15, 0.18, 0.22, 1], [1.0, 0.73, 0.28, 1]],
        }
    ],
}


def build(params):
    sections = []
    for y, width in [(-0.08, 0.12), (-0.04, 0.8), (0.015, 1), (0.07, 0.65), (0.105, 0.06)]:
        sections.append(
            [
                (0.035 * width * cos(i * pi / 4), y, 0.045 + 0.045 * width * sin(i * pi / 4))
                for i in range(8)
            ]
        )
    faces = [Face(Wire.make_polygon(sections[0])), Face(Wire.make_polygon(sections[-1]))]
    for left, right in zip(sections, sections[1:], strict=False):
        for index in range(8):
            following = (index + 1) % 8
            faces.append(
                Face(
                    Wire.make_polygon(
                        [left[index], left[following], right[following], right[index]]
                    )
                )
            )
    body = Solid(Shell(faces))
    tail = Solid.extrude(
        Face(
            Wire.make_polygon(
                [(-0.004, -0.115, 0.005), (-0.004, -0.067, 0.045), (-0.004, -0.115, 0.085)]
            )
        ),
        (0.008, 0, 0),
    )
    return body.fuse(tail)
