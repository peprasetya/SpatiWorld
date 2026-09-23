"""Build the SpatiWorld skin ("Deep Glass") from the Firestorm skin and its Dark theme.

Run from indra/newview/skins. Regenerates skins/spatiworld entirely, so it can be rerun after a
rebase that changes the Firestorm skin it starts from.
"""
import pathlib, shutil, re
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw, ImageFilter

SKINS = pathlib.Path(".")
SRC = SKINS / "firestorm"
DARK = SRC / "themes" / "dark"
OUT = SKINS / "spatiworld"

if OUT.exists():
    shutil.rmtree(OUT)
shutil.copytree(SRC, OUT, ignore=shutil.ignore_patterns("themes"))

# ---------------------------------------------------------------- colours: Firestorm + Dark + ours
def load_colors(path):
    return ET.parse(path)

colors = load_colors(OUT / "colors.xml")
root = colors.getroot()
by_name = {c.get("name"): c for c in root.findall("color")}
for c in load_colors(DARK / "colors.xml").getroot().findall("color"):
    name = c.get("name")
    if name in by_name:
        root.remove(by_name[name])
    root.append(c)
    by_name[name] = c

def rgba(r, g, b, a=1.0):
    return f"{r:.3f} {g:.3f} {b:.3f} {a:.3f}"

ACCENT = (0.34, 0.84, 1.00)
NAVY = (0.055, 0.080, 0.125)
TEXT = (0.90, 0.95, 1.00)
palette = {
    # Windows. The frame textures carry the colour; these only tint them (white = as drawn)
    # and set how solid they are: focused opaque, unfocused see-through.
    "SpatiFloaterTint": rgba(1, 1, 1, 1.0),
    "SpatiFloaterTintAlpha": rgba(1, 1, 1, 0.62),
    "floater_bg_color": rgba(*NAVY, 1.0),
    "floater_bg_color_a": rgba(*NAVY, 0.62),
    "FloaterDefaultBackgroundColor": rgba(*NAVY, 0.62),
    "FloaterFocusBackgroundColor": rgba(*NAVY, 1.0),
    "FloaterFocusBorderColor": rgba(*ACCENT, 0.90),
    "FloaterUnfocusBorderColor": rgba(0.55, 0.65, 0.78, 0.35),
    # Menus: solid, lifted by a strong shadow, an accent bar under the item in reach.
    "MenuDefaultBgColor": rgba(0.060, 0.090, 0.140, 0.98),
    "MenuPopupBgColor": rgba(0.060, 0.090, 0.140, 0.98),
    "MenuBarBgColor": rgba(0.045, 0.065, 0.105, 0.92),
    "MenuItemHighlightBgColor": rgba(0.18, 0.58, 0.82, 0.95),
    "MenuItemHighlightFgColor": rgba(1, 1, 1, 1),
    "MenuItemEnabledColor": rgba(*TEXT, 1),
    "MenuItemDisabledColor": rgba(0.48, 0.54, 0.62, 1),
    "ColorDropShadow": rgba(0, 0, 0, 0.62),
    # Buttons are drawn in colour; the tint leaves them alone.
    "ButtonImageColor": rgba(1, 1, 1, 1),
    "ButtonLabelColor": rgba(*TEXT, 1),
    "ButtonLabelSelectedColor": rgba(1, 1, 1, 1),
    "ButtonLabelDisabledColor": rgba(0.50, 0.56, 0.64, 1),
    "ButtonLabelSelectedDisabledColor": rgba(0.55, 0.70, 0.78, 1),
    "ToolTipBgColor": rgba(0.060, 0.090, 0.140, 0.96),
    "ToolTipBorderColor": rgba(*ACCENT, 0.80),
    "ToolTipTextColor": rgba(*TEXT, 1),
    "EmphasisColor": rgba(*ACCENT, 1),
}
for name, value in palette.items():
    c = by_name.get(name)
    if c is None:
        c = ET.SubElement(root, "color", name=name)
        by_name[name] = c
    for attr in list(c.attrib):
        if attr != "name":
            del c.attrib[attr]
    c.set("value", value)
ET.indent(colors, space="  ")
colors.write(OUT / "colors.xml", encoding="utf-8", xml_declaration=True)

