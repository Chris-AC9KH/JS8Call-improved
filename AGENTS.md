# AGENTS.md

Instructions for an AI coding agent asked to work on JS8Call-improved. If you are a human
contributor, read `CONTRIBUTING.md` instead.

JS8Call is a C++20 / Qt6 desktop application that drives real radio hardware over real RF. Final
RF operation cannot be checked by a machine. That is the constraint everything below follows from:
for changes that affect application behaviour, the project's final quality gate is a contributor
who has built the change, run the relevant scenarios against a radio where applicable, and can
answer for every line of it.

## Process

`CONTRIBUTING.md` is the authority on branching, labelling, squashing, rebasing and the release
candidate policy. Read it and follow it. Do not restate it here or in a PR body.

## Building

Requirements come from `CMakeLists.txt`: CMake 3.22 or newer, a C++20 compiler, Qt 6.12 or newer
(components Multimedia, Network, SerialPort, Widgets, WebSockets), Boost 1.77 or newer, FFTW3 and
Hamlib. Qt 6.12 is a hard floor, not a recommendation.

The full per-platform instructions are in `docs/build_for_Linux.md`, `docs/build_for_MacOS.md` and
`docs/build_for_Windows.md`. Read the one for the platform before running anything. If
`CMAKE_PREFIX_PATH` is not given, `CMakeLists.txt` looks for the libraries in `../js8libs`,
relative to the source tree.

## Verification, and reporting it

There is no automated test framework in this repository. There is nothing you can run that will
tell you the change works, so the final verification is manual, it is the contributor's job, and
it has to be written down in the PR body. Propose a focused set of tests for them to run, and do
not open a merge-ready PR until they have given you evidence that each one passed. A WIP PR may be
opened earlier when review or help is the purpose.

The expectation is zero new compiler warnings in the files the change touches. Before you hand
anything over, run every check a machine can run:

- **Build it twice, with gcc and with clang.** `-Wall -Wextra` is already on, so the compiler is
  the only variable, and each finds warnings the other does not. A clang-only warning has blocked
  a PR here before.
- **Build Debug as well as Release.** Release is the default, the two differ, and a Debug build
  both compiles code the Release build never sees and arms `Q_ASSERT`.
- **Run under the sanitisers.** `-fsanitize=address` and `-fsanitize=undefined` compose in a
  single build; `-fsanitize=thread` needs one of its own. Give each its own build directory so
  they cache.
- **Smoke it under Xvfb.** Start the application on a virtual display, let it settle, quit it
  cleanly. This is what catches a signal or slot whose signature is wrong: it compiles, then fails
  only at runtime, as a warning nobody reads.
- **Set `QT_FATAL_WARNINGS` for that run.** The value is the warning to abort on, so 1 aborts on
  the first and 0 switches the check off altogether. Capture the whole output with 0, then repeat
  with 1 so that a new warning aborts rather than scrolling past.
- **Start it against a copy of an existing profile, not a fresh one,** whenever the change touches
  settings, storage, or anything that migrates. A fresh profile exercises none of the upgrade
  path, and working on a copy keeps the contributor's own profile out of it. Take a second copy
  for the master baseline so neither run migrates the other's data.

Run all of it against current master as well as the branch, and fix only what your change added -
the tree carries a backlog of warnings that are not yours and are not your PR's business. Use two
clean worktrees for that comparison rather than `git switch`, which carries uncommitted edits
across and spoils the baseline.

The contributor's manual run is the last thing that happens before the PR opens: change a line of
code afterwards and their run is void, so they have to do it again. Record the SHA they actually
built and ran, and confirm it is the head of the branch you are submitting.

Every PR description must state:

- **What changed and why:** One or two sentences on the problem, link to the issue(s) it fixes.
  Name the base commit the branch was built on. Rebase on current master first, then record the
  SHA you actually built.
