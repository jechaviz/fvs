#!/usr/bin/env python3
from __future__ import annotations
import argparse, json, pathlib, stat, sys, zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import release_manifest

FIXED_DT = (1980, 1, 1, 0, 0, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description='Build a deterministic FVS release ZIP from RELEASE_MANIFEST.json')
    ap.add_argument('--root', type=pathlib.Path, default=pathlib.Path('.'))
    ap.add_argument('--manifest', default='RELEASE_MANIFEST.json')
    ap.add_argument('--output', type=pathlib.Path, required=True)
    args = ap.parse_args()
    root = args.root.resolve()
    manifest_path = root / args.manifest
    ok, problems, manifest = release_manifest.verify(root, manifest_path)
    if not ok:
        print(json.dumps({'ok': False, 'problems': problems}, separators=(',', ':')), file=sys.stderr)
        return 2
    names = sorted(manifest['files']) + [args.manifest]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for name in names:
            path = root / name
            data = path.read_bytes()
            zi = zipfile.ZipInfo(name, FIXED_DT)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 3
            mode = path.stat().st_mode
            perms = 0o755 if (mode & stat.S_IXUSR) else 0o644
            zi.external_attr = (stat.S_IFREG | perms) << 16
            zi.flag_bits |= 0x800
            zf.writestr(zi, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    print(json.dumps({'ok': True, 'output': str(args.output), 'entries': len(names), 'tree_sha256': manifest['tree_sha256']}, separators=(',', ':')))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