# ---------------------------------------------------------------- textures: Firestorm + Dark + ours
tex_tree = ET.parse(OUT / "textures" / "textures.xml")
tex_root = tex_tree.getroot()
tex_by = {t.get("name"): t for t in tex_root.findall("texture")}

def put_texture(name, file_name, **scale):
    t = tex_by.get(name)
    if t is not None:
        tex_root.remove(t)
    t = ET.SubElement(tex_root, "texture", name=name, file_name=file_name, preload="true")
    for k, v in scale.items():
        t.set(f"scale.{k}", str(v))
    tex_by[name] = t

dark_tex = DARK / "textures"
for f in dark_tex.rglob("*"):
    if f.is_file() and f.name != "textures.xml":
        dest = OUT / "textures" / f.relative_to(dark_tex)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, dest)
for t in ET.parse(dark_tex / "textures.xml").getroot().findall("texture"):
    name = t.get("name")
    if name in tex_by:
        tex_root.remove(tex_by[name])
    tex_root.append(t)
    tex_by[name] = t

GLASS = OUT / "textures" / "spatiworld"
GLASS.mkdir(parents=True, exist_ok=True)
SS = 4  # drawn four times over and scaled down, for clean round corners

def c8(rgb, a=1.0):
    return tuple(int(round(v * 255)) for v in rgb) + (int(round(a * 255)),)

