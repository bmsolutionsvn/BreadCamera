# Remaining BreadCamera Milestones (M2 Task 4 → M6) Implementation Plan

> **For agentic workers:** This is a handoff plan for OpenCode. Execute one task at a time, in the order below. Each task's full text lives in its milestone plan; follow that plan's steps, with the substitutions and rules in this file. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish milestones M2 (matte and background, from Task 4), M3, M4a, M4b, M5 and M6 of BreadCamera, one commit per task, each milestone on its own branch cut from the previous one.

**Architecture:** Milestone plans in `docs/superpowers/plans/2026-10-08-*.md` are the authority for each task. The spec `docs/superpowers/specs/2026-10-08-breadcamera-design.md` is binding over the plans. Branches form a chain: `bcam-m2` (from `main`) → `bcam-m3` → `bcam-m4a` → `bcam-m4b` → `bcam-m5` → `bcam-m6`. `bcam-m2` already holds Tasks 1–3 (commits `3b794c8`, `2e2c049`, `d69c4fc`).

**Tech Stack:** C++20 (MSVC, VS 2026), D3D11, FFmpeg (LGPL), doctest, CMake/vcpkg; WebView2 + React + TypeScript in `ui/` (vitest, Vite). Build and test only through `scripts/build.ps1` and `scripts/test.ps1`.

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md`

## Global Constraints

- Everything in M1's Global Constraints (`docs/superpowers/plans/2026-10-08-m1-passthrough.md`, lines 17–29) still applies: `/W4 /permissive- /utf-8`; FFmpeg LGPL-only (never `gpl`, `nonfree`, `x264`, `x265`); every SDK behind `BC_WITH_<NAME>` and a missing SDK never fails the build; tests needing an SDK, NVIDIA GPU, card or encoder use `BC_REQUIRES(...)` and are skipped with a printed reason; never black; control API on 127.0.0.1 with the token; DPAPI secrets; signal states `ready|connecting|live|offline`; English-only UI, Unicode names limited in code points; conventional commit prefixes, one commit per task.
- Build machine: AMD Radeon RX 6600, no NVIDIA. NVIDIA-only code (Maxine, CUDA, NVENC) compiles when its SDK is present and stays untested.
- Never loosen an acceptance bound or an expected value to make a test pass. Fix the source and report the numbers.
- Never commit or write a secret, token or private key.
- Match existing code: reuse `bc::` types and helpers, `nlohmann::json`, doctest; read a file before editing it.
- Plan code that disagrees with existing code (renamed member, different signature): follow the existing code, keep the plan's behaviour, and say so in the commit body.

## Known environment limits (do not "fix" these in product code)

- **WSL mirrored networking reserves TCP ports 44620–48715** on this PC. Four socket tests bind fixed ports in that range and fail here: `ControlServer: refuses a wrong token…`, `ControlServer: hostile messages…`, `FfmpegInput: an SRT listener…`, `FfmpegInput: stop() returns promptly…`. Run the suite with `--test-case-exclude` for those names (see the verification command below). Tracked in `docs/superpowers/follow-ups.md`.
- **M1's 4× 1080p60 acceptance case fails** (about 24 fps; readback hotspot, see `docs/superpowers/follow-ups.md`). M1 was merged on the owner's decision. Acceptance cases are excluded from the routine suite.

## Verification command (every task)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
./build/release/bc_tests.exe '--test-case-exclude=*acceptance*,ControlServer: refuses a wrong token*,ControlServer: hostile messages*,FfmpegInput: an SRT listener*,FfmpegInput: stop() returns promptly*'
```

If a task touches `ui/`, also run `cd ui && npm test && npm run build` with `PATH="/c/Program Files/nodejs:$PATH"`.

Acceptance tasks (named "acceptance" or "M<n> acceptance" in the milestone plans) are **not** for OpenCode to mark as passed: implement them, run them, and record the numbers in the task report. The owner judges them.

## Task order

Each row is one task in its milestone plan. Task text, Files, Interfaces and commit message are in the plan named in the last column.

