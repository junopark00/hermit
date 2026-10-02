"""Regenerates the Hermit icons: a pixel-art hermit crab carrying the Shell seashell (16x16).

Usage (from anywhere): python hermit/branding/deploy.py
The picture is the text grid below (the shell is the same as the Shell host icon);
pixelart.py turns it into crisp ICO and SVG files: app/res/hermit.svg for the window icon, which Qt
renders, and app/hermit.ico for the exe.
"""
import os

import pixelart as px

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))

# Hermit crab seen from the front: the spiral shell on top, eyes on stalks, claws up
HERMIT = px.parse("""
....KKKKKKKK....
..KKSSTTTTTtKK..
.KSSTTKKKKTTttK.
.KSTTKTTTTKTttK.
KSTTKTKKKTKTtttK
KTTTKTKSKTKTtttK
KTTTTKTTTKTTtttK
.KKKKKKKKKKKKKK.
KK..KwK..KwK..KK
KRK.KKK..KKK.KRK
KRRK.K....K.KRRK
.KRRKRRRRRRKRRK.
..KRRRRRRRRRRK..
..KRrRRRRRRrRK..
.K.KKKKKKKKKK.K.
.K..K......K..K.
""")


def main():
    svg = px.svg(HERMIT)
    for path in (os.path.join(HERE, 'hermit.svg'), os.path.join(REPO, 'app', 'res', 'hermit.svg')):
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(svg)
    with open(os.path.join(REPO, 'app', 'hermit.ico'), 'wb') as f:
        f.write(px.ico(HERMIT))
    print('Hermit icons regenerated')


if __name__ == '__main__':
    main()
