# Contributing

## Branches

`staging` is the testing layer between a change and `main`. Everything lands in `staging` first, gets tested there, and reaches `main` only when `staging` is merged into it. Every game built on the engine follows `main`, so nothing reaches them untested.

| Branch | Holds | Accepts |
|--------|-------|---------|
| `staging` | Changes being tested | Pull requests from any branch, once CI passes and a maintainer approves |
| `main` | Tested, released code | Pull requests from `staging` only, once CI passes and a maintainer approves |

Open every pull request against `staging`. A pull request into `main` from any other branch fails the **Only staging merges into main** check and cannot be merged. To fix it, edit the pull request and change its base branch to `staging`.

---

## Testing in staging

Every pull request into `staging` runs the full CI: the format check, the Linux and Windows build matrix, the shader check, and the downstream game build.

The downstream game build uses the SleakCraft branch of the same tier: pull requests into `staging` build SleakCraft `staging`, and pull requests into `main` build SleakCraft `main`. SleakCraft's own CI pairs the same way, so a change that spans both repos goes into both `staging` branches and is tested together.

---

## Releasing staging to main

1. Open a pull request from `staging` into `main`.
2. Wait for CI to pass.
3. Merge it with **Create a merge commit**.
4. When SleakCraft has matching changes, release its `staging` right after, so the game's `main` builds against the engine's `main` again.

`main` only allows merge commits. Squash and rebase copy staging's commits into new ones, so the two branches would stop sharing history, and every later release would replay old commits and hit conflicts that aren't real.

After a release, GitHub shows `staging` as behind `main`. The commits it is missing are the release merge commits, which change nothing on their own and need no action.

---

## Branch protection

The rules are the JSON files in `.github/rulesets/`, which GitHub imports as they are: **Settings → Rules → Rulesets → New ruleset → Import a ruleset**.

| Rule | `main` | `staging` |
|------|--------|-----------|
| Changes arrive by pull request only | ✓ | ✓ |
| No force pushes, no deletion | ✓ | ✓ |
| CI must pass | ✓ | ✓ |
| Review threads resolved | ✓ | ✓ |
| A maintainer approves | ✓ | ✓ |
| Source must be `staging` | ✓ | |
| Merge method | Merge commit | Any |

> **Setting it up?** Import `main.json` only after `.github/workflows/branch-policy.yml` is on `main`. The ruleset requires the check that workflow reports, so until the workflow exists nothing can merge into `main`. **Allow merge commits** must stay on under Settings → General → Pull Requests.

The approval rule lives in its own ruleset, `approval.json`, because GitHub does not let authors approve their own pull requests. Repository admins can bypass that one ruleset, and only through a pull request, so a maintainer can merge their own work once CI passes. Everyone else needs a maintainer's approval, and a new push dismisses an earlier approval.

Nobody can bypass the other two rulesets, admins included. To get out of a jam, set the ruleset's enforcement to **Disabled**, fix the problem, then set it back to **Active**.

Required checks are matched by name, so renaming a job in `ci.yml` or `branch-policy.yml` means updating the rulesets to match.
