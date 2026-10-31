#!/usr/bin/env python3
"""Reject application images containing the legacy LCD task banner.
This is an artifact identity check, not proof of target compatibility or display.
Use the app ELF / uncompressed BIN before signing/packaging, not an outer fwpkg.
"""
import argparse
import hashlib
from pathlib import Path

REQUIRED = (b'BUILD=HY-LCD-ONLY-1', b'I2C=OFF scans=OFF')
FORBIDDEN = (b'demo 200_lcd_display hello world!', b'BOARD CHECK', b'LCD diagnosis v30')


def verify(path):
    path = Path(path)
    data = path.read_bytes()
    missing = [text.decode() for text in REQUIRED if text not in data]
    old = [text.decode() for text in FORBIDDEN if text in data]
    if missing or old:
        raise ValueError(f'REJECT {path}: missing={missing}; legacy markers={old}. '
                         'Check SDK source list/config and rebuild; do not flash this artifact. '
                         'Encrypted/compressed containers cannot be checked with this tool.')
    print('PASS application marker check:', path)
    print('SHA256:', hashlib.sha256(data).hexdigest())
    print('Still requires target build/boot and visual validation; not a hardware test.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    args = parser.parse_args()
    try:
        verify(args.image)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
