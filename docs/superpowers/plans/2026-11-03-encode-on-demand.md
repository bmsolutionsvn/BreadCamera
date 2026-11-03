# Encode Only While Someone Is Pulling — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An OMT or OMT-HX sender with nobody connected packs, encodes and sends nothing, and BreadCamera skips the readback and the RGBA texture for such an output.

**Architecture:** OMT-HX gets a small pure state machine (`IdleGate`) that decides per video frame: send, resume with a keyframe, skip, or skip and close the encoders after 10 s idle. `Sender` feeds it a cached `omt_send_connections > 0` and gates every video send, `sendCoded` and `sendAudio`. BreadCamera's `OmthOutput` publishes `hasReceivers()` from its worker and asks the camera for no picture while it is false. OMT-Bridge moves its OMT-HX submodule forward and needs no code.

**Tech Stack:** C++20, libomt (C API, unchanged), doctest, CMake presets, PowerShell build scripts.

**Spec:** `BreadCamera/docs/superpowers/specs/2026-11-03-encode-on-demand-design.md`

## Global Constraints

- Plain OMT (VMX) stays standard on the wire. libomt and `omt/libomtnet` are not changed.
- Warm time after the last receiver leaves: **10 s** (`IdleGate::kWarmMs = 10'000`).
- The connection count is refreshed at most every **100 ms**.
- "Has receivers" = `omt_send_connections(tx) > 0`. Any connection counts, probes and preview receivers included (spec §3.1).
- Argument checks still throw while idle: the gate comes after them.
- No new field in `omthx_c_sender_counters` (the DLL fills it; old callers would be overrun).
- Order: OMT-HX first (merged to `main`), then BreadCamera, then OMT-Bridge.
- OMT-HX holds the owner's uncommitted work: `omt/libomtnet`, `dist/`, `scripts/link-check.ps1`. Never stage, stash, reset or clean it.
- Never push. Never use `git worktree`.

## Review Focus

