# Agent status records

The canonical place for OOT True Coop's engineering state, kept for Chief, the owner's chief-of-staff agent, which reads these files from GitHub. Standard: Navideus Agent Reporting Standard v1.1, https://github.com/bghill95/navideus-reporting-standard (its `STANDARD.md` is the full text; this page keeps what a reader of this repository needs). Every claim here is evidence a reader can check from this repository.

## Files

| File | Holds | How it changes |
|---|---|---|
| `STATUS.md` | One page: project and repository, Business Context, lifecycle status, phase, milestone, branch blocks with the six states, verification, blockers, next action, links, revisions, last verified, uncertainties | Replace stale lines |
| `ROADMAP.md` | Current milestone and acceptance, confirmed priorities, planned milestones, dependencies, risks, future ideas; every planned item labelled | Edit when the owner or a record changes it |
| `DECISIONS.md` | One `DEC-nnn` entry per significant decision, eight fields | Append only; a replaced entry is marked Superseded, never rewritten |
| `VERIFICATION.md` | One `VER-nnn` entry per run actually executed, ten fields, newest first; tables by branch head, of cited results and of checks not run | Append entries; replace the tables |
| `README.md` | This guide and the evidence table for this project | Edit when the standard changes |

## When to update

After a milestone, a decision, an implementation step, a test run or a review; not for trivial or redundant changes. Read `STATUS.md`, `ROADMAP.md` and `DECISIONS.md` before significant work. When a correction changes a claim, say in one line what was corrected.

## Lifecycle states

- **Implemented:** Functionality exists in code.
- **Merged:** Functionality entered the integration or default branch.
- **Tested:** Specified tests were executed with recorded results.
- **Independently Reviewed:** A separate reviewer examined the change and recorded its disposition.
- **Deployed:** Functionality was installed in a specified environment.
- **Released:** Functionality reached its intended users through the project's defined release process.

Each state is reported separately, at a named commit; none implies another, and "released" never means merged. Missing evidence is `Unknown` or `Not Verified`; a project with no deployment environment or release process writes `Not applicable:` followed by the reason for Deployed, Released and Commit deployed. `## Lifecycle status` in `STATUS.md` is a different word set (Active, Maintenance, Completed, Paused, Superseded, Archived) that describes the project, not a change.

## Evidence for this project

| State | Evidence that counts here |
|---|---|
| Implemented | a commit on `enemy-sync`, the only branch on GitHub apart from the records' own `agent-status`; this fork's own work starts at 27f6fcb, and everything before it is upstream history |
| Merged | a commit on the default branch `enemy-sync`, pushed directly (the fork's history is linear) or brought in by the merge of a pull request such as #2, named by its merge commit |
| Tested | the CI build (the `generate-builds` workflow on every push: `soh.o2r` generation, then Linux, macOS and Windows builds, with no test step), the two-instance rig checks that commit messages report, and the eight-step play-test in `COOP-TEST-GUIDE.md`; each a `VERIFICATION.md` entry or cited row naming its commit |
| Independently reviewed | no reviewer is defined; a review counts when its reviewer, the commit it covered and its disposition are recorded in `VERIFICATION.md` (the reviews that commit messages cite record neither reviewer nor commit) |
| Deployed | Not applicable: no deployment environment is defined; players build the game and run two instances locally with an Anchor relay server (root `README.md`, "Running it") |
| Released | Not applicable: no release process is defined (no tags, no GitHub releases); players build from source, CI build artifacts are not releases, and nothing is Released |

## Revision traceability

`STATUS.md` records separately the default-branch commit, the implementation commit, the commit tested, the commit reviewed, the commit deployed and the records revision (`this commit` until it exists). The default branch and each branch holding unmerged work, at most three, have a block with the actual head, an unmerged one marked not merged; every other branch is one line above the first block, giving its head, its date, and whether it is merged into the default branch or stale and not merged. The records' own `agent-status` branch never gets a block. A service is never assumed to run the latest revision.

## Verification entries

Ten fields per entry, in this order and spelling: Test name and purpose; Command or procedure; Commit tested; Environment and configuration; Execution date; Result; Evidence; Reviewer and disposition; Remaining failures, limitations or untested scenarios; Who ran it. A command, time or environment that was not kept is `not recorded`, never reconstructed. Results cited from the long-form record sit in their own table with the commit their source names; a result at one commit is never carried to a later one.

## Result words

`Passed`, `Failed`, `Blocked`, `Not Run`. Only runs actually executed are recorded; reading code is inspection, not testing.

## Roles

- Fable plans and verifies, and records the final verification disposition.
- Opus implements and updates these records, and never claims verification.
- Codex audits independently; its findings and their dispositions are recorded in `VERIFICATION.md`.

## Rules

- Evidence only: commit hashes that resolve, paths that exist, links to this repository on GitHub. Anything else is `Unknown`, or `unverified` with who reported it and when.
- Never invent states, results, decisions or priorities; priorities come from the owner, never from commit activity.
- Nothing personal or private: no credentials, hostnames, IPs, device names, email addresses or personal names (write "the owner", "the client's approver").
- No commercial documents, terms, money amounts or customer details: the Business Operations Tracker holds those.
- Printable ASCII only; timestamps in UTC as `YYYY-MM-DDTHH:MMZ`.
- No production change for reporting; nothing is merged, deployed or released without the owner.

## Business Context

A section of `STATUS.md` that classifies this repository commercially, so Chief can tell paid client work from Navideus products, Navideus internal tools and personal projects. Its fifteen lines and their allowed values are in the standard's Business Context section. Every field is `Unknown` until verified against project documentation or the owner's word; nothing is inferred from repository ownership, commit activity or the project name, and a paid project does not make a change billable. Agents never approve scope changes, promise dates or make commercial commitments.

## Reference check

The standard's `scripts/` folder (https://github.com/bghill95/navideus-reporting-standard/tree/main/scripts) checks these files from the repository root: `python -I ~/projects/navideus-reporting-standard/main/scripts/check_records.py --repo . --base origin/enemy-sync` runs every rule and prints `PASS` or the failures; `reference-check.sh <file> <ceiling>` checks one record's hashes, paths, banned strings and length. Ceilings: README 110, STATUS 90, ROADMAP 70, DECISIONS 160, VERIFICATION 110 lines. Both are inspection, not tests.

## Where older records live

- Root `README.md` (f3ff1fb, 2026-07-04): the overview, milestones M1 to M3, how it is built, how to run it, the known limitations and the roadmap
- `COOP-TEST-GUIDE.md` (f3ff1fb): the play-test guide for M2 and Gohma: launch steps, an eight-step test script, known limitations, the desync log canaries and the kill switch
- The commit messages of 27f6fcb to b738c76: the design notes and developer checks of each change
- `docs/`: upstream Ship of Harkinian documentation (building, modding, versioning, credits), not changed by this fork's commits
- GitHub: pull request #2 and issue #1, with their conversations
