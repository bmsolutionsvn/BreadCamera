# Working rules for Claude in BreadCamera

BreadCamera is a native Windows multi-camera GPU processing app (C++20 engine +
WebView2/React UI). The design is
`docs/superpowers/specs/2026-10-08-breadcamera-design.md`; plans live in
`docs/superpowers/plans/`.

## Planning session vs implementing agents

The main (interactive) Claude session **plans; it does not implement**. It writes
specs and plans, reviews finished work, merges branches, and answers questions. It
does not write product code, tests or build scripts. Implementation goes to Hermes
(below). An agent dispatched to implement a plan is exempt.

## Handing plans to Hermes (same flow as TYCyber, owner 2026-10-08)

- Every finished plan is queued as one task on Hermes's kanban board: board
  `default`, assignee `default`, workspace `dir:F:\Coding\BreadMedia\BreadCamera`,
  skills `superpowers-sdd-via-hermes` and `subagent-plan-execution`, created with
  `--created-by claude-planner --initial-status blocked`, then
  `hermes kanban schedule <id> "Night window: resumes at 23:00"`.
- Tasks run **only between 23:00 and 07:00**; the night cron jobs release and stop
  them. Never dispatch outside that window.
- One complete brief per task; to change a queued task, archive it and create a new
  one with the full brief and its links.
- Plans of 4 tasks or fewer run **native** (one implementer, then one whole-branch
  review and one fix wave). Plans of 5+ use **batched reviews** (a reviewer per batch
  of up to 3 tasks, then the whole-branch review).
- The milestones are a chain (each builds on the last): `--parent` on the previous
  milestone's task, each branch cut from its parent's branch, all in the main
  checkout.
- Workers never merge and never push. The planner verifies and merges each branch
  into `main`.

## Build machine facts

- GPU: **AMD Radeon RX 6600** — no NVIDIA. NVIDIA-only code (CUDA, Maxine, NVENC)
  is built when its SDK is present and stays "untested on hardware" (spec §7.2).
- Visual Studio 2026 Community (`C:\Program Files\Microsoft Visual Studio\18\Community`)
  with its bundled CMake, Ninja and vcpkg. Use a VS developer shell
  (`Launch-VsDevShell.ps1 -Arch amd64`) for every build.
- The `@bmsolutionsvn/design` npm package comes from GitHub Packages:
  `NODE_AUTH_TOKEN="$(gh auth token)" npm ci`. Node 24 for vitest/jsdom
  (`PATH="/c/Program Files/nodejs:$PATH"`).
