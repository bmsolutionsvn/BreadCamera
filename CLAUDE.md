# Working rules for Claude in BreadCamera

BreadCamera is a native Windows multi-camera GPU processing app (C++20 engine +
WebView2/React UI). The design is
`docs/superpowers/specs/2026-10-08-breadcamera-design.md`; plans live in
`docs/superpowers/plans/`.

## Planning session vs implementing agents

The main (interactive) Claude session **plans; it does not implement**. It writes
specs and plans, reviews finished work, merges branches, and answers questions. It
does not write product code, tests or build scripts. Implementation goes to aider
(below). An agent dispatched to implement a plan is exempt.

## Handing plans to aider (owner 2026-10-10; replaces Hermes, which cost too many tokens)

- Skill `handing-plans-to-aider`, model `openai/mimo-v2.6-flash`. The Windows
  scheduled task `aider-night-breadcamera` runs `aider_night.py` daily at 23:00 with
  `--until 06:45` on the queue `.superpowers/aider/queue.json` (gitignored, with the
  project brief `.superpowers/aider/brief.md` and `night.log`).
- Runs happen **only between 23:00 and 07:00**. Never start a run outside that window.
- The queue holds one entry per milestone, in chain order, each branch cut from its
  parent's branch (`bcam-m2` from `bcam-m1`, …), all in the main checkout. One entry
  runs per night; an entry whose ledger shows `## DONE` waits for the planner.
- Acceptance and verification-only tasks are left out of the queue's task ranges; the
  planner runs them.
- The planner reviews each finished entry (a reviewer per batch of up to 3 tasks, then
  the whole-branch review), fixes findings with `aider_task.py --extra`, merges into
  `main`, then sets `"reviewed": true` so the next entry runs.
- Night checks leave out tests that can't pass on this PC: acceptance cases, and the 4
  socket tests whose ports (44620–48715) WSL mirrored networking reserves.
- aider never commits, merges or pushes; `aider_run.py` commits each green task.

## Build machine facts

- GPU: **AMD Radeon RX 6600** — no NVIDIA. NVIDIA-only code (CUDA, Maxine, NVENC)
  is built when its SDK is present and stays "untested on hardware" (spec §7.2).
- Visual Studio 2026 Community (`C:\Program Files\Microsoft Visual Studio\18\Community`)
  with its bundled CMake, Ninja and vcpkg. Use a VS developer shell
  (`Launch-VsDevShell.ps1 -Arch amd64`) for every build.
- The `@bmsolutionsvn/design` npm package comes from GitHub Packages:
  `NODE_AUTH_TOKEN="$(gh auth token)" npm ci`. Node 24 for vitest/jsdom
  (`PATH="/c/Program Files/nodejs:$PATH"`).