1. **A probe of an idle HX sender** (BreadCamera's add-camera dialog, OMT-Bridge) must still read the source: the probe's connection wakes the sender. Test in Task 2.
2. **A receiver returning inside the warm window** must get a keyframe at once, not wait for the 10 s periodic one. `Resume` is pinned by Task 1's tests. Task 2's end-to-end check cannot isolate it, because an OMT-HX receiver also asks for a keyframe when it joins; it proves the picture comes promptly.
3. **A relay (`sendCoded`) that wakes up** must ask its own source for a keyframe, or its new receiver waits a whole GOP. Test in Task 2, on a plain-OMT relay so the receiver's own join request cannot mask it.
4. **A keyed or plain-OMT BreadCamera output** must ask for the CPU picture once a receiver connects, and stop asking when it leaves. Test in Task 3.
5. **A refused GPU path while idle** (odd frame size) must still show its warning, and read back once a receiver connects. Test in Task 3.

---

### Task 1: OMT-HX `IdleGate`

**Files:**
- Modify: `OMT-HX/src/codec.h` (next to `KeyframePolicy`)
- Modify: `OMT-HX/src/pure.cpp` (next to `KeyframePolicy::next`)
- Test: `OMT-HX/tests/pure_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // Whether a sender codes this video frame: only while a receiver is connected (encode on demand).
  class IdleGate {
   public:
    static constexpr int64_t kWarmMs = 10'000;  // encoders stay open this long after the last receiver leaves
    enum class Step { Send, Resume, Skip, SkipAndClose };
    Step next(int64_t nowMs, bool receivers);
  };
  ```
  `Send`: code it. `Resume`: the first frame after an idle spell; code it as a keyframe. `Skip`: nobody is connected. `SkipAndClose`: returned once per idle spell, on the first frame at or after `kWarmMs` of idleness; close the encoders, then skip. An idle spell starts at the first frame with `receivers == false`, including the very first frame.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("IdleGate: sends while a receiver is connected") {
  IdleGate g;
  CHECK(g.next(0, true) == IdleGate::Step::Send);
  CHECK(g.next(16, true) == IdleGate::Step::Send);
}

TEST_CASE("IdleGate: skips while idle, closes once after 10 s, resumes with a keyframe") {
  IdleGate g;
  CHECK(g.next(0, true) == IdleGate::Step::Send);
  CHECK(g.next(100, false) == IdleGate::Step::Skip);
  CHECK(g.next(10'099, false) == IdleGate::Step::Skip);
  CHECK(g.next(10'100, false) == IdleGate::Step::SkipAndClose);
  CHECK(g.next(10'116, false) == IdleGate::Step::Skip);
  CHECK(g.next(20'000, true) == IdleGate::Step::Resume);
  CHECK(g.next(20'016, true) == IdleGate::Step::Send);
}

TEST_CASE("IdleGate: a receiver back within 10 s resumes without a close, and the next spell starts afresh") {
  IdleGate g;
  CHECK(g.next(0, false) == IdleGate::Step::Skip);
  CHECK(g.next(5'000, false) == IdleGate::Step::Skip);
  CHECK(g.next(5'016, true) == IdleGate::Step::Resume);
  CHECK(g.next(6'000, false) == IdleGate::Step::Skip);
  CHECK(g.next(15'999, false) == IdleGate::Step::Skip);
  CHECK(g.next(16'000, false) == IdleGate::Step::SkipAndClose);
}
```

- [ ] **Step 2: Run them to see them fail**

Run (OMT-HX): `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "IdleGate*"`
Expected: build error, `IdleGate` not declared.

- [ ] **Step 3: Implement `IdleGate::next` in `src/pure.cpp`** with the semantics above (two members: whether idle, and when the spell started, plus whether it already closed).

- [ ] **Step 4: Run them to see them pass**

Same command. Expected: 3 test cases pass.

- [ ] **Step 5: Commit** (on branch `omthx-encode-on-demand`)

```bash
git add src/codec.h src/pure.cpp tests/pure_test.cpp
git commit -F - <<'EOF'
feat: IdleGate decides when a sender codes nothing
EOF
```

### Task 2: OMT-HX senders code nothing while nobody is connected

**Files:**
- Modify: `OMT-HX/include/omthx/omthx.h` (`SenderStats`, `Sender`, its class comment)
- Modify: `OMT-HX/include/omthx/omthx_c.h`, `OMT-HX/src/capi.cpp`
- Modify: `OMT-HX/src/link.cpp` (`Sender::Impl` and every `Sender` send)
- Test: `OMT-HX/tests/link_test.cpp`, `OMT-HX/tests/capi_test.cpp`

**Interfaces:**
- Consumes: `IdleGate` (Task 1).
- Produces:
  - `bool Sender::hasReceivers();` — safe from any thread; `omt_send_connections(tx) > 0`, cached, refreshed at most every 100 ms (atomics; the first call always counts).
  - `SenderStats::framesIdle` (`uint64_t`): video frames not coded or sent because nobody was connected.
  - `OMTHX_C_API int omthx_c_sender_has_receivers(omthx_c_sender* sender);` — 1 or 0; 0 for null.

Behaviour, in `link.cpp`:
- One private `bool Impl::skipVideo()` called by `sendVideo(format, bgra, …)`, `sendVideoNv12`, `sendVideoUyvy`, `sendVideoD3D11` and `sendCoded` right after their argument checks, and by `sendVideo(const VideoFrame&)` in its `frame.gpu` branch after the format check (its other branches call the gated overloads). `sendVideoD3D11` gates before it makes or uses `bgraToNv12`. It runs `gate.next(nowMs, hasReceivers())`, `nowMs` from `start` as `sendPicture` does:
  - `Skip`: `++stats.framesIdle` (under `statsMutex`), return true.
  - `SkipAndClose`: reset `encoder`, `alphaEncoder`, `previewEncoder`; clear `previewStamps`; then as `Skip`. (`openEncoder` already clears the other deques when it reopens.)
  - `Resume`: `policy = KeyframePolicy()`, `previewPolicy = KeyframePolicy()` (a fresh policy codes a keyframe at once, past the 200 ms limit), `forward = true` (a relay asks its source); return false.
  - `Send`: return false.
- `sendAudio` returns after its checks while `!hasReceivers()`; it does not touch the gate or `framesIdle`.
- Comments: `Sender`'s class comment says sends return at once while nobody is connected, and the first frame after a connect is a keyframe; `hasReceivers` and `framesIdle` say what they count.

- [ ] **Step 1: Write the failing tests** in `tests/link_test.cpp` (sender names unique, e.g. `omthx-test-idle-…`; hardware tests return early when `hardware(Codec::Hevc)` is false, like their neighbours):

  - `"an HX sender with no receiver codes nothing"`: 120 `sendVideo` calls of the test pattern at 640×360 without sleeping →
    `CHECK(s.stats().framesIdle == 120)`, `CHECK(s.stats().framesSent == 0)`, `CHECK(s.stats().encoder.empty())`, `CHECK_FALSE(s.hasReceivers())`.
    Then a `Receiver` and a `Pump` → `REQUIRE(receiveSome(r, frame, 1, 3) == 1)`, `CHECK(s.hasReceivers())`.
  - `"a receiver that comes back to a warm HX sender gets a keyframe at once"`: `Pump` running; receiver 1 gets one picture; destroy it; poll up to 2 s until `!s.hasReceivers()` and `framesIdle` grows; note `keyframes`; receiver 2 →
    picture within 1 s of its construction, `CHECK(s.stats().keyframes > before)`.
  - `"probe reads an idle HX source"`: `Pump` on an HX sender with no receiver; `REQUIRE(probe(s.address(), 3000, info))`, `CHECK(info.format == f)`.
  - `"a plain OMT sender with no receiver hands libomt nothing"`: as the first test in `Mode::Full` (no hardware check) → `framesIdle == 120`, `framesSent == 0`; then a receiver gets a picture.
  - `"a plain OMT relay with no receiver sends nothing, and asks for a keyframe when one joins"`: take one real VMX `CodedFrame` from a plain OMT source through a `PixelFormat::Coded` receiver (as `"a plain OMT relay passes VMX frames on without decoding them"` does). Relay `Sender b(…, Mode::Full)`: 10 × `b.sendCoded(c)` → `framesIdle == 10`, `framesSent == 0`, `CHECK_FALSE(b.keyframeRequested())`. Then `Receiver r(b.address(), PixelFormat::Coded)`; call `sendCoded` every 16 ms for up to 2 s until `keyframeRequested()` → `CHECK(requested)`, `CHECK(b.stats().framesSent > 0)`.

  In `tests/capi_test.cpp`: `"has_receivers is 0 for a fresh sender and for null"` → `CHECK(omthx_c_sender_has_receivers(s) == 0)`, `CHECK(omthx_c_sender_has_receivers(nullptr) == 0)`.

- [ ] **Step 2: Run them to see them fail**

Run: `scripts/test.ps1 -Filter "*idle*"` and `-Filter "*has_receivers*"`. Expected: build errors (`hasReceivers`, `framesIdle`, `omthx_c_sender_has_receivers` unknown).

- [ ] **Step 3: Implement** as described above.

- [ ] **Step 4: Run the new tests, then the whole suite**

Run: the filters from Step 2, then `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1`. Expected: all green. Existing tests that send before a receiver connects still pass, since their receivers wake the sender.

- [ ] **Step 5: Commit, then merge into OMT-HX `main`**

```bash
git add include/omthx/omthx.h include/omthx/omthx_c.h src/capi.cpp src/link.cpp tests/link_test.cpp tests/capi_test.cpp
git commit -F - <<'EOF'
feat: senders code nothing while nobody is connected
EOF
git checkout main
git merge --no-ff omthx-encode-on-demand -m "merge: encode only while someone is pulling"
```

Run `scripts/test.ps1` once more on `main`. Leave OMT-HX on `main` (BreadCamera builds `../OMT-HX` as checked out). If red, do not merge; stop.

### Task 3: BreadCamera skips the picture for an output nobody pulls

**Files:**
- Modify: `BreadCamera/src/io/omt_output.h`, `BreadCamera/src/io/omt_output.cpp`
- Test: `BreadCamera/tests/omt_output_test.cpp`

**Interfaces:**
- Consumes: `omthx::Sender::hasReceivers()` (Task 2).
- Produces (unchanged signatures, new meaning):
  - private `bool gpuPath() const { return mode_ == omthx::Mode::Hx && !keyed_ && !gpuRefused_; }`
  - `needsCpu()` = `receivers_ && !gpuPath()`; `wantsGpuRgba8()` = `receivers_ && gpuPath()`.
  - `std::atomic<bool> receivers_{false}` — set by the worker at the end of `deliver` (`send_->hasReceivers()`), next to the tally; reset to false in `close()`. The tick thread never touches `send_`.
  - `deliver` chooses its path with `gpuPath()` instead of `wantsGpuRgba8()`. Audio and tally keep running every tick.

- [ ] **Step 1: Write the failing tests / change the existing ones**

  - Replace `"OmthOutput: the HX output without alpha uses the GPU path"` with `"OmthOutput: an output nobody pulls asks the camera for no picture"`: for the HX, Full and keyed outputs, `CHECK_FALSE(needsCpu())` and `CHECK_FALSE(wantsGpuRgba8())`.
  - `"OmthOutput: an HX output sends the GPU frame"`: after `start()` and some fed frames, `CHECK_FALSE(out.wantsGpuRgba8())`; after the `Receiver` connects, poll up to 2 s until `out.wantsGpuRgba8()`, then `CHECK_FALSE(out.needsCpu())`. The picture and `"(d3d11)"` checks stay. After the receiver is destroyed, poll up to 2 s until `!out.wantsGpuRgba8()`. After `stop()`, `CHECK_FALSE(out.wantsGpuRgba8())`.
  - New `"OmthOutput: keyed and plain OMT outputs ask for the CPU picture once a receiver connects"` (Omt requirement): for each, started and fed frames with `bgra`, `needsCpu()` false before a receiver, true within 2 s after one connects, and `wantsGpuRgba8()` false throughout.
  - `"OmthOutput: an odd-sized frame falls back to the CPU path"`: wait for `status().error->code == "omt-gpu-refused"` instead of `needsCpu()`; after the receiver connects, poll up to 2 s until `out.needsCpu()`. The picture checks stay.
  - Grep the file for any other `needsCpu()` / `wantsGpuRgba8()` read before a receiver connects and move it after.

- [ ] **Step 2: Run them to see them fail**

Run (BreadCamera): `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -SkipUi` then `./build/release/bc_tests.exe "--test-case=OmthOutput*"`. Expected: the nobody-pulls test and the keyed/plain test fail (`needsCpu()` true without a receiver).

- [ ] **Step 3: Implement** as described in Interfaces.

- [ ] **Step 4: Run the OMT tests, then the full verification command**

```powershell
./build/release/bc_tests.exe "--test-case=OmthOutput*"
./build/release/bc_tests.exe '--test-case-exclude=*acceptance*,ControlServer: refuses a wrong token*,ControlServer: hostile messages*,FfmpegInput: an SRT listener*,FfmpegInput: stop() returns promptly*'
```
Expected: all green.

- [ ] **Step 5: Commit on `bcam-encode-on-demand`, merge into `main`**

```bash
git add src/io/omt_output.h src/io/omt_output.cpp tests/omt_output_test.cpp
git commit -F - <<'EOF'
feat: an OMT output nobody pulls asks the camera for no picture
EOF
git checkout main
git merge --no-ff bcam-encode-on-demand -m "merge: encode only while someone is pulling"
```

### Task 4: OMT-Bridge takes the new OMT-HX

**Files:**
- Modify: `OMT-Bridge/third_party/OMT-HX` (submodule pin only)

- [ ] **Step 1: Move the pin** (on branch `ob-omthx-encode-on-demand`)

```bash
git -C third_party/OMT-HX fetch sibling main      # the remote "sibling" is F:\Coding\BreadMedia\OMT-HX
git -C third_party/OMT-HX checkout <OMT-HX main hash from Task 2>
git add third_party/OMT-HX
git commit -F - <<'EOF'
deps: OMT-HX <short hash>; outputs nobody pulls code nothing
EOF
```
Commit before building: `scripts/build.ps1` runs `git submodule update`, which checks out the recorded pin. The libomtnet pin is unchanged since e94ca7a, so nothing is fetched from GitHub for it.

- [ ] **Step 2: Build and test**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1`. Expected: green, and `git -C third_party/OMT-HX log -1 --format=%h` prints the new pin.

- [ ] **Step 3: Merge into `main`**

```bash
git checkout main
git merge --no-ff ob-omthx-encode-on-demand -m "merge: OMT-HX with encode on demand"
```

### Task 5: Measure (reviewer, after the merges; not the run)

- `BreadCamera/.superpowers/omt-bench.ps1`: P0 (no outputs), P1 (4 HX outputs, no receivers), P1 with one receiver per output. P1 without receivers must be within noise of P0; with receivers, within noise of the post-copy-paths P1 (CPU 8.9%).
- The owner checks a vMix receiver on an idle plain-OMT BreadCamera output: the picture appears at once.
