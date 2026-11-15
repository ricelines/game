from build123d import Align, Box

MATERIAL = "base.wood"


def build(params):
    deck = Box(12, 8, 0.22, align=(Align.CENTER, Align.CENTER, Align.MIN))
    opening = Box(2.8, 3, 0.5, align=(Align.CENTER, Align.CENTER, Align.CENTER)).translate(
        (-4.8, -2.5, 0.11)
    )
    return deck - opening
