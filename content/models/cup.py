from amb.shapes import hollow_cylinder

MATERIAL = "base.ceramic"
REPLICATED = ["base.liquid"]
BINDINGS = {"container": {"stored": "base.liquid"}}


def build(params):
    return hollow_cylinder(0.045, 0.10, 0.004, 0.007)
