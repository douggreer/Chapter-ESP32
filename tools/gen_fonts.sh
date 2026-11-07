#!/bin/sh
# Regenerates main/ui/fonts/ui_font_*.c: Montserrat Medium with accented Latin, Cyrillic,
# Vietnamese and common punctuation, plus LVGL's built-in symbol icons (LV_SYMBOL_*), so they
# drop in for lv_font_montserrat_*. Needs Node (npx fetches lv_font_conv) and a built project
# (for the LVGL sources in managed_components/).
set -e
cd "$(dirname "$0")/.."
SRC=managed_components/lvgl__lvgl/scripts/built_in_font
# Same symbol list as LVGL's built_in_font_gen.py.
SYMS=61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650
# ASCII, Latin-1, Latin Extended-A/B, Cyrillic, Vietnamese, punctuation, bullet, euro, trademark.
TEXT=0x20-0x7F,0xA0-0x24F,0x400-0x45F,0x1EA0-0x1EF9,0x2010-0x2027,0x2030,0x2039,0x203A,0x20AC,0x2122
for size in 14 16 20; do
    npx -y lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size $size --format lvgl \
        --font $SRC/Montserrat-Medium.ttf -r $TEXT \
        --font $SRC/FontAwesome5-Solid+Brands+Regular.woff -r $SYMS \
        --lv-include lvgl.h -o main/ui/fonts/ui_font_$size.c
done
