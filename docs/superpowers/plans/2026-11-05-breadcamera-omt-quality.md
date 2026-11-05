# BreadCamera OMT Quality Setting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each BreadCamera OMT output gets a saved **quality** (Low / Medium / High), so a venue can fit its cameras to the upload with the encoder it already runs.

**Architecture:** A new output param `quality` beside the existing `codec`, checked in the same two places (`CommandRouter::validateOutput` and the adapter factory), passed to `OmthOutput` and on to `omthx::Sender`, which already takes an `omthx::Quality`. The Add Output dialog gets a Quality select.

**Tech Stack:** C++20, doctest, React + vitest.

**Spec:** `F:\Coding\BreadMedia\OMT-Bridge\docs\superpowers\specs\2026-11-05-omt-bridge-remote-design.md` §7 (plan 2 of the remote link). The codec setting the spec also asks for already exists (`params.codec`, "hevc" / "h264"); this plan adds only quality.

Repo: `F:\Coding\BreadMedia\BreadCamera`, branch `bcam-omt-quality` from `main`, merged back with `--no-ff`. Never push.

## Global Constraints

- Param `quality`: `"low" | "medium" | "high"`; absent = `"medium"` (today's behaviour, so old show files are unchanged).
- Maps to `omthx::Quality::Low / Medium / High`. 1080p60 bitrates this gives (for the UI hint, from OMT-HX): HEVC 15 / 30 / 50 Mbps, H.264 21 / 42 / 70 Mbps.
- Applies to both modes: HX (its encoder's bitrate) and plain OMT (libomt's own quality levels, still standard VMX on the wire).
- Pass-through is unchanged: a coded OMT-HX frame is sent as it came, whatever this output's quality (`takesCodedOmt` does not look at quality).
- Error copy: `OMT quality must be text`, `OMT quality must be 'low', 'medium' or 'high'`.
- UI tests for this plan: run only `npm --prefix ui test -- AddOutputDialog` (Node 24; in Bash `PATH="/c/Program Files/nodejs:$PATH"`). This overrides the brief's "do not run the UI suite" for this one file; do not run or fix the rest of the suite.

## Review Focus

1. A show file saved before this change (no `quality`): loads and sends at Medium, exactly as before. → Task 1 test.
2. A hand-edited show file with `"quality": 2` or `"quality": "ultra"`: refused with the error copy, by the factory as well as by the router. → Task 1 tests.
3. A plain OMT (full) output with `quality: "low"`: accepted and used (unlike codec, which full ignores). → Task 1 test.
4. Switching the dialog between OMT-HX and plain OMT keeps the chosen quality and sends it for both. → Task 2 test.
5. An output with quality set, fed by a pass-through OMT-HX source: still passes through (no encode). → covered by the existing pass-through tests staying green; no new test.

---

### Task 1: Engine — validate, build and use `quality`

**Files:**
- Modify: `src/engine/command_router.cpp:232-249` (the `omt` branch of `validateOutput`), `src/engine/adapter_factory.cpp:126-147` (the `omt` branch), `src/io/omt_output.h:18`, `src/io/omt_output.cpp:6-16`
- Test: `tests/command_router_test.cpp` (beside the codec cases at ~471-480); the factory case goes in `tests/omt_input_test.cpp`, building a `bc::DefaultAdapterFactory` from a GPU device and pool the way `tests/srt_pass_through_test.cpp` (~line 159, its `real_`) does

**Interfaces:**
- Produces:
  - `OmthOutput(std::string name, omthx::Mode mode, omthx::Codec codec = omthx::Codec::Hevc, bool keyed = false, omthx::Quality quality = omthx::Quality::Medium);` — stored as `quality_`, passed to `omthx::Sender(name_, mode_, quality_, codec_)` in `open()`.
  - `omthx::Quality quality() const { return quality_; }` on `OmthOutput` (for the test).
  - In `adapter_factory.cpp`, a local `omthx::Quality qualityFromParam(const nlohmann::json& params)` (in the anonymous namespace): absent → Medium; non-string → `invalid_argument("OMT quality must be text")`; unknown → `invalid_argument("OMT quality must be 'low', 'medium' or 'high'")`. The type check runs outside `#if BC_WITH_OMT`, like the codec check.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("validateOutput: OMT quality") {
  for (const char* q : {"low", "medium", "high"})
    CHECK_NOTHROW(bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "hx"}, {"quality", q}}}));
  CHECK_NOTHROW(bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "full"}, {"quality", "low"}}}));
  CHECK_NOTHROW(bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "hx"}}}));
  CHECK_THROWS_WITH_AS(bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"quality", "ultra"}}}),
                       "OMT quality must be 'low', 'medium' or 'high'", std::invalid_argument);
  CHECK_THROWS_WITH_AS(bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"quality", 2}}}),
                       "OMT quality must be text", std::invalid_argument);
}
```

Plus `TEST_CASE("AdapterFactory: OMT quality")` in `tests/omt_input_test.cpp` (guarded by `#if BC_WITH_OMT` like its neighbours): `makeOutput` for an `omt` output with `{"quality": "high"}` gives an `OmthOutput` whose `quality()` is `High`; with no `quality`, `Medium`; with `{"quality": 2}` the factory throws `"OMT quality must be text"`. The factory constructs the output without opening it, so no sender starts.