| # | Branch | Task | Plan |
|---|---|---|---|
| 1 | bcam-m2 | M2 Task 4 — Matte service: backend interface, batched matte per tick, stages in the camera | `2026-10-08-m2-matte-background.md` |
| 2 | bcam-m2 | M2 Task 5 — Maxine AI Green Screen backend (NVIDIA-only, untested on the build PC) | same |
| 3 | bcam-m2 | M2 Task 6 — UI: Effects in the Inspector: add-ins, schema-driven settings, Add effect | same |
| 4 | bcam-m2 | M2 Task 7 — M2 acceptance (record numbers; owner judges) | same |
| 5 | bcam-m3 | M3 Tasks 1–8 (colour maths, chart geometry, colour stage, chart detection (OpenCV optional), calibration service, UI, M3 acceptance) | `2026-10-08-m3-color-match.md` |
| 6 | bcam-m4a | M4a Tasks 1–6 (shared H.264 encoder and SRT output, NDI input, OMT output, USB/UVC input, factory and UI, M4a acceptance) | `2026-10-08-m4a-network-usb-io.md` |
| 7 | bcam-m4b | M4b Tasks 1–5 (DeckLink, AJA (experimental), Windows virtual camera, factory and UI, M4b verification) | `2026-10-08-m4b-cards-vcam.md` |
| 8 | bcam-m5 | M5 Tasks 1–5 (Settings dialog, error headlines, soak tool, installer and licence notices, verification) | `2026-10-08-m5-hardening.md` |
| 9 | bcam-m6 | M6 Tasks 1–3 (ONNX Runtime + DirectML + model, `OnnxMatte`, M6 acceptance on the AMD PC) | `2026-10-08-m6-onnx-matte.md` |

**M4c (branch `bcam-m4c`, plan `2026-10-13-m4c-omt-hx-output.md`): done.** One `omt` output kind on the merged `omthx` library — plain OMT and OMT-HX (HEVC from the GPU hardware encoder) with alpha, audio, a keyframe on a dropped frame, and tally published on the output state. It **supersedes M4a Task 3** (the libomt `OmtOutput`, never implemented).

## Per-task procedure

- [ ] **Step 1: Branch.** `git checkout <branch>`. Create it from the previous branch if missing (`git checkout -b bcam-m3 bcam-m2`, etc.). Confirm `git status` is clean before you start.
- [ ] **Step 2: Read.** Read the task in its milestone plan, then the files it names (with `file:line` where given). Read the Interfaces block; it names what this task consumes and produces.
- [ ] **Step 3: Implement** the steps as written, TDD where the plan says so. Do not edit files the task does not name except to add to `CMakeLists.txt` when the task needs a new source file.
- [ ] **Step 4: Verify** with the verification command above, plus `ui/` checks when the task touches `ui/`. Record the output tail.
- [ ] **Step 5: Commit** exactly the task's files with the commit message from the plan. Never `git add -A`. Never `--no-verify`.
- [ ] **Step 6: Report.** Append to `docs/superpowers/manual-checks/` or the task's own report file only if the plan names one. Otherwise put the report in the commit body: test counts, the `BC_WITH_*` configure line used, any ruling and any deferred minor. Add deferred minors to `docs/superpowers/follow-ups.md`.

## Milestone gates

- After each milestone's last task, run the full verification command and record the counts. Stop at the milestone boundary; the owner reviews and merges the branch into `main` before the next branch is started from `main`.
- Do **not** merge, push, rebase onto `main`, or mark any task reviewed. Do not change `.superpowers/` or the aider queue (aider is retired for this work).

## Human gates (owner runs these; OpenCode records them as open)

- M3: calibration with a printed chart and a tablet on real cameras.
- M4b: DeckLink, AJA and virtual camera on real hardware.
- M5: fresh-install check.
- M6: acceptance on the AMD build PC.
- Any RTX / Maxine check.

## Stop conditions

- A task stays red after two honest attempts: stop, leave the working tree as it is for inspection, and write the failing output into the report. Do not commit red work and do not weaken tests.
- A plan step needs a secret, an account login, a card or an NVIDIA GPU: skip that step, mark it as unverified, and continue only if later steps do not depend on it.
