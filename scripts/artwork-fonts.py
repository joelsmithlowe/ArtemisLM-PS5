# The site's Recursive webfonts as static TTFs with their own family names
# ("PS5LM Mono", "PS5LM Sans"), for rendering the app's SVG artwork.
import os, sys
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
for name, family in (("recursive-mono-v1", "PS5LM Mono"), ("recursive-sans-v1", "PS5LM Sans")):
    for weight, style in ((400, "Regular"), (800, "Bold")):
        f = TTFont(f"{src}/{name}.woff2")
        f.flavor = None
        axes = {"wght": weight}
        if "CASL" in [a.axisTag for a in f["fvar"].axes]:
            axes["CASL"] = 0
        f = instantiateVariableFont(f, axes)
        for rec in f["name"].names:
            if rec.nameID in (1, 16):
                rec.string = family
            elif rec.nameID in (2, 17):
                rec.string = style
            elif rec.nameID == 4:
                rec.string = f"{family} {style}"
            elif rec.nameID == 6:
                rec.string = f"{family.replace(' ', '')}-{style}"
        f["OS/2"].usWeightClass = weight
        f["OS/2"].fsSelection = (f["OS/2"].fsSelection & ~0b1100001) | (0b100000 if style == "Bold" else 0b1000000)
        f["head"].macStyle = 1 if style == "Bold" else 0
        out = f"{dst}/{family.replace(' ', '')}-{style}.ttf"
        f.save(out)
        print(out)
