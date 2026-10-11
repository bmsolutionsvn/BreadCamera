# Working rules for Claude in BreadCamera

BreadCamera is a native Windows multi-camera GPU processing app (C++20 engine +
WebView2/React UI). The design is
`docs/superpowers/specs/2026-10-08-breadcamera-design.md`; plans live in
`docs/superpowers/plans/`.

## Planning session vs implementing agents

The main (interactive) Claude session **plans; it does not implement**. It writes
specs and plans, reviews finished work, merges branches, and answers questions. It
does not write product code, tests or build scripts. Implementation goes to OpenCode
(below). An agent dispatched to implement a plan is exempt.

## Handing plans to OpenCode (owner 2026-10-11; aider and Hermes are retired for BreadCamera)

- Remaining work is in `docs/superpowers/plans/2026-10-11-remaining-milestones-opencode.md`: the task order, branch chain, verification command and stop conditions. OpenCode runs it, one task per commit.
- The planner (this session) reviews each finished milestone branch, fixes findings, and merges into `main` only when the owner asks. Workers never merge or push.
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
