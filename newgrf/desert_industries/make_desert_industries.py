#!/usr/bin/env python3
"""Makes the NewGRF 'Desert Industries' (Sub-tropical): draws the building sprites (gfx/sheet.png) and writes
desert_industries.nml and lang/english.lng.
Run:  python3 make_desert_industries.py      then:  nmlc --grf desert_industries.grf desert_industries.nml   (nmlc = NML, github.com/OpenTTD/nml)

Seven industries in three chains, all ending in a cargo that towns accept (Food, Goods), so every chain is complete without
touching the default industries:
  Sugar:  Sugar Cane Plantation (SGCN) -> Sugar Mill (SGCN -> SUGR) -> Bakery (SUGR -> FOOD)
  Nuts:   Nut Grove (NUTS) -> Oil Press (NUTS -> EOIL) -> Bakery (EOIL -> FOOD)
  Cotton: Cotton Plantation (COTN) -> Textile Mill (COTN -> GOOD)
The cargos SGCN, NUTS, COTN, SUGR, EOIL are added to Sub-tropical by the mod (src/firs_desert_cargo.cpp); FOOD and GOOD are default
cargos. The buildings are drawn here with Pillow (simple isometric blocks); no outside art is used."""
from pathlib import Path
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
W = 64            # a tile sprite is 64 pixels wide, the ground diamond is 31 pixels high
D = 31
HMAX = 44         # room above the diamond for tall buildings
SCALE = 4         # draw big, shrink, then make the alpha hard (no half-transparent pixels)
CELL = 68         # distance between sprites on the sheet
GROUND_Y = 1
BUILD_Y = GROUND_Y + D + 4


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) + (255,)


def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c[:3]) + (255,)