def tile(w, h, radius, top, bottom, rim, rim_width=1.0, highlight=0.10, header=None):
    """A rounded tile: a vertical gradient, an optional header band, a rim and a top sheen."""
    W, H, R = w * SS, h * SS, radius * SS
    body = Image.new("RGBA", (W, H))
    px = body.load()
    for y in range(H):
        t = y / max(1, H - 1)
        col = tuple(top[i] * (1 - t) + bottom[i] * t for i in range(4))
        if header and y < header[0] * SS:
            col = header[1]
        for x in range(W):
            px[x, y] = tuple(int(round(v * 255)) for v in col)
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), R, fill=255)
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out.paste(body, (0, 0), mask)
    d = ImageDraw.Draw(out)
    # Sheen along the top: light catching the upper edge, what makes it read as raised.
    if highlight > 0:
        sheen = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(sheen).rounded_rectangle(
            (SS, SS, W - 1 - SS, int(H * 0.45)), max(1, R - SS), fill=(255, 255, 255, int(255 * highlight)))
        sheen = sheen.filter(ImageFilter.GaussianBlur(SS * 1.5))
        clipped = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        clipped.paste(sheen, (0, 0), mask)
        out = Image.alpha_composite(out, clipped)
        d = ImageDraw.Draw(out)
    if header:
        y = header[0] * SS
        d.line((R // 2, y, W - R // 2, y), fill=c8(rim[:3], rim[3] * 0.55), width=SS)
    d.rounded_rectangle((0, 0, W - 1, H - 1), R, outline=c8(rim[:3], rim[3]), width=max(1, int(rim_width * SS)))
    return out.resize((w, h), Image.LANCZOS)

def save(img, rel):
    path = GLASS / rel
    img.save(path)
    return f"spatiworld/{rel}"

# Windows: 64x96 with a 36-pixel top that does not stretch (the title), corners 10.
BODY_TOP = (0.075, 0.110, 0.170, 1.0)
BODY_BOTTOM = (0.050, 0.072, 0.115, 1.0)
HEAD = (0.095, 0.145, 0.225, 1.0)
for name, rim in (("Window_Foreground", ACCENT + (0.85,)), ("Window_Background", (0.55, 0.65, 0.78, 0.35))):
    img = tile(64, 96, 10, BODY_TOP, BODY_BOTTOM, rim, 1.5, 0.05, header=(26, HEAD))
    put_texture(name, save(img, f"{name}.png"), left=12, top=60, right=52, bottom=12)
for name, rim in (("Window_NoTitle_Foreground", ACCENT + (0.85,)), ("Window_NoTitle_Background", (0.55, 0.65, 0.78, 0.35))):
    img = tile(64, 64, 10, BODY_TOP, BODY_BOTTOM, rim, 1.5, 0.05)
    put_texture(name, save(img, f"{name}.png"), left=12, top=52, right=52, bottom=12)

# Push buttons: 32x23, the size the layouts already use.
BUTTONS = {
    "PushButton_Off":               ((0.17, 0.24, 0.35, 1), (0.11, 0.16, 0.24, 1), (0.50, 0.68, 0.86, 0.60), 0.14),
    "PushButton_Over":              ((0.22, 0.33, 0.48, 1), (0.14, 0.21, 0.32, 1), ACCENT + (1.0,), 0.18),
    "PushButton_Press":             ((0.10, 0.36, 0.50, 1), (0.08, 0.28, 0.40, 1), ACCENT + (1.0,), 0.04),
    "PushButton_Selected":          ((0.18, 0.56, 0.76, 1), (0.12, 0.42, 0.60, 1), (0.60, 0.95, 1.00, 1.0), 0.16),
    "PushButton_Selected_Press":    ((0.12, 0.44, 0.62, 1), (0.10, 0.34, 0.50, 1), (0.60, 0.95, 1.00, 1.0), 0.04),
    "PushButton_On":                ((0.18, 0.56, 0.76, 1), (0.12, 0.42, 0.60, 1), (0.60, 0.95, 1.00, 1.0), 0.16),
    "PushButton_On_Selected":       ((0.18, 0.56, 0.76, 1), (0.12, 0.42, 0.60, 1), (0.60, 0.95, 1.00, 1.0), 0.16),
    "PushButton_Disabled":          ((0.13, 0.15, 0.19, 0.7), (0.10, 0.12, 0.16, 0.7), (0.45, 0.50, 0.56, 0.35), 0.0),
    "PushButton_Selected_Disabled": ((0.12, 0.28, 0.36, 0.7), (0.10, 0.22, 0.30, 0.7), (0.45, 0.60, 0.66, 0.40), 0.0),
}
for name, (top, bottom, rim, sheen) in BUTTONS.items():
    img = tile(32, 23, 6, top, bottom, rim, 1.0, sheen)
    put_texture(name, save(img, f"{name}.png"), left=7, top=17, right=25, bottom=6)

# Toolbar buttons: separate tiles that float, not segments of a flat strip.
TOOLBAR = {
    "Off":      ((0.09, 0.13, 0.20, 0.86), (0.06, 0.09, 0.15, 0.86), (0.48, 0.70, 0.90, 0.55), 0.12),
    "Over":     ((0.15, 0.25, 0.38, 0.95), (0.10, 0.17, 0.27, 0.95), ACCENT + (1.0,), 0.18),
    "Selected": ((0.16, 0.50, 0.70, 0.97), (0.11, 0.38, 0.55, 0.97), (0.60, 0.95, 1.00, 1.0), 0.16),
    "Flash":    ((0.70, 0.52, 0.12, 0.97), (0.55, 0.40, 0.08, 0.97), (1.00, 0.85, 0.40, 1.0), 0.16),
}
for state, (top, bottom, rim, sheen) in TOOLBAR.items():
    img = tile(31, 25, 6, top, bottom, rim, 1.0, sheen)
    rel = save(img, f"Toolbar_{state}.png")
    for pos in ("Left", "Middle", "Right"):
        put_texture(f"Toolbar_{pos}_{state}", rel, left=7, top=19, right=24, bottom=6)

ET.indent(tex_tree, space="  ")
tex_tree.write(OUT / "textures" / "textures.xml", encoding="utf-8", xml_declaration=True)

# ---------------------------------------------------------------- widgets
widgets = OUT / "xui" / "en" / "widgets"
widgets.mkdir(parents=True, exist_ok=True)
floater = (widgets / "floater.xml").read_text()
floater = floater.replace('bg_alpha_color="floater_bg_color_a"', 'bg_alpha_color="SpatiFloaterTintAlpha"')
floater = floater.replace('bg_opaque_color="floater_bg_color"', 'bg_opaque_color="SpatiFloaterTint"')
assert "SpatiFloaterTint" in floater
(widgets / "floater.xml").write_text(floater)
(widgets / "menu.xml").write_text("""<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<!-- SpatiWorld: menus are pointed at from a metre away, so their items are the bigger font. -->
<menu name="menu"
      bg_color="MenuDefaultBgColor"
      bg_visible="true"
      drop_shadow="true"
      tear_off="false"
      font="SansSerif"
      shortcut_pad="18">
</menu>
""")
(widgets / "menu_item.xml").write_text("""<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<!-- SpatiWorld: the top-level menu names, the same size as the items under them. -->
<menu_item font="SansSerif" />
""")
print("skin written to", OUT)
