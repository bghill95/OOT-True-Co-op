# OOT True Coop verification log

Result words: Passed, Failed, Blocked, Not Run. Only runs actually executed are entries, newest first; reading code is inspection, not testing. A result at one commit is never carried to a later one, and a command, time or environment that was not kept is `not recorded`, never reconstructed.

## Verification by branch head
Current state only; each cell names the commit its result is at.

| Branch | Head | Unit tests | CI build and developer checks | Independent review |
|---|---|---|---|---|
| enemy-sync | b738c76 | Not Run: no unit test suite is registered in the repository's CMake files (inspection) | CI build Failed at b738c76 before any build job ran; a rig check Passed at b738c76, cited from its commit message; neither re-run | Not Verified |

## VER-002: Fable's verification of these records
- Test name and purpose: Fable's verification of these records against the repository: the rollout verification checklist (checker, hashes and dates, lifecycle claims, Business Context values, pilot leakage, roadmap labels, ceilings, diff scope, the PR).
- Command or procedure: `python -I ~/projects/navideus-reporting-standard/main/scripts/check_records.py --repo . --base origin/enemy-sync` with the banned-words variable set at run time, then `git log -1` on every cited hash, `git branch -r`, `gh pr list --state all`, `gh issue list --state all`, and a read of every cited document.
- Commit tested: 14490f6f3 (the records as they stood before this entry's commit; later record edits are outside this disposition)
- Environment and configuration: the owner's PC, Git Bash, Python 3.11, git 2.52, gh 2.86; the checklist was run by a read-only verification subagent on Fable's behalf, and Fable read its report.
- Execution date: 2026-10-26T07:49Z
- Result: Passed after fix round 1 (the lifecycle observations dated in the seed line, the CI failure's cause marked unrecorded, the review scope and branch wording corrected, the VER-001 snapshot named, in 14490f6; the PR body's Merge section now names the workflows that pushes start)
- Evidence: checker: PASS with the banned-words variable set to the 223 name tokens from the 168 author and committer names and the documents, no warning (the verification seat's run), and PASS again by the implementer at 14490f6; a controller run with an uncurated author-list word set produced only common-word collisions from the 168-author list, none a name in the records; all 14 cited hashes resolve with the dates and subjects recorded and sit on origin/enemy-sync; the README roadmap lines were committed by the owner in this fork and state a plan, so their [approved by record] labels stand; the CI run at b738c76 (failure at a build-environment step, log expired) and the rig result in the b738c76 message were confirmed; the fix (14490f6) was read back by diff: ten lines across STATUS.md, VERIFICATION.md and docs/agent-status/README.md, nothing else; the verification report was returned to the controller and is not a file
- Reviewer and disposition: Fable; disposition: the records are verified as of 14490f6; no Codex audit is scheduled for this repository; the runs the records push started are not cited (controller ruling)
- Remaining failures, limitations or untested scenarios: no build or test was run by the rollout; the first CI build of this code past the SDL step fails on Linux at the records push (not credited to b738c76, uncited); the lifecycle word, PR #2, issue #1 and the local-only plan and rig files await the owner's answers in the PR
- Who ran it: a read-only verification subagent on Fable's behalf (the checklist and the checker); Fable (the read of its report and this entry)

## VER-001: Records check
- Test name and purpose: Records check: the standard's checker on these records, for their form, hashes, paths and banned strings.
- Command or procedure: `python -I ~/projects/navideus-reporting-standard/main/scripts/check_records.py --repo .`, from the worktree root
- Commit tested: a draft of the files committed as 0724082, run uncommitted on b738c76
- Environment and configuration: the `OOT-True-Co-op` worktree on branch `agent-status`, Git Bash on Windows 11, Python 3.11.15, git 2.52.0.windows.1
- Execution date: 2026-10-26T06:55Z
- Result: Passed
- Evidence: exit 0: `PASS`, then `WARN AGENTS.md, CLAUDE.md, README.md: R3 not checked without --base`. The same command with `AGENT_STATUS_BANNED_WORDS` set at run time to the owner's first and last names (not written here) printed the same two lines.
- Reviewer and disposition: none; Fable's verification follows
- Remaining failures, limitations or untested scenarios: the checker checks form, references and banned strings, not whether a claim is true; this run is without `--base`, so it skips R16 and R3 on AGENTS.md, CLAUDE.md and README.md, which the `--base` run after the commit covers (reported with the draft PR).
- Who ran it: Opus

## Cited results (not re-run)
Each row is taken from the source named in Evidence. Date is the run's UTC date for the CI rows and the source commit's UTC date for the others; no command, rig or output of the developer checks is in the repository.

| Check | Date | Commit | Environment | Result | Evidence |
|---|---|---|---|---|---|
| CI build: `generate-builds` workflow (`soh.o2r` generation, then Linux, macOS and Windows builds; no test step) | 2026-07-12 | b738c76 | GitHub Actions, push to `enemy-sync` | Failed: job `generate-soh-otr` failed at its "Install latest SDL" step, a build-environment step (by the workflow file it downloads, configures and builds SDL 2.30.3 and does not use this project's source); the run's only error annotation reads "Process completed with exit code 2." and its log has expired (HTTP 410, checked 2026-10-26); the three build jobs were skipped | the run for b738c76 at https://github.com/bghill95/OOT-True-Co-op/actions/workflows/generate-builds.yml?query=branch%3Aenemy-sync |
| Rig check of the Deku Baba adapter: zero-input boot and packet injector on two instances (identical 9-enemy rosters, an injected hit applied on the authority, one permanent death message, zero desync canaries) | 2026-07-12 | b738c76 | not recorded (a two-instance rig, not in this repository) | Passed | the commit message of b738c76 |
| Music seek: the skip path advances the script counter in the unit it reports | 2026-07-05 | faf0cf2 | not recorded | Passed | the commit message of faf0cf2 ("verified end-to-end") |
| CI build: `generate-builds` workflow | 2026-07-04 | f3ff1fb | GitHub Actions, push to `enemy-sync` | Passed: all four jobs | the run for f3ff1fb at the same link |
| Rig and packet-injector check after the seven review fixes | 2026-07-04 | 0599601 | not recorded (the two-instance rig and the packet injector) | Passed | the commit message of 0599601 |
| Rig check of M2 and M3: an identical 9-enemy Deku Tree roster streamed and suppressed with zero desync canaries; the Gohma arena streamed at full rate | 2026-07-04 | ec103b7 | not recorded (a two-instance rig) | Passed | the commit message of ec103b7 |

Commit messages also cite reviews whose records are not in this repository: a multi-agent review (its scope is not stated) whose seven findings 0599601 fixes (its findings file, `enemy-sync-review-findings.md`, is absent), a review of an earlier task carried into f1c05e2, and a final review whose two Important and one Minor findings b906a25 fixes. None names its reviewer or the commit it covered, and none is an Independently reviewed state for b738c76. a199cca records a check of its boss table against the game's entrance table: inspection, not a test.

## Not Run
| Check | Result | What it needs |
|---|---|---|
| CI build of b738c76 through the build jobs | Not Run | a `generate-builds` run that gets past "Install latest SDL" |
| Play-test script, `COOP-TEST-GUIDE.md` (eight steps: mirroring, Gohma, input isolation) | Not Run | two instances on one PC with two controllers, a legally dumped copy of the game, an Anchor relay server, and the launcher and instance folders the guide names, which are not in the repository |
| Rig checks at b738c76, run by an agent | Not Run | the two-instance rig and the packet injector, which are not in the repository |
| `clang-format` workflow on `enemy-sync` | Not Run | it runs on pull requests only (`.github/workflows/clang-format.yml`) |
| `test-builds-on-distros` workflow (Linux distribution builds) | Not Run | a manual dispatch (`.github/workflows/test-builds-on-distros.yml`); no run exists |
| Independent review of 27f6fcb to b738c76 | Not Run | a reviewer separate from the implementer and a recorded disposition |