class Canvas:
    """One sprite, drawn at SCALE times the size."""

    def __init__(self, height, base=None):
        self.h = height
        self.img = Image.new("RGBA", (W * SCALE, height * SCALE), base or (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)
        self.cy = HMAX + D / 2       # centre of the ground diamond (building sprites)

    def poly(self, pts, fill):
        self.d.polygon([(x * SCALE, y * SCALE) for x, y in pts], fill=fill)

    def line(self, p, q, fill, width=1):
        self.d.line([(p[0] * SCALE, p[1] * SCALE), (q[0] * SCALE, q[1] * SCALE)], fill=fill, width=int(width * SCALE))

    def dot(self, x, y, r, fill):
        """Circle centred x pixels from the sprite centre, y pixels from the ground centre (building sprites)."""
        X, Y = W / 2 + x, self.cy + y
        self.d.ellipse([(X - r) * SCALE, (Y - r) * SCALE, (X + r) * SCALE, (Y + r) * SCALE], fill=fill)

    def box(self, h, color, f=0.62, z0=0, dx=0, dy=0, top=None):
        """Isometric block on the tile: footprint scaled by f, from height z0 to z0+h. Returns the centre of the top face."""
        a, b = 31.5 * f, 15.5 * f
        cx, cy = W / 2 + dx, self.cy + dy
        T, R, B, L = (cx, cy - b), (cx + a, cy), (cx, cy + b), (cx - a, cy)
        up = lambda p, z: (p[0], p[1] - z)
        left = [up(L, z0), up(B, z0), up(B, z0 + h), up(L, z0 + h)]
        right = [up(B, z0), up(R, z0), up(R, z0 + h), up(B, z0 + h)]
        topf = [up(T, z0 + h), up(R, z0 + h), up(B, z0 + h), up(L, z0 + h)]
        edge = shade(color, 0.45)
        self.poly(left, shade(color, 0.92))
        self.poly(right, shade(color, 0.68))
        self.poly(topf, top or shade(color, 1.12))
        for pts in (left, right, topf):
            self.d.line([(x * SCALE, y * SCALE) for x, y in pts + [pts[0]]], fill=edge, width=SCALE // 2)
        return (cx, cy - z0 - h)

    def cylinder(self, x, y, r, h, color):
        X, Y = W / 2 + x, self.cy + y
        S = SCALE
        self.d.rectangle([(X - r) * S, (Y - h) * S, (X + r) * S, Y * S], fill=shade(color, 0.85))
        self.d.ellipse([(X - r) * S, (Y - r / 2) * S, (X + r) * S, (Y + r / 2) * S], fill=shade(color, 0.85))
        self.d.ellipse([(X - r) * S, (Y - h - r / 2) * S, (X + r) * S, (Y - h + r / 2) * S], fill=shade(color, 1.1))
        self.d.line([((X - r) * S, (Y - h) * S), ((X - r) * S, Y * S)], fill=shade(color, 0.5), width=S // 2)
        self.d.line([((X + r) * S, (Y - h) * S), ((X + r) * S, Y * S)], fill=shade(color, 0.5), width=S // 2)

    def finish(self, diamond=False):
        if diamond:   # ground sprite: cut the texture to the tile diamond
            mask = Image.new("L", self.img.size, 0)
            ImageDraw.Draw(mask).polygon([(x * SCALE, y * SCALE) for x, y in
                                          [(W / 2, 0), (W, D / 2), (W / 2, D), (0, D / 2)]], fill=255)
            self.img.putalpha(mask)
        small = self.img.resize((W, self.h), Image.LANCZOS)
        r, g, b, a = small.split()
        a = a.point(lambda v: 255 if v >= 128 else 0)
        return Image.merge("RGBA", (r, g, b, a))


# ------------------------------------------------------------------ ground tiles
def ground(base):
    return Canvas(D, rgb(base))


def g_cane(flip):
    c = ground("#3f8a2e")
    for k in range(-40, 70, 7):
        if flip:
            c.line((k, D), (k + 64, 0), rgb("#69b648"), 2)
        else:
            c.line((k, 0), (k + 64, D), rgb("#69b648"), 2)
    return c.finish(True)


def g_dirt():
    c = ground("#a97c4a")
    for x, y in [(14, 14), (30, 8), (44, 20), (22, 24), (50, 12), (36, 17)]:
        c.dot_flat = None
        c.d.ellipse([(x - 1.5) * SCALE, (y - 1) * SCALE, (x + 1.5) * SCALE, (y + 1) * SCALE], fill=rgb("#8a6238"))
    return c.finish(True)


def g_cotton():
    c = ground("#4c9a38")
    for k in range(-40, 70, 7):
        c.line((k, 0), (k + 64, D), rgb("#5eb046"), 1.5)
    for x, y in [(12, 15), (20, 10), (27, 19), (34, 12), (41, 21), (47, 14), (24, 14), (37, 16), (52, 17), (17, 19)]:
        c.d.ellipse([(x - 1.6) * SCALE, (y - 1.6) * SCALE, (x + 1.6) * SCALE, (y + 1.6) * SCALE], fill=rgb("#f4f4ee"))
    return c.finish(True)


def g_conc():
    c = ground("#9b9a92")
    c.line((0, D / 2), (W, D / 2), rgb("#8b8a82"), 1)
    c.line((W / 2, 0), (W / 2, D), rgb("#8b8a82"), 1)
    return c.finish(True)


def g_grove():
    c = ground("#7f8a3e")
    for x, y in [(12, 15), (26, 8), (40, 22), (50, 13), (30, 20), (20, 22)]:
        c.d.ellipse([(x - 3) * SCALE, (y - 1.5) * SCALE, (x + 3) * SCALE, (y + 1.5) * SCALE], fill=rgb("#6c7a30"))
    return c.finish(True)


# ------------------------------------------------------------------ building sprites
H = HMAX + D


def b_farm():
    c = Canvas(H)
    c.box(11, rgb("#e6d9b4"), 0.5)
    c.box(4, rgb("#b5412b"), 0.6, z0=11)
    return c.finish()


def b_trees():
    c = Canvas(H)
    for x, y in [(-12, 0), (10, -3), (-2, 5), (4, 8), (-16, 7)]:
        c.d.rectangle([(W / 2 + x - 1) * SCALE, (c.cy + y - 6) * SCALE, (W / 2 + x + 1) * SCALE, (c.cy + y) * SCALE],
                      fill=rgb("#6b4524"))
        c.dot(x, y - 10, 7, rgb("#2f7a2a"))
        c.dot(x - 2, y - 12, 3.5, rgb("#4ea03c"))
    return c.finish()


def b_mill():
    c = Canvas(H)
    c.box(22, rgb("#d8ceb0"), 0.7)
    c.box(14, rgb("#8c8f96"), 0.14, z0=22, dx=10, dy=-4)
    return c.finish()


def b_silos():
    c = Canvas(H)
    c.box(3, rgb("#a9a79c"), 0.9)
    c.cylinder(-10, 5, 8, 28, rgb("#eee6cf"))
    c.cylinder(10, 0, 8, 34, rgb("#eee6cf"))
    return c.finish()


def b_press():
    c = Canvas(H)
    c.box(18, rgb("#d2c7a8"), 0.7)
    c.box(3, rgb("#6e6a60"), 0.76, z0=18)
    c.box(16, rgb("#8c8f96"), 0.13, z0=21, dx=-12, dy=3)
    return c.finish()


def b_textile(sky):
    c = Canvas(H)
    c.box(20, rgb("#b8473a"), 0.76, top=rgb("#6b6f7a"))
    if sky:
        for i in range(3):
            x, y = W / 2 - 14 + i * 12, c.cy - 24 + i * 1
            c.poly([(x, y), (x + 8, y - 3), (x + 8, y + 3), (x, y + 6)], rgb("#9bb8d8"))
    else:
        c.box(14, rgb("#6f4a3a"), 0.14, z0=20, dx=-12, dy=3)
    return c.finish()


def b_bakery():
    c = Canvas(H)
    c.box(16, rgb("#efe3c2"), 0.62)
    c.box(5, rgb("#c4502f"), 0.72, z0=16)
    c.box(14, rgb("#6b4524"), 0.12, z0=19, dx=9, dy=-3)
    return c.finish()


def b_yard():
    c = Canvas(H)
    c.box(6, rgb("#7189ab"), 0.82)
    return c.finish()


# name, builder(), is_ground
SPRITES = [
    ("G_CANE_A", lambda: g_cane(False), True), ("G_CANE_B", lambda: g_cane(True), True),
    ("G_DIRT", g_dirt, True), ("G_COTTON", g_cotton, True), ("G_CONC", g_conc, True), ("G_GROVE", g_grove, True),
    ("B_FARM", b_farm, False), ("B_TREES", b_trees, False), ("B_MILL", b_mill, False), ("B_SILOS", b_silos, False),
    ("B_PRESS", b_press, False), ("B_TEXTILE_A", lambda: b_textile(True), False), ("B_TEXTILE_B", lambda: b_textile(False), False),
    ("B_BAKERY", b_bakery, False), ("B_YARD", b_yard, False),
]
IDX = {name: i for i, (name, _, _) in enumerate(SPRITES)}


def make_sheet():
    sheet = Image.new("RGBA", (CELL * len(SPRITES), BUILD_Y + H + 1), (0, 0, 0, 0))
    rects = []
    for i, (name, fn, is_ground) in enumerate(SPRITES):
        x = i * CELL
        y = GROUND_Y if is_ground else BUILD_Y
        im = fn()
        sheet.paste(im, (x, y))
        if is_ground:
            rects.append("[%4d, %3d, %d, %d, -31,   0]" % (x, y, W, D))
        else:
            rects.append("[%4d, %3d, %d, %d, -31, %3d]" % (x, y, W, H, -HMAX))
    (HERE / "gfx").mkdir(exist_ok=True)
    sheet.save(HERE / "gfx" / "sheet.png")
    return rects


# ------------------------------------------------------------------ the NewGRF
# tile name: (ground sprite, building sprite or None)
TILES = {
    "tile_cane_a": ("G_CANE_A", None), "tile_cane_b": ("G_CANE_B", None), "tile_cotton": ("G_COTTON", None),
    "tile_grove": ("G_GROVE", "B_TREES"), "tile_farm": ("G_DIRT", "B_FARM"),
    "tile_mill": ("G_CONC", "B_MILL"), "tile_silos": ("G_CONC", "B_SILOS"), "tile_press": ("G_CONC", "B_PRESS"),
    "tile_textile_a": ("G_CONC", "B_TEXTILE_A"), "tile_textile_b": ("G_CONC", "B_TEXTILE_B"),
    "tile_bakery": ("G_CONC", "B_BAKERY"), "tile_yard": ("G_CONC", "B_YARD"),
}
# layout name: [(x, y, tile)]
LAYOUTS = {
    "lay_sugar_plantation": [(0, 0, "tile_cane_a"), (1, 0, "tile_cane_b"), (0, 1, "tile_farm"), (1, 1, "tile_cane_a")],
    "lay_nut_grove": [(0, 0, "tile_grove"), (1, 0, "tile_grove"), (0, 1, "tile_farm"), (1, 1, "tile_grove")],
    "lay_cotton_plantation": [(0, 0, "tile_cotton"), (1, 0, "tile_cotton"), (0, 1, "tile_farm"), (1, 1, "tile_cotton")],
    "lay_sugar_mill": [(0, 0, "tile_mill"), (1, 0, "tile_silos"), (0, 1, "tile_yard"), (1, 1, "tile_mill")],
    "lay_oil_press": [(0, 0, "tile_press"), (1, 0, "tile_yard")],
    "lay_textile_mill": [(0, 0, "tile_textile_a"), (1, 0, "tile_textile_a"), (0, 1, "tile_yard"), (1, 1, "tile_textile_b")],
    "lay_bakery": [(0, 0, "tile_bakery"), (1, 0, "tile_yard")],
}
# key, name, substitute, life type, layout, cargo_types, map colour, prob map gen, prob in game, fund cost, flags, new msg, closure, up, down
INDUSTRIES = [
    ("sugar_plantation", "Sugar Cane Plantation", "FRUIT_PLANTATION", "ORGANIC", "lay_sugar_plantation",
     'produce_cargo("SGCN", 9)', 0x6C, 5, 4, 55, "0",
     "New sugar cane plantation under construction", "Sugar cane plantation is closing down",
     "Sugar cane harvest is bigger this year", "Sugar cane harvest is smaller this year"),
    ("nut_grove", "Nut Grove", "FRUIT_PLANTATION", "ORGANIC", "lay_nut_grove",
     'produce_cargo("NUTS", 8)', 0x75, 5, 4, 55, "0",
     "New nut grove is being planted", "Nut grove is closing down",
     "Nut harvest is bigger this year", "Nut harvest is smaller this year"),
    ("cotton_plantation", "Cotton Plantation", "FRUIT_PLANTATION", "ORGANIC", "lay_cotton_plantation",
     'produce_cargo("COTN", 10)', 0x0F, 5, 4, 55, "0",
     "New cotton plantation under construction", "Cotton plantation is closing down",
     "Cotton harvest is bigger this year", "Cotton harvest is smaller this year"),
    ("sugar_mill", "Sugar Mill", "TROPICAL_FACTORY", "PROCESSING", "lay_sugar_mill",
     'accept_cargo("SGCN", produce_cargo("SUGR", 1))', 0x0A, 4, 3, 70, "0",
     "New sugar mill under construction", "Sugar mill is closing down",
     "Sugar mill increases production", "Sugar mill decreases production"),
    ("oil_press", "Oil Press", "TROPICAL_FACTORY", "PROCESSING", "lay_oil_press",
     'accept_cargo("NUTS", produce_cargo("EOIL", 1))', 0x7C, 4, 3, 60, "0",
     "New oil press under construction", "Oil press is closing down",
     "Oil press increases production", "Oil press decreases production"),
    ("textile_mill", "Textile Mill", "TROPICAL_FACTORY", "PROCESSING", "lay_textile_mill",
     'accept_cargo("COTN", produce_cargo("GOOD", 1))', 0xB8, 4, 3, 70, "0",
     "New textile mill under construction", "Textile mill is closing down",
     "Textile mill increases production", "Textile mill decreases production"),
    ("bakery", "Bakery", "TROPICAL_FACTORY", "PROCESSING", "lay_bakery",
     'accept_cargo("SUGR", produce_cargo("FOOD", 1)),\n            accept_cargo("EOIL", produce_cargo("FOOD", 1))',
     0x0C, 5, 4, 65, "bitmask(IND_FLAG_BUILT_NEAR_TOWN)",
     "New bakery under construction", "Bakery is closing down",
     "Bakery increases production", "Bakery decreases production"),
]


def write_nml(rects):
    o = []
    o.append("""/* Desert Industries: generated by make_desert_industries.py, do not edit by hand. */
grf {
    grfid: "SDI\\01";
    name: string(STR_GRF_NAME);
    desc: string(STR_GRF_DESCRIPTION);
    version: 1;
    min_compatible_version: 1;
}

/* The cargos are added to Sub-tropical by the mod (src/firs_desert_cargo.cpp); FOOD and GOOD are default cargos. */
cargotable {
    SGCN, NUTS, COTN, SUGR, EOIL, FOOD, GOOD
}

spriteset (ss_all, ZOOM_LEVEL_NORMAL, BIT_DEPTH_32BPP, "gfx/sheet.png") {""")
    for (name, _, _), r in zip(SPRITES, rects):
        o.append("    %s   // %d %s" % (r, IDX[name], name))
    o.append("}\n")
    for tile, (g, b) in TILES.items():
        o.append("spritelayout lt_%s {" % tile)
        o.append("    ground { sprite: ss_all(%d); }" % IDX[g])
        if b:
            o.append("    building { sprite: ss_all(%d); xextent: 16; yextent: 16; zextent: 20; }" % IDX[b])
        o.append("}")
        o.append("item(FEAT_INDUSTRYTILES, %s) {" % tile)
        o.append("    property {")
        o.append("        substitute: 0;   // the tile that supplies the defaults; the flag below makes the tile accept what the industry accepts")
        o.append("        special_flags: bitmask(INDTILE_FLAG_ACCEPT_ALL);")
        o.append("    }")
        o.append("    graphics {")
        o.append("        lt_%s;" % tile)
        o.append("    }")
        o.append("}\n")
    for lay, cells in LAYOUTS.items():
        o.append("tilelayout %s {" % lay)
        for x, y, t in cells:
            o.append("    %d, %d: %s;" % (x, y, t))
        o.append("}")
    o.append("")
    for key, name, subst, life, lay, cargo, colour, pmg, pig, cost, flags, *_ in INDUSTRIES:
        o.append("item(FEAT_INDUSTRIES, ind_%s) {" % key)
        o.append("    property {")
        o.append("        substitute: INDUSTRYTYPE_%s;" % subst)
        o.append("        name: string(STR_NAME_%s);" % key.upper())
        o.append("        layouts: [%s];" % lay)
        o.append("        life_type: IND_LIFE_TYPE_%s;" % life)
        o.append("        cargo_types: [")
        o.append("            %s" % cargo)
        o.append("        ];")
        o.append("        map_colour: 0x%02X;" % colour)
        o.append("        prob_map_gen: %d;" % pmg)
        o.append("        prob_in_game: %d;" % pig)
        o.append("        fund_cost_multiplier: %d;" % cost)
        o.append("        spec_flags: %s;" % flags)
        o.append("        new_ind_msg: string(STR_NEW_%s);" % key.upper())
        o.append("        closure_msg: string(STR_CLOSE_%s);" % key.upper())
        o.append("        prod_increase_msg: string(STR_UP_%s);" % key.upper())
        o.append("        prod_decrease_msg: string(STR_DOWN_%s);" % key.upper())
        o.append("    }")
        o.append("}\n")
    (HERE / "desert_industries.nml").write_text("\n".join(o), encoding="utf-8")


def write_lang():
    o = ["##grflangid 0x01",
         "STR_GRF_NAME                                  :Desert Industries",
         "STR_GRF_DESCRIPTION                           :{ORANGE}Desert Industries{}{BLACK}Seven industries for Sub-tropical: sugar, nut and cotton chains that end in Food and Goods. Needs the cargos SGCN, NUTS, COTN, SUGR and EOIL from the mod."]
    for key, name, *_rest in INDUSTRIES:
        new, close, up, down = _rest[-4:]
        K = key.upper()
        o.append("STR_NAME_%-30s:%s" % (K, name))
        o.append("STR_NEW_%-31s:%s" % (K, new))
        o.append("STR_CLOSE_%-29s:%s" % (K, close))
        o.append("STR_UP_%-32s:%s" % (K, up))
        o.append("STR_DOWN_%-30s:%s" % (K, down))
    (HERE / "lang").mkdir(exist_ok=True)
    (HERE / "lang" / "english.lng").write_text("\n".join(o) + "\n", encoding="utf-8")


if __name__ == "__main__":
    write_nml(make_sheet())
    write_lang()
    print("wrote gfx/sheet.png, desert_industries.nml, lang/english.lng")
