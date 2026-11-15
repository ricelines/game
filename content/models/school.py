from math import cos, pi, sin

from build123d import Align, Box, Compound, Face, Shell, Solid, Wire

MATERIAL = "base.steel"
PARAMS = {"count": (300, 1, 300)}
PARTS = {}
REPLICATED = ["aq.health"]


def fish_shape():
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


def build(params):
    count = int(params["count"])
    if count != params["count"]:
        raise ValueError("Fish count must be a whole number")
    fish = fish_shape()
    solids = [Box(0.4, 0.25, 0.6, align=(Align.CENTER, Align.CENTER, Align.MIN))]
    PARTS.clear()
    for index in range(count):
        origin = [
            0.75 + (index % 15) * 0.24,
            -1.25 + ((index // 15) % 8) * 0.24,
            1.2 + (index // 120) * 0.35,
        ]
        name = f"fish_{index:03}"
        PARTS[name] = {
            "solid": index + 1,
            "origin": origin,
            "material": "base.living",
            "replicated": ["aq.health", "aq.alive"],
            "bindings": {
                "motion": {"mode": "kinematic"},
                "physics": {"collision_solid": False},
                "face_motion": True,
                "render": [
                    {
                        "term": "aq.health",
                        "property": "color",
                        "range": [0, 1],
                        "colors": [[0.15, 0.18, 0.22, 1], [1, 0.73, 0.28, 1]],
                    }
                ],
            },
        }
        solids.append(fish.translate(tuple(origin)))
    return Compound(children=solids)
