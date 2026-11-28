---
name: release
description: Cut a gdfu-proxy stable release. Bump the CalVer version in platformio.ini, date the [Unreleased] changelog section, land it on dev through a PR, then open and merge the dev → master PR. Pushing to master publishes the GitHub release. Use when the user says "release", "/release", "cut/ship/publish a release", "bump the version", or "promote dev to master".
---

# Release

Merging into `master` publishes firmware to users: the push makes CI build every board and create the `v<version>` GitHub release. Stop and report back if anything below looks wrong. Don't work around it.

## 1. Preflight

```bash
git fetch origin
git log --oneline origin/master..origin/dev   # must not be empty
grep -n 'PROXY_RELEASE_VERSION' platformio.ini
```

- If `dev` has nothing new over `master`, there is nothing to release. Stop.
- `CHANGELOG.md` on `origin/dev` needs a non-empty `## [Unreleased]` section. The release body comes from it, and CI fails without it.

## 2. Compute the next version

The version is CalVer `YYYY.M.K`: month without a leading zero, K counts releases within that month. Today is `date +%Y.%-m`.

- If the current version is `X-rcN`, release `X`.
- If its year and month match today's, increment K. `2026.11.1` becomes `2026.11.2`.
- If the month or year changed, use today's year and month with K = 1. `2026.11.4` in December becomes `2026.12.1`. `2026.12.3` in January 2027 becomes `2027.1.1`.

Make sure no `v<new>` tag exists yet: `git ls-remote --tags origin "v<new>"`.

## 3. Release branch from dev

```bash
git worktree add -b release/<new> ../gdfu-proxy-release-<new> origin/dev
```

Inside the worktree:

- `platformio.ini`: set `-DPROXY_RELEASE_VERSION=\"<new>\"`. It must appear exactly once.
- `CHANGELOG.md`: rename `## [Unreleased]` to `## [<new>] - <date +%F>`. Leave the entries alone.
- Check: `python3 scripts/build-version.py` prints `<new>-dirty`, which is dirty only because nothing is committed yet. Also check that `python3 scripts/release-notes.py <new> --notes-file /tmp/notes.md` exits 0.
- Commit `Release <new>` and push: `git push -u origin release/<new>`.

## 4. PR into dev, then merge it

Open the PR `release/<new>` → `dev`, titled `Release <new>`. Wait for its checks to pass, then squash-merge it, like other dev PRs. Delete the worktree and the branch afterwards.

## 5. PR dev → master, then merge it

Open the PR `dev` → `master`, titled `Release <new>`, with the new changelog section as the body. Wait for checks to pass, then merge with a merge commit. Don't squash or rebase. A merge commit keeps `master` an ancestor of `dev`, so later release PRs stay conflict-free.

## 6. Report

Give the version, both PR links, and the CI run for the `master` push that publishes the release. Don't claim the release is published until that run succeeds.
