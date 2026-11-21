import json, os, shutil
from fontTools.ttLib import TTFont
src = r'D:\voxl-build\f7\package\fonts\Framework7Icons-Regular.ttf'
dst_dir = r'D:\Users\G433m\Documents\aiplantrx\ui\fonts'
f = TTFont(src)
order = [g for g in f.getGlyphOrder()
         if not g.startswith('.') and g not in ('space', 'nonmarkingreturn') and len(g) > 1 and not g.startswith('uni')]
# Glyphs reached through ASCII codepoints are the ligature input letters (a-z, 0-9, _), not icons.
cmap_names = {g for c, g in f.getBestCmap().items() if c < 0x80}
icons = [g for g in order if g not in cmap_names]
mapping = {g: 0xE000 + i for i, g in enumerate(icons)}
for table in f['cmap'].tables:
    if table.isUnicode():
        for g, cp in mapping.items():
            table.cmap[cp] = g
f['name'].setName('Framework7 Icons PUA', 1, 3, 1, 0x409)
f['name'].setName('Framework7IconsPUA-Regular', 6, 3, 1, 0x409)
f.save(os.path.join(dst_dir, 'Framework7Icons-PUA.ttf'))
json.dump({g: '%04X' % cp for g, cp in mapping.items()}, open(os.path.join(dst_dir, 'framework7-icons-pua.json'), 'w'), indent=0)
shutil.copy(r'D:\voxl-build\f7\package\fonts\Framework7Icons-Regular.woff2', dst_dir)
shutil.copy(r'D:\voxl-build\f7\package\LICENSE', os.path.join(dst_dir, 'Framework7Icons-LICENSE.txt'))
print('icons', len(icons), 'first', icons[:3], 'last', icons[-1], hex(0xE000 + len(icons) - 1))

