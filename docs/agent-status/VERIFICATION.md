# OOT True Coop verification log

Result words: Passed, Failed, Blocked, Not Run. Only runs actually executed are entries, newest first; reading code is inspection, not testing. A result at one commit is never carried to a later one, and a command, time or environment that was not kept is `not recorded`, never reconstructed.

## Verification by branch head
Current state only; each cell names the commit its result is at.

| Branch | Head | Unit tests | CI build and developer checks | Independent review |
|---|---|---|---|---|
| enemy-sync | b738c76 | Not Run: no unit test suite is registered in the repository's CMake files (inspection) | CI build Failed at b738c76 before any build job ran; a rig check Passed at b738c76, cited from its commit message; neither re-run | Not Verified |

## VER-001: Records check
- Test name and purpose: Records check: the standard's checker on these records, for their form, hashes, paths and banned strings.
- Command or procedure: `python -I ~/projects/navideus-reporting-standard/main/scripts/check_records.py --repo .`, from the worktree root
- Commit tested: the files of the commit that carries this entry, uncommitted on b738c76
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
| CI build: `generate-builds` workflow (`soh.o2r` generation, then Linux, macOS and Windows builds; no test step) | 2026-07-12 | b738c76 | GitHub Actions, push to `enemy-sync` | Failed: job `generate-soh-otr` failed at "Install latest SDL"; the three build jobs were skipped | the run for b738c76 at https://github.com/bghill95/OOT-True-Co-op/actions/workflows/generate-builds.yml?query=branch%3Aenemy-sync |
| Rig check of the Deku Baba adapter: zero-input boot and packet injector on two instances (identical 9-enemy rosters, an injected hit applied on the authority, one permanent death message, zero desync canaries) | 2026-07-12 | b738c76 | not recorded (a two-instance rig, not in this repository) | Passed | the commit message of b738c76 |
| Music seek: the skip path advances the script counter in the unit it reports | 2026-07-05 | faf0cf2 | not recorded | Passed | the commit message of faf0cf2 ("verified end-to-end") |
| CI build: `generate-builds` workflow | 2026-07-04 | f3ff1fb | GitHub Actions, push to `enemy-sync` | Passed: all four jobs | the run for f3ff1fb at the same link |
| Rig and packet-injector check after the seven review fixes | 2026-07-04 | 0599601 | not recorded (the two-instance rig and the packet injector) | Passed | the commit message of 0599601 |
| Rig check of M2 and M3: an identical 9-enemy Deku Tree roster streamed and suppressed with zero desync canaries; the Gohma arena streamed at full rate | 2026-07-04 | ec103b7 | not recorded (a two-instance rig) | Passed | the commit message of ec103b7 |

Commit messages also cite reviews whose records are not in this repository: a multi-agent review of M2 and M3 whose seven findings 0599601 fixes (its findings file, `enemy-sync-review-findings.md`, is absent), a review of an earlier task carried into f1c05e2, and a final review whose two Important and one Minor findings b906a25 fixes. None names its reviewer or the commit it covered, and none is an Independently reviewed state for b738c76. a199cca records a check of its boss table against the game's entrance table: inspection, not a test.

## Not Run
| Check | Result | What it needs |
|---|---|---|
| CI build of b738c76 through the build jobs | Not Run | a `generate-builds` run that gets past "Install latest SDL" |
| Play-test script, `COOP-TEST-GUIDE.md` (eight steps: mirroring, Gohma, input isolation) | Not Run | two instances on one PC with two controllers, a legally dumped copy of the game, an Anchor relay server, and the launcher and instance folders the guide names, which are not in the repository |
| Rig checks at b738c76, run by an agent | Not Run | the two-instance rig and the packet injector, which are not in the repository |
| `clang-format` workflow on `enemy-sync` | Not Run | it runs on pull requests only (`.github/workflows/clang-format.yml`) |
| `test-builds-on-distros` workflow (Linux distribution builds) | Not Run | a manual dispatch (`.github/workflows/test-builds-on-distros.yml`); no run exists |
| Independent review of 27f6fcb to b738c76 | Not Run | a reviewer separate from the implementer and a recorded disposition |
