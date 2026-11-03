# BreadCamera Lean Paths Phase 2: Plain OMT Output on UYVY, If It Pays — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure how much of a plain-OMT (VMX) send is spent turning BGRA into YUV. If UYVY input saves enough, the unkeyed plain-OMT output takes the camera's UYVY readback (phase 1) instead of BGRA. The wire stays standard VMX.

**Architecture:** A measuring test in OMT-HX compares process CPU time for `sendVideo` (BGRA) and `sendVideoUyvy` on a `Mode::Full` sender with a receiver connected. If the decision rule passes, `OmthOutput` reports `wantsUyvy` for Full mode without a key, and `deliver` sends `f.uyvy` with `sendVideoUyvy`.

**Tech Stack:** C++20, OMT-HX, libomt/libvmx (unchanged), doctest.

**Spec:** `BreadCamera/docs/superpowers/specs/2026-11-03-breadcamera-lean-paths-design.md` (§3.1 item 2, phase 2). Owner's condition: measure first, then check with a vMix receiver.

**Runs after:** lean phase 1 (`2026-11-03-lean-uyvy-outputs.md`) is merged.

## Global Constraints

- **Decision rule:** switch only if `sendVideoUyvy` uses at least **15% less** process CPU per 1920×1080 frame than `sendVideo` (BGRA), averaged over 300 frames, measured 3 times. Otherwise write the numbers in the ledger, add them to the spec's §6, and skip Task 2 (the run still ends green).
- Plain OMT stays standard on the wire. libomt is not changed.
- Keyed outputs and alpha frames keep BGRA (the UYVY readback has no alpha).
- The vMix check is the owner's, after the merge. The run cannot do it; it goes in the final report as a human step.

## Review Focus

1. **Colour:** a plain-OMT receiver sees the same picture as before (centre pixel of solid colours within ±8 per channel, at 1080 and 576 lines). Test in Task 2.
2. **Alpha frames** on an unkeyed plain-OMT output still arrive (through BGRA). Test in Task 2.
3. **Idle outputs** (encode on demand) still ask for no picture. Covered by the existing OmthOutput tests.
4. **Measurement noise:** three runs, each reported; the rule uses the mean, and a run spread above 10% is reported as such.
5. **A 576-line camera:** BT.601 UYVY reaches VMX tagged BT.709 by OMT-HX's header. Check what `videoHeader` sets and what libomt's VMX does with an SD picture; log a `Ruling:` with the finding. If the colour is off by more than ±8, keep BGRA below 720 lines (`wantsUyvy` false there).

---

### Task 1: Measure

**Files:** Test `OMT-HX/tests/link_test.cpp` (a measuring test that only prints and checks that both paths send).

- [ ] **Step 1: Write** `"plain OMT: CPU per frame, BGRA vs UYVY"` (no hardware needed): a `Mode::Full` sender with one `Receiver` connected and draining on a thread; 300 frames of a 1920×1080 test pattern via `sendVideo` (BGRA), then 300 of the same picture as UYVY via `sendVideoUyvy`; process CPU time from `GetProcessTimes` around each loop. `MESSAGE` ms per frame for each, and the saving in %. `CHECK` both loops sent all frames (`stats().framesSent`).
- [ ] **Step 2: Run** it 3 times (`scripts/test.ps1 -Filter "plain OMT: CPU per frame*"`). Record the three results in the ledger.
- [ ] **Step 3: Decide** by the rule. Commit the test on OMT-HX branch `omthx-measure-vmx-input` (`test: measure plain OMT's CPU for BGRA and UYVY input`) and merge it into OMT-HX `main` (`merge: VMX input measurement`). If the rule fails, append the numbers to the spec's §6 in BreadCamera (commit `docs: plain OMT stays on BGRA, measured`), and stop here with `DONE`.

### Task 2: `OmthOutput` takes UYVY in Full mode (only if Task 1 passed)

**Files:** Modify `src/io/omt_output.h`, `src/io/omt_output.cpp`; test `tests/omt_output_test.cpp`.

**Interfaces:**
- Consumes: `OutFrame::uyvy`, `Output::wantsUyvy` (phase 1).
- Produces: `OmthOutput::wantsUyvy(w, h)` = `mode_ == Mode::Full && !keyed_` (and `h >= 720` if Review Focus 5 says so). `deliver`: with `f.uyvy` → `send_->sendVideoUyvy(format_, data, width * 2, nullptr, 0, false, ts)`; otherwise today's paths.

- [ ] **Step 1: Failing tests** (`Omt`):
  - `"OmthOutput: plain OMT takes UYVY"`: `wantsUyvy(1920, 1080)` true for an unkeyed Full output, false for keyed and for HX.
  - `"OmthOutput: plain OMT sends a UYVY frame"`: started, receiver connected, fed `OutFrame`s with only `uyvy` (solid colours at 1920×1080 and 720×576, made with `bgraToUyvy`) → the receiver's BGRA centre pixel within ±8 of the colour.
  - An alpha frame (only `bgra`, `hasAlpha`) still arrives.
- [ ] **Step 2: Run** `--test-case=OmthOutput*` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** them, then the full verification command → green.
- [ ] **Step 5: Commit, merge into `main`** (`feat(io): plain OMT output takes UYVY`, `merge: lean phase 2, plain OMT on UYVY`). Final report names the owner's vMix check as the remaining step.
