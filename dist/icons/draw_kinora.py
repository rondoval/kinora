#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Draw Kinora's icon art, Kinora.png (48 x 40, RGBA): a strip of film with a play triangle.

The PNG is committed source art; run this only to change the drawing (needs Pillow).
It is drawn at eight times the size and scaled down, which smooths the edges.

Usage:   python3 draw_kinora.py
"""
import os
from PIL import Image, ImageDraw

WIDTH, HEIGHT, SCALE = 48, 40, 8
FILM = (38, 40, 52, 255)        # the strip
EDGE = (16, 17, 24, 255)        # its outline
HOLE = (236, 238, 244, 255)     # the perforations
FRAME = (58, 120, 200, 255)     # the picture
FRAME_LIGHT = (120, 176, 236, 255)
PLAY = (255, 214, 64, 255)      # the triangle
PLAY_EDGE = (150, 104, 0, 255)


def box(draw, left, top, right, bottom, radius, fill, outline=None):
    """A rounded rectangle in icon pixels."""
    draw.rounded_rectangle([left * SCALE, top * SCALE, right * SCALE - 1, bottom * SCALE - 1], radius * SCALE, fill=fill,
                           outline=outline, width=SCALE if outline else 0)


def main():
    image = Image.new("RGBA", (WIDTH * SCALE, HEIGHT * SCALE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    box(draw, 1, 3, 47, 37, 3, FILM, EDGE)                       # the strip
    for column in range(6):                                      # perforations, top and bottom
        left = 4.5 + column * 7
        box(draw, left, 6, left + 4, 9.5, 1, HOLE)
        box(draw, left, 30.5, left + 4, 34, 1, HOLE)
    box(draw, 5, 12, 43, 28, 1.5, FRAME)                         # the picture
    draw.polygon([(5 * SCALE, 12 * SCALE), (43 * SCALE, 12 * SCALE), (5 * SCALE, 22 * SCALE)], fill=FRAME_LIGHT)
    box(draw, 5, 12, 43, 28, 1.5, None, EDGE)
    triangle = [(19.5 * SCALE, 14.5 * SCALE), (19.5 * SCALE, 25.5 * SCALE), (30 * SCALE, 20 * SCALE)]
    draw.polygon(triangle, fill=PLAY, outline=PLAY_EDGE)
    draw.line(triangle + [triangle[0]], fill=PLAY_EDGE, width=SCALE)
    image = image.resize((WIDTH, HEIGHT), Image.LANCZOS)
    image.save(os.path.join(os.path.dirname(os.path.abspath(__file__)), "Kinora.png"))
    print("wrote Kinora.png")


if __name__ == "__main__":
    main()