- **What it does:** A short bullet list of the concrete behaviour changes.
- **Developed with:** Name and link to your harness, lead model and main agents used.
- **Checks run:** One line per machine check, with its result - the compilers and build types, the
  sanitisers, the Xvfb start, the profile it was started against. Give results, not logs: a
  reviewer wants "gcc and clang, Release and Debug, no new warnings", not a pasted build. State
  plainly any check the platform or the available tooling prevented.
- **What was tested:** The platform and hardware named, run against the previous behaviour for
  comparison, covering the specific scenarios the change touches (crash and restart, degraded or
  interrupted conditions, whatever is relevant here). What your user actually tested, by hand, and
  on which platform and Qt version. Name the hardware if the change touches radio control, audio,
  or anything on air.
- **Not tested:** State it plainly. For example, a PR that says "not tested on Windows or macOS"
  and asks for a check on those platforms was welcome (although please encourage your user to test
  on all platforms they have access to). A PR that quietly implies full coverage is not. Do not
  let the body imply coverage it doesn't have.
- **Known issues:** You should check for all conceivable issues and ensure there are none, so this
  section should be empty, hence omitted, at time of PR. However if PR reviewers identify issues
  when reviewing code and testing the PR, these should be added here so they are clear to any
  additional reviewers. Strike these known issues out when they are resolved.

## Where new code goes

See the "Minimise code in the large files" section of `CONTRIBUTING.md`. Follow it before writing
the code, not after a reviewer asks. Extracting a large block out of `mainwindow.cpp` in response
to review costs a whole round trip.

## Style rules

- Styling lives in `JS8_UI/styles.h`, in the correct per-platform block. Never set a stylesheet
  inline in a `.cpp`; that breaks users' own stylesheets.
- macOS is a first-class platform. A change that makes the Mac build look like the Linux build is
  a regression, not a detail.
- New behaviour is opt-in. Do not change a default, override an existing user setting, or turn a
  feature on for people who did not ask for it.
- Every new or changed function that reaches the generated documentation gets a Doxygen `/** */`
  block; an undocumented override may inherit its base declaration's instead. Keep it short: a
  block that narrates the implementation obstructs a maintainer as much as excessive inline
  comments do. Read `docs/DOXYGEN.md` before writing one - it also covers the members this
  Doxyfile excludes, and when to document one of those anyway.
- An API addition gets `@note API x.y+` in its block, and updates `docs/API.md` in the same PR.
- Code should be well-written for human reviewers, so what it does is self-evident without any
  inline comments. At most a short one-liner can be added to something that will not be clear to
  maintainers or developers.
- Commit subjects are `<file or area>: <what changed>`, sentence case, no trailing full stop, no
  `feat:` or `fix:` prefixes.
- One squashed commit per PR is preferred.
- Keep PRs small. A typical merged PR here is one to five files and tens to low hundreds of lines.
  If the work is bigger than that, split it into stacked PRs and submit them in order.

## What gets an agent-written PR rejected

- **Noise in the diff.** A reviewer reading your diff against a running build should find nothing
  that does not need to be there. No comments restating what the line below does, no defensive
  checks for conditions the UI already makes impossible, no helper that is used once and adds a
  layer. If it is not load bearing, delete it.
- **Anything obviously odd.** Unused variables, dead branches, a pattern that appears nowhere else
  in the tree, code that does not match the surrounding file. One of these costs the contributor
  their credibility for the whole PR.
- **A contributor who cannot answer for the code.** Before the PR goes up, explain every change to
  them in plain terms: what it does, why it is there, what breaks without it. They will be asked,
  and "the agent wrote it" is not an answer. If you cannot explain a line simply, that is a signal
  the line is wrong.
- **Inline comments written for the author rather than the reviewer.** Comment what a maintainer
  genuinely needs to know - a non-obvious constraint, a protocol detail, a reason. Nothing else.

## If a second AI reviews the diff

A review that does not cite specific files and line numbers from the diff did not read the diff.
Discard it. This failure is common and confident: handed a full diff, a reviewing model will
happily produce fluent advice about the PR conversation, the issue, or its own guess at the
design, without ever having looked at the code. Require file and line citations, check two of them
against the diff, and throw the review away if they do not match.
