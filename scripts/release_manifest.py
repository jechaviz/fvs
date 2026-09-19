#!/usr/bin/env python3
from __future__ import annotations
import argparse, hashlib, json, os, pathlib, sys

EXCLUDED_DIRS = {'.git', '.idea', '.vscode', '__pycache__', 'build', '.pytest_cache', '.mypy_cache'}
EXCLUDED_FILES = {'RELEASE_MANIFEST.json', 'MANIFEST.sha256'}
EXCLUDED_SUFFIXES = {'.pyc', '.pyo', '.plist', '.zip', '.diff'}


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def iter_release_files(root: pathlib.Path):
    for path in sorted(root.rglob('*')):
        if not path.is_file():
            continue
        rel = path.relative_to(root)
        if any(part in EXCLUDED_DIRS for part in rel.parts):
            continue
        if rel.name in EXCLUDED_FILES or rel.suffix in EXCLUDED_SUFFIXES:
            continue
        yield rel, path


def build_manifest(root: pathlib.Path, version: str) -> dict:
    files = {rel.as_posix(): sha256_file(path) for rel, path in iter_release_files(root)}
    canonical = ''.join(f'{name}\0{digest}\n' for name, digest in files.items()).encode('utf-8')
    tree = hashlib.sha256(canonical).hexdigest()
    return {
        'format': 'fvs-release-manifest-v1',
        'version': version,
        'algorithm': 'sha256',
        'file_count': len(files),
        'tree_sha256': tree,
        'files': files,
    }


def verify(root: pathlib.Path, manifest_path: pathlib.Path) -> tuple[bool, list[str], dict]:
    problems: list[str] = []
    try:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    except Exception as e:
        return False, [f'manifest_read:{type(e).__name__}'], {}
    if manifest.get('format') != 'fvs-release-manifest-v1':
        problems.append('manifest_format')
    expected = manifest.get('files')
    if not isinstance(expected, dict):
        return False, problems + ['manifest_files'], manifest
    actual = {rel.as_posix(): sha256_file(path) for rel, path in iter_release_files(root)}
    expected_names, actual_names = set(expected), set(actual)
    for name in sorted(expected_names - actual_names):
        problems.append(f'missing:{name}')
    for name in sorted(actual_names - expected_names):
        problems.append(f'unexpected:{name}')
    for name in sorted(expected_names & actual_names):
        if actual[name].lower() != str(expected[name]).lower():
            problems.append(f'hash:{name}')
    canonical = ''.join(f'{name}\0{actual[name]}\n' for name in sorted(actual)).encode('utf-8')
    tree = hashlib.sha256(canonical).hexdigest()
    if tree.lower() != str(manifest.get('tree_sha256', '')).lower():
        problems.append('tree_sha256')
    if int(manifest.get('file_count', -1)) != len(actual):
        problems.append('file_count')
    return not problems, problems, manifest


def main() -> int:
    ap = argparse.ArgumentParser(description='Generate or verify the deterministic FVS release manifest')
    ap.add_argument('--root', type=pathlib.Path, default=pathlib.Path('.'))
    ap.add_argument('--output', type=pathlib.Path, default=pathlib.Path('RELEASE_MANIFEST.json'))
    ap.add_argument('--version', default='6.0.0')
    ap.add_argument('--verify', action='store_true')
    args = ap.parse_args()
    root = args.root.resolve()
    output = args.output if args.output.is_absolute() else root / args.output
    if args.verify:
        ok, problems, manifest = verify(root, output)
        print(json.dumps({'ok': ok, 'version': manifest.get('version'), 'problems': problems}, separators=(',', ':')))
        return 0 if ok else 2
    manifest = build_manifest(root, args.version)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print(json.dumps({'ok': True, 'version': args.version, 'file_count': manifest['file_count'], 'tree_sha256': manifest['tree_sha256']}, separators=(',', ':')))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
