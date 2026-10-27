# Working rules for Claude in BreadCamera

BreadCamera is a native Windows multi-camera GPU processing app (C++20 engine +
WebView2/React UI). The design is
`docs/superpowers/specs/2026-10-08-breadcamera-design.md`; plans live in
`docs/superpowers/plans/`.

## Planning session vs implementing agents

The main (interactive) Claude session **plans; it does not implement**. It writes
specs and plans, reviews finished work, and answers questions. It
does not write product code, tests or build scripts. Implementation goes to OpenCode
(below). An agent dispatched to implement a plan is exempt.

## Handing plans to OpenCode (owner 2026-10-11; aider and Hermes are retired for BreadCamera)

- Remaining work is in `docs/superpowers/plans/2026-10-11-remaining-milestones-opencode.md`: the task order, branch chain, verification command and stop conditions. OpenCode runs it, one task per commit.
- **Merging (owner 2026-10-27):** OpenCode merges its own branch into `main` (`git merge --no-ff`) when it is green: the full verification command and `ui/` checks pass and no Critical/Important review finding is open. Anything red: no merge, stop and report. Nobody pushes unless the owner asks. This replaces the "do not merge" and "the owner merges" lines in older plans, including `2026-10-11-remaining-milestones-opencode.md`.
- The planner (this session) reviews the merged result on `main` and writes a fix prompt for any finding; it reverts a merge only when the owner asks.
- **No extra git worktrees** (owner 2026-10-27): OpenCode and reviews work in this checkout, one plan at a time. Extra worktrees strand uncommitted work that is lost when they are removed. Never remove an existing worktree that has uncommitted changes or unmerged commits.
- Acceptance tasks and the human gates listed in that plan are the owner's to judge.

## Build machine facts

- GPU: **AMD Radeon RX 6600** — no NVIDIA. NVIDIA-only code (CUDA, Maxine, NVENC)
  is built when its SDK is present and stays "untested on hardware" (spec §7.2).
- Visual Studio 2026 Community (`C:\Program Files\Microsoft Visual Studio\18\Community`)
  with its bundled CMake, Ninja and vcpkg. Use a VS developer shell
  (`Launch-VsDevShell.ps1 -Arch amd64`) for every build.
- The `@bmsolutionsvn/design` npm package comes from GitHub Packages:
  `NODE_AUTH_TOKEN="$(gh auth token)" npm ci`. Node 24 for vitest/jsdom
  (`PATH="/c/Program Files/nodejs:$PATH"`).
