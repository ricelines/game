from amb.shapes import stair

MATERIAL = "base.wood"
PARAMS = {
    "width": (2.4, 0.8, 4.0),
    "rise": (0.16, 0.12, 0.20),
    "run": (0.30, 0.25, 0.5),
    "steps": (20, 1, 40),
}


def build(params):
    return stair(params["width"], params["rise"], params["run"], params["steps"])
