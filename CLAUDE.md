# CLAUDE.md

## Changelog

Always update `CHANGELOG.md` with the latest changes **before every commit** (including amends). Add them under `## [Unreleased]` in the matching `### Added` / `### Changed` / `### Fixed` / `### Removed` section. The release workflow publishes the changelog section as the GitHub release body, so an empty entry fails the build (`scripts/release-notes.py`). Keep entries short and user-facing.

The changelog is for people flashing and running the proxy, not a git log: only record changes a user would notice (boards, BLE/radio behaviour, OLED, release downloads, fixes). Never add KTLO or CI entries (pipelines, build/release scripts, tooling, refactors, dependency bumps, repo housekeeping). Merge related changes into one entry rather than listing each tweak. If a commit has no user-facing change, don't add an entry.

## Git

- Treat `master` as protected: never commit, push or merge to it directly.
- Start all new work in a git worktree branched from `dev`.
- Land every commit on `dev` through a PR; never commit directly to `dev`.
- Workflow: worktree branch from `dev` → PR → `dev` → PR → `master`.
- Open a PR to `master` only when the user asks for one.
