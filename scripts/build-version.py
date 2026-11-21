#!/usr/bin/env python3
"""Derive the firmware build version from the committed PROXY_RELEASE_VERSION."""

import argparse
from pathlib import Path
import re
import subprocess

VERSION = r"[0-9]{4}\.[0-9]+\.[0-9]+(-rc[0-9]+)?"

def build_version(root: Path, version: str) -> str:
    """Release candidates get the short commit hash; dirty trees get -dirty."""
    match = re.fullmatch(VERSION, version)
    if not match:
        raise ValueError(f"Invalid version: {version} (expected YYYY.M.K or YYYY.M.K-rcN)")

    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()

    if match[1]:
        version += f"-{git('rev-parse', '--short', 'HEAD')}"
    if git('status', '--porcelain', '--untracked-files=normal'):
        version += '-dirty'
    return version

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('version', nargs='?')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        version = args.version
        if version is None:
            stamps = re.findall(r'-DPROXY_RELEASE_VERSION=\\"([^\\"]+)\\"', (root / 'platformio.ini').read_text())
            if len(stamps) != 1:
                raise ValueError('Expected one PROXY_RELEASE_VERSION in platformio.ini')
            version = stamps[0]
        print(build_version(root, version))
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'build-version: {error}\n')

if __name__ == '__main__':
    main()
