"""
Generates server-icon.png — a 3D isometric server rack icon for Port OS "My Server".
Requires: pip install Pillow
Run once: python generate_server_icon.py
"""
from PIL import Image, ImageDraw, ImageFilter


def generate_server_icon(size: int = 256) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    s = size / 256.0  # scale factor

    def p(x: float, y: float):
        return (int(x * s), int(y * s))

    # Front face of the server chassis
    fl = p(22, 108)    # front-left
    fr = p(196, 108)   # front-right
    br = p(196, 188)   # bottom-right
    bl = p(22, 188)    # bottom-left

    # Isometric depth offset (right 30px, up 22px)
    ox, oy = int(30 * s), int(-22 * s)

    # Top-face back corners
    tl = (fl[0] + ox, fl[1] + oy)
    tr = (fr[0] + ox, fr[1] + oy)

    # ── Right face ──────────────────────────────────────────
    right_face = [fr, tr, (tr[0], br[1] + oy), br]
    draw.polygon(right_face, fill=(28, 32, 44, 255))

    # Right face highlight edge (top-right)
    draw.line([fr, tr], fill=(50, 56, 70, 200), width=max(1, int(s)))

    # ── Top face ─────────────────────────────────────────────
    top_face = [fl, tl, tr, fr]
    # Fill with gradient-like layered lines
    draw.polygon(top_face, fill=(72, 78, 96, 255))
    # Subtle highlight strip along the front top edge
    draw.line([fl, fr], fill=(100, 106, 124, 180), width=max(1, int(2 * s)))

    # ── Front face base ──────────────────────────────────────
    draw.rectangle([fl, br], fill=(46, 50, 63, 255))

    # Subtle vertical gradient (lighter top → darker bottom)
    h_front = br[1] - fl[1]
    for dy in range(h_front):
        t = dy / max(h_front - 1, 1)
        a = int(25 * (1.0 - t))
        color = (88, 94, 114, a)
        draw.line([(fl[0], fl[1] + dy), (br[0], fl[1] + dy)], fill=color)

    # ── Rack units (3 units on front face) ───────────────────
    rack_x1 = fl[0] + int(6 * s)
    rack_x2 = br[0] - int(6 * s)
    rack_top_y = fl[1] + int(8 * s)
    rack_h = int(16 * s)
    rack_gap = int(6 * s)

    for i in range(3):
        ry = rack_top_y + i * (rack_h + rack_gap)
        rx1, rx2 = rack_x1, rack_x2

        # Slot background (recessed)
        draw.rectangle([(rx1, ry), (rx2, ry + rack_h)], fill=(22, 25, 35, 255))
        draw.rectangle([(rx1 + 1, ry + 1), (rx2 - 1, ry + rack_h - 1)], fill=(16, 18, 26, 255))

        # Drive bay slots inside the rack unit
        bay_w = int(12 * s)
        bay_h = rack_h - int(6 * s)
        bay_y = ry + int(3 * s)
        for j in range(5):
            bx = rx1 + int(4 * s) + j * (bay_w + int(2 * s))
            if bx + bay_w < rx2 - int(20 * s):
                draw.rectangle([(bx, bay_y), (bx + bay_w, bay_y + bay_h)],
                                fill=(26, 30, 40, 255))

        # Activity LED (green)
        led_r = max(2, int(3.5 * s))
        led_x = rx2 - int(14 * s)
        led_y = ry + rack_h // 2
        draw.ellipse([(led_x - led_r, led_y - led_r), (led_x + led_r, led_y + led_r)],
                     fill=(0, 220, 80, 255))

        # Status LED (blue)
        led2_x = led_x - int(10 * s)
        draw.ellipse([(led2_x - led_r, led_y - led_r), (led2_x + led_r, led_y + led_r)],
                     fill=(0, 150, 255, 255))

    # ── Bottom strip: expansion / USB ports ──────────────────
    bottom_strip_y = fl[1] + 3 * (rack_h + rack_gap) + int(8 * s)
    bs_h = int(10 * s)
    draw.rectangle([(rack_x1, bottom_strip_y), (rack_x2, bottom_strip_y + bs_h)],
                   fill=(20, 23, 33, 255))
    for k in range(4):
        px = rack_x1 + int(4 * s) + k * int(16 * s)
        draw.rectangle([(px, bottom_strip_y + int(2 * s)),
                         (px + int(10 * s), bottom_strip_y + bs_h - int(2 * s))],
                        fill=(14, 16, 22, 255))

    # Power button (bottom-right of front face)
    pw_x = br[0] - int(14 * s)
    pw_y = br[1] - int(12 * s)
    pw_r = max(3, int(5 * s))
    draw.ellipse([(pw_x - pw_r, pw_y - pw_r), (pw_x + pw_r, pw_y + pw_r)],
                 fill=(0, 200, 75, 255))
    draw.ellipse([(pw_x - pw_r + 2, pw_y - pw_r + 2),
                  (pw_x + pw_r - 2, pw_y + pw_r - 2)],
                 fill=(200, 255, 210, 255))

    # ── Edge highlights ───────────────────────────────────────
    draw.line([fl, fr], fill=(96, 102, 122, 220), width=max(1, int(2 * s)))   # front top
    draw.line([fl, bl], fill=(82, 88, 106, 180), width=max(1, int(2 * s)))    # front left
    draw.line([fl, tl], fill=(80, 86, 104, 160), width=max(1, int(s)))        # top left
    draw.line([tl, tr], fill=(68, 74, 90, 150), width=max(1, int(s)))         # top back
    draw.line([tr, fr], fill=(55, 60, 76, 140), width=max(1, int(s)))         # top right

    # ── LED glow overlay ─────────────────────────────────────
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for i in range(3):
        ry = rack_top_y + i * (rack_h + rack_gap)
        led_x = rack_x2 - int(14 * s)
        led_y = ry + rack_h // 2
        gr = int(10 * s)
        gd.ellipse([(led_x - gr, led_y - gr), (led_x + gr, led_y + gr)],
                   fill=(0, 255, 80, 55))
        led2_x = led_x - int(10 * s)
        gd.ellipse([(led2_x - gr, led_y - gr), (led2_x + gr, led_y + gr)],
                   fill=(0, 150, 255, 38))
    glow = glow.filter(ImageFilter.GaussianBlur(int(5 * s)))
    img = Image.alpha_composite(img, glow)

    return img


if __name__ == "__main__":
    icon256 = generate_server_icon(256)
    icon256.save("server-icon.png")
    print("server-icon.png generated (256x256).")

    # 230x230 used by the desktop app
    icon230 = generate_server_icon(230)
    icon230.save("server-icon-small.png")
    print("server-icon-small.png generated (230x230).")