- [ ] **Step 2: Run and see them fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*OMT quality*"`
Expected: build error (no `quality()`), or the new cases fail.

- [ ] **Step 3: Implement** the interfaces above; in `validateOutput` add the `quality` check beside `codec`, with the same shape and a one-line comment that quality applies to both modes.
- [ ] **Step 4: Run** — `scripts/test.ps1 -Filter "*OMT quality*"` passes; then the brief's full `bc_tests.exe` command passes.
- [ ] **Step 5: Commit**

```bash
git add src/engine/command_router.cpp src/engine/adapter_factory.cpp src/io/omt_output.h src/io/omt_output.cpp tests
git commit -m "feat(omt): a quality setting per OMT output (low / medium / high)"
```

---

### Task 2: Add Output dialog — Quality select

**Files:**
- Modify: `ui/src/dialogs/AddOutputDialog.tsx` (state beside `omtCodec` ~line 42; `submit()` ~line 102; the select after Codec ~line 170)
- Test: `ui/src/dialogs/AddOutputDialog.test.tsx` (beside the codec tests ~line 42-60)

**Interfaces:**
- Consumes: Task 1's `quality` param.
- Produces: a `<Prop label="Quality">` select shown for **both** OMT modes, options in this order with this copy:
  - `low` — `Low (HEVC ~15 Mbps at 1080p60)`
  - `medium` — `Medium (HEVC ~30 Mbps)` (default)
  - `high` — `High (HEVC ~50 Mbps)`
  
  `submit()` sends `quality` in the params for both modes: `{ mode, codec, quality, name }` for hx, `{ mode, quality, name }` for full.

- [ ] **Step 1: Write the failing tests**
  - `'an OMT output offers a quality and sends the one chosen'` — pick type `omt`, `getByLabelText('Quality')` exists with value `medium`; change it to `low`; Add → params `toMatchObject({ mode: 'hx', codec: 'hevc', quality: 'low' })`.
  - `'a plain OMT output keeps the chosen quality'` — choose `low`, switch mode to `full`, `getByLabelText('Quality')` still shows `low`; Add → params `toMatchObject({ mode: 'full', quality: 'low' })` and `params.codec` is `undefined`.
- [ ] **Step 2: Run and see them fail** — `npm --prefix ui test -- AddOutputDialog`: the two new tests fail (no Quality field).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `npm --prefix ui test -- AddOutputDialog`: all pass, including the existing codec tests (the plain OMT one still sends no codec).
- [ ] **Step 5: Commit**

```bash
git add ui/src/dialogs/AddOutputDialog.tsx ui/src/dialogs/AddOutputDialog.test.tsx
git commit -m "feat(ui): choose an OMT output's quality when adding it"
```

---

### Task 3: Merge

- [ ] **Step 1:** The brief's full `bc_tests.exe` command and `npm --prefix ui test -- AddOutputDialog` pass on the branch.
- [ ] **Step 2:** `git checkout main; git merge --no-ff bcam-omt-quality -m "merge: OMT output quality setting"`; run the same two again on `main`, then the brief's BreadReplay compile check. Do not push.
