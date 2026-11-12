#!/usr/bin/env python3
"""Packs a PS Vita VPK.

A VPK is a plain zip archive with eboot.bin and sce_sys/param.sfo inside. This does the same
job as vita-pack-vpk, but takes whole directories, which the game needs for its data folders
(lang, mods, palettes, dynos).

usage: mkvpk.py <output.vpk> <source>=<path inside the vpk> [...]
       a source can be a file or a directory (added recursively)
"""

import os
import sys
import zipfile

# These are stored uncompressed: they are already compressed (or tiny)
STORED_EXTENSIONS = ('.png', '.bin', '.ogg', '.mp3', '.zip', '.sfo', '.tex', '.lvl', '.bhv', '.col')


def add_file(vpk, src, dst):
    dst = dst.replace('\\', '/')
    method = zipfile.ZIP_STORED if src.lower().endswith(STORED_EXTENSIONS) else zipfile.ZIP_DEFLATED
    vpk.write(src, dst, compress_type=method)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    out_path = sys.argv[1]
    count = 0
    with zipfile.ZipFile(out_path, 'w', zipfile.ZIP_DEFLATED) as vpk:
        for spec in sys.argv[2:]:
            if '=' not in spec:
                print('mkvpk: bad argument "%s", expected <source>=<destination>' % spec)
                return 1
            src, dst = spec.rsplit('=', 1)
            if os.path.isdir(src):
                for root, dirs, files in os.walk(src):
                    dirs.sort()
                    for name in sorted(files):
                        full = os.path.join(root, name)
                        rel = os.path.relpath(full, src)
                        add_file(vpk, full, os.path.join(dst, rel))
                        count += 1
            elif os.path.isfile(src):
                add_file(vpk, src, dst)
                count += 1
            else:
                # optional folders (dynos packs) may simply not exist
                print('mkvpk: skipping missing "%s"' % src)

    print('mkvpk: %s (%d files, %.1f MiB)' % (out_path, count, os.path.getsize(out_path) / (1024.0 * 1024.0)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
