#!/usr/bin/env python3
"""Validate a release version and extract its changelog section."""

import argparse
from pathlib import Path
import re

def sections(changelog, section):
    matches = []
    for entry in re.split(r"(?m)^##[ \t]+", changelog)[1:]:
        heading, _, body = entry.partition("\n")
        name = re.match(r"\[([^\]]+)\]|(\S+)", heading)
        if name and (name[1] or name[2]).casefold() == section:
            matches.append(body.strip())
    return matches

def release_notes(changelog, version):
    match = re.fullmatch(r"([0-9]{4}\.[0-9]+\.[0-9]+)(-rc[0-9]+-[0-9a-f]{7,40})?", version)
    if not match:
        raise ValueError(f"Invalid release version: {version}")

    # Release candidates use [Unreleased]; stable prefers [version], falls back to [Unreleased].
    section = "unreleased" if match[2] else version
    matches = sections(changelog, section)
    if not matches:
        section = "unreleased"
        matches = sections(changelog, section)

    if len(matches) != 1 or not matches[0]:
        raise ValueError(f"CHANGELOG.md must contain one non-empty [{section}] section")
    return matches[0] + "\n"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--notes-file", type=Path, default=Path("release-notes.md"))
    args = parser.parse_args()
    try:
        notes = release_notes(args.changelog.read_text(encoding="utf-8"), args.version)
    except ValueError as error:
        parser.error(str(error))
    args.notes_file.write_text(notes, encoding="utf-8")

if __name__ == "__main__":
    main()
