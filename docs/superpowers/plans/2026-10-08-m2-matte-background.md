# BreadCamera M2 — Matte and background Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** AI background removal (NVIDIA Maxine AI Green Screen, batched across cameras) plus the background stage (keyed fill+alpha, masked blur, replace), with per-stage bypass on failure or overrun, alpha over NDI, and a camera detail screen with stage panels.

**Architecture:** The camera tick from M1 splits into two halves around one batched matte call per tick: `beginTick` (pick + convert) → `MatteService` (one `MatteBackend::process` for every camera that wants a matte) → `endTick` (refine alpha → background compositor → readback → fan-out). Each stage is guarded by a `StageGuard` (bypass on exception or sustained overrun, retry after 5 s). The background maths are compute shaders on the shared D3D11 device; the Maxine backend is behind `BC_WITH_MAXINE` and stages its frames through CPU memory (guaranteed NvCVImage API). Parameters live in the show as `stages` per camera and are validated by pure schema code that the router and the UI share.

**Tech Stack:** as M1, plus the NVIDIA Video Effects SDK (Maxine) core + `nvvfxgreenscreen` feature (optional, NVIDIA-only).

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§3.2, §3.4, §3.6, §5.1, §5.2, §5.4, §7.2).

**Branch:** `bcam-m2`, cut from `bcam-m1` (M1 must be complete on that branch).

## Global Constraints

- Everything in M1's Global Constraints still applies (build/test scripts, LGPL FFmpeg, `BC_WITH_*` + `BC_REQUIRES`, never black, 127.0.0.1 + token, DPAPI secrets, signal states, Vietnamese-first UI copy, conventional commits).
- Stage order is fixed: **Color → Matte → Background** (spec §3.2). M2 builds Matte and Background; Color arrives in M3 and slots in before Matte.
- A stage failure bypasses **that stage for that camera only**, shows an error, and retries after **5 s** (spec §3.6). A bypassed matte means alpha = 1 (opaque passthrough), never a black or transparent frame.
- Matte runs at **1280×720** for every camera (one batch needs one size); the refined alpha is resampled to each frame's size (spec §5.1 "reduced resolution, refined at full resolution").
- Matte controls and defaults (spec §5.1): choke 0 px (range −8..8), feather 1 px (0..8), threshold 0.5 (0..1); quality mode `quality` or `performance`.
- Background modes (spec §5.2): `none`, `keyed` (fill + straight alpha), `blur` (masked, weighted by 1−alpha, no halo), `replace` (still image or solid colour). Default blur 12 px (1..50) at 1080p.
- The Maxine code is NVIDIA-only and **untested on the AMD build PC** (spec §7.2): it must compile when the SDK is present, its tests use `BC_REQUIRES(Req::Maxine)`, and it is listed in `docs/superpowers/follow-ups.md`.

## Review Focus

1. **No matte backend (AMD, or SDK missing) or a backend that fails mid-show** — every output keeps getting opaque frames, the camera shows `matte-unavailable` / `stage-failed`, and a recovered backend resumes within ~5 s — owned by Task 4.
2. **A stage that is too slow** — after 30 consecutive over-budget ticks it is bypassed with `stage-overrun`, the tick stops overrunning, and it is retried after 5 s — owned by Task 2 (guard) and Task 4 (slow backend test).
3. **Cameras added or removed while matting** — the removed camera's temporal state is released (`forget`), a new camera joins the next batch — owned by Task 4.
4. **Mixed input sizes in one batch** (720p and 1080p cameras) — each camera's alpha lands at its own frame size — owned by Task 4.
5. **Halo around the person when blurring** — background pixels next to the edge contain no person colour — owned by Task 3.

## Prerequisites the owner installs (not the night run)

- NVIDIA Video Effects SDK core + `nvvfxgreenscreen` feature from NGC (free Developer Program account), with `VFXSDK_ROOT` set to its folder. Without it `BC_WITH_MAXINE=OFF`: everything else in M2 still builds and is tested with the test matte backend.

## File Structure

```
src/core/stage_params.h/.cpp      schemas, defaults, validation (pure)
src/core/stage_guard.h/.cpp       bypass / overrun / retry state machine (pure)
src/gpu/compositor.h/.cpp         alpha refine + background modes
src/gpu/shaders/alpha_threshold.hlsl, alpha_morph.hlsl, alpha_blur.hlsl,
                bg_downsample.hlsl, bg_blur.hlsl, bg_composite.hlsl
src/engine/matte.h/.cpp           MatteJob, MatteBackend, makeMatteBackend
src/engine/maxine_matte.h/.cpp    Maxine backend (BC_WITH_MAXINE)
cmake/FindMaxine.cmake
ui/src/screens/CameraDetail.tsx, ui/src/components/StageForm.tsx
tests/support/test_matte.h
```

## Execution notes for the controller

- 7 tasks → batched reviews (1–3, 4–6, 7).
- Task 2 changes M1's `Camera::tick` into two halves. M1's engine tests must still pass unchanged after Task 2 — that is the refactor's acceptance test.

---

### Task 1: Stage parameters — schemas, validation, show storage, `setStage`

**Files:**
- Create: `src/core/stage_params.h`, `src/core/stage_params.cpp`
- Modify: `src/core/show.h` (`CameraConfig::stages`), `src/core/show.cpp` (read/write `stages`), `src/engine/command_router.cpp` (`setStage`, `getStageSchemas`), `CMakeLists.txt`
- Test: `tests/stage_params_test.cpp`, `tests/command_router_test.cpp` (add cases), `tests/show_test.cpp` (add a case)

**Interfaces:**
- Consumes: `CameraConfig`, `cameraToJson`, `cameraFromJson`, `ShowError` (M1 Task 3); `CommandRouter` (M1 Task 11).
- Produces:
  - `bc::StageField { std::string key, type; double min = 0, max = 0, step = 0; std::vector<std::string> options; nlohmann::json def; std::string labelVi, labelEn; }` — `type` is `"number" | "enum" | "bool" | "color" | "path"`.
  - `bc::StageSchema { std::string kind, labelVi, labelEn; std::vector<StageField> fields; }`.
  - `const std::vector<StageSchema>& bc::stageSchemas()`; `const StageSchema* bc::findStageSchema(std::string_view)`; `nlohmann::json bc::schemaToJson(const StageSchema&)`; `nlohmann::json bc::normalizeStageParams(std::string_view kind, const nlohmann::json& params)` (fills defaults, rejects unknown keys / wrong types / out of range / unknown enum values; throws `std::invalid_argument`).
  - `CameraConfig::stages` — `nlohmann::json` object `{ "<kind>": {normalized params} }`, default `{}`; a missing kind means "all defaults".
  - Commands: `setStage {cameraId, stage, params}` → `{}` (stores normalized params; `params` may be partial: it is merged over the current values, then normalized); `getStageSchemas` → array of schema JSON.

Schemas (M3 adds `color`):
| kind | key | type | range / options | default |
|---|---|---|---|---|
| `matte` | `enabled` | bool | | `false` |
| | `quality` | enum | `quality`, `performance` | `quality` |
| | `choke` | number | −8..8 step 1 | 0 |
| | `feather` | number | 0..8 step 0.5 | 1 |
| | `threshold` | number | 0..1 step 0.01 | 0.5 |
| `background` | `mode` | enum | `none`, `keyed`, `blur`, `replace` | `none` |
| | `blur` | number | 1..50 step 1 | 12 |
| | `color` | color | `#rrggbb` | `#101418` |
| | `image` | path | ≤ 1024 chars, empty = use colour | `""` |

Vietnamese labels: matte "Tách nền" (enabled "Bật", quality "Chất lượng" with options shown as "Chất lượng cao"/"Hiệu năng", choke "Co viền (px)", feather "Làm mềm viền (px)", threshold "Ngưỡng"); background "Nền" (mode "Chế độ nền": "Không", "Tách nền (key + fill)", "Làm mờ", "Thay nền"; blur "Độ mờ (px)"; color "Màu nền"; image "Ảnh nền"). English: Background removal / Enabled / Quality ("High quality", "Performance") / Choke (px) / Feather (px) / Threshold; Background / Background mode ("None", "Keyed (fill + key)", "Blur", "Replace") / Blur (px) / Colour / Image. The enum option labels travel as `optionLabelsVi` / `optionLabelsEn` arrays in the schema JSON, same order as `options`.

- [ ] **Step 1: Write the failing tests**

`tests/stage_params_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/stage_params.h"

using nlohmann::json;

TEST_CASE("normalizeStageParams: fills defaults") {
  auto m = bc::normalizeStageParams("matte", json::object());
  CHECK(m == json{{"enabled", false}, {"quality", "quality"}, {"choke", 0}, {"feather", 1}, {"threshold", 0.5}});
  auto b = bc::normalizeStageParams("background", {{"mode", "blur"}});
  CHECK(b["mode"] == "blur");
  CHECK(b["blur"] == 12);
  CHECK(b["color"] == "#101418");
  CHECK(b["image"] == "");
}

TEST_CASE("normalizeStageParams: rejects bad input") {
  CHECK_THROWS_AS(bc::normalizeStageParams("teleport", json::object()), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"choke", 9}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"choke", "big"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"quality", "ultra"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"enabled", 1}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"surprise", 1}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("background", {{"color", "red"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("background", {{"image", std::string(2000, 'x')}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", json::array()), std::invalid_argument);
}

TEST_CASE("stage schemas: matte and background, with labels and option labels") {
  auto* m = bc::findStageSchema("matte");
  REQUIRE(m);
  CHECK(m->labelVi == "Tách nền");
  auto j = bc::schemaToJson(*bc::findStageSchema("background"));
  CHECK(j["kind"] == "background");
  auto& mode = j["fields"][0];
  CHECK(mode["key"] == "mode");
  CHECK(mode["options"].size() == mode["optionLabelsVi"].size());
  CHECK(bc::stageSchemas().size() == 2);
}
```

Append to `tests/show_test.cpp`:
```cpp
TEST_CASE("Show: stages round-trip and are validated on load") {
  bc::Show s;
  bc::CameraConfig cam;
  cam.id = "cam1";
  cam.name = "Cam";
  cam.stages = {{"matte", bc::normalizeStageParams("matte", {{"enabled", true}})}};
  s.cameras.push_back(cam);
  auto j = bc::showToJson(s, bc::SecretMode::Plain);
  CHECK(bc::showFromJson(j) == s);
  j["cameras"][0]["stages"]["matte"]["choke"] = 99;
  CHECK_THROWS_AS(bc::showFromJson(j), bc::ShowError);
  j["cameras"][0]["stages"] = {{"teleport", nlohmann::json::object()}};
  CHECK_THROWS_AS(bc::showFromJson(j), bc::ShowError);
  auto old = bc::showToJson(s, bc::SecretMode::Plain);
  old["cameras"][0].erase("stages");  // an M1 show file
  CHECK(bc::showFromJson(old).cameras[0].stages == nlohmann::json::object());
}
```
(add `#include "core/stage_params.h"` at the top of `tests/show_test.cpp`.)

Append to `tests/command_router_test.cpp`:
```cpp
TEST_CASE("CommandRouter: setStage merges, normalizes and validates") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  REQUIRE(call(r, {{"cmd", "setStage"}, {"args", {{"cameraId", cam}, {"stage", "matte"}, {"params", {{"enabled", true}}}}}})["ok"] == true);
  REQUIRE(call(r, {{"cmd", "setStage"}, {"args", {{"cameraId", cam}, {"stage", "matte"}, {"params", {{"choke", 2}}}}}})["ok"] == true);
  auto m = r.show().cameras[0].stages["matte"];
  CHECK(m["enabled"] == true);  // kept from the first call
  CHECK(m["choke"] == 2);
  auto bad = call(r, {{"cmd", "setStage"}, {"args", {{"cameraId", cam}, {"stage", "matte"}, {"params", {{"choke", 50}}}}}});
  CHECK(bad["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "setStage"}, {"args", {{"cameraId", "nope"}, {"stage", "matte"}, {"params", json::object()}}}})["error"]["code"] == "not-found");
  CHECK(call(r, {{"cmd", "getStageSchemas"}})["result"].size() == 2);
}
```

Add `tests/stage_params_test.cpp` to `bc_tests` and `src/core/stage_params.cpp` to `bc_core`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*Stage*,*stage*"`
Expected: build FAILS — `core/stage_params.h` not found.

- [ ] **Step 3: Implement the schemas**

`src/core/stage_params.h`:
```cpp
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace bc {
struct StageField {
  std::string key;
  std::string type;  // number | enum | bool | color | path
  double min = 0, max = 0, step = 0;
  std::vector<std::string> options;
  std::vector<std::string> optionLabelsVi, optionLabelsEn;
  nlohmann::json def;
  std::string labelVi, labelEn;
};

struct StageSchema {
  std::string kind;
  std::string labelVi, labelEn;
  std::vector<StageField> fields;
};

// spec §5.4: every stage has a parameter schema; the UI builds its panel from it.
const std::vector<StageSchema>& stageSchemas();
const StageSchema* findStageSchema(std::string_view kind);
nlohmann::json schemaToJson(const StageSchema& s);
// Fills defaults; throws std::invalid_argument on unknown keys, wrong types or bad values.
nlohmann::json normalizeStageParams(std::string_view kind, const nlohmann::json& params);
}  // namespace bc
```

`src/core/stage_params.cpp`:
```cpp
#include "core/stage_params.h"

#include <algorithm>
#include <regex>
#include <stdexcept>

namespace bc {
namespace {
using nlohmann::json;

StageField number(std::string key, double min, double max, double step, double def, std::string vi, std::string en) {
  StageField f;
  f.key = std::move(key);
  f.type = "number";
  f.min = min;
  f.max = max;
  f.step = step;
  f.def = def;
  f.labelVi = std::move(vi);
  f.labelEn = std::move(en);
  return f;
}

StageField enumeration(std::string key, std::vector<std::string> options, std::vector<std::string> vi,
                       std::vector<std::string> en, std::string def, std::string labelVi, std::string labelEn) {
  StageField f;
  f.key = std::move(key);
  f.type = "enum";
  f.options = std::move(options);
  f.optionLabelsVi = std::move(vi);
  f.optionLabelsEn = std::move(en);
  f.def = std::move(def);
  f.labelVi = std::move(labelVi);
  f.labelEn = std::move(labelEn);
  return f;
}

StageField simple(std::string key, std::string type, json def, std::string vi, std::string en) {
  StageField f;
  f.key = std::move(key);
  f.type = std::move(type);
  f.def = std::move(def);
  f.labelVi = std::move(vi);
  f.labelEn = std::move(en);
  return f;
}

std::vector<StageSchema> build() {
  StageSchema matte{"matte", "Tách nền", "Background removal", {}};
  matte.fields = {
      simple("enabled", "bool", false, "Bật", "Enabled"),
      enumeration("quality", {"quality", "performance"}, {"Chất lượng cao", "Hiệu năng"}, {"High quality", "Performance"},
                  "quality", "Chất lượng", "Quality"),
      number("choke", -8, 8, 1, 0, "Co viền (px)", "Choke (px)"),
      number("feather", 0, 8, 0.5, 1, "Làm mềm viền (px)", "Feather (px)"),
      number("threshold", 0, 1, 0.01, 0.5, "Ngưỡng", "Threshold"),
  };
  StageSchema background{"background", "Nền", "Background", {}};
  background.fields = {
      enumeration("mode", {"none", "keyed", "blur", "replace"},
                  {"Không", "Tách nền (key + fill)", "Làm mờ", "Thay nền"}, {"None", "Keyed (fill + key)", "Blur", "Replace"},
                  "none", "Chế độ nền", "Background mode"),
      number("blur", 1, 50, 1, 12, "Độ mờ (px)", "Blur (px)"),
      simple("color", "color", "#101418", "Màu nền", "Colour"),
      simple("image", "path", "", "Ảnh nền", "Image"),
  };
  return {matte, background};
}

json checkField(const StageField& f, const json& v) {
  const std::string where = "'" + f.key + "'";
  if (f.type == "bool") {
    if (!v.is_boolean()) throw std::invalid_argument(where + " must be true or false");
    return v;
  }
  if (f.type == "number") {
    if (!v.is_number()) throw std::invalid_argument(where + " must be a number");
    const double d = v.get<double>();
    if (d < f.min || d > f.max) {
      throw std::invalid_argument(where + " must be between " + json(f.min).dump() + " and " + json(f.max).dump());
    }
    return v;
  }
  if (f.type == "enum") {
    if (!v.is_string() || std::find(f.options.begin(), f.options.end(), v.get<std::string>()) == f.options.end()) {
      throw std::invalid_argument(where + " has an unknown value");
    }
    return v;
  }
  if (f.type == "color") {
    static const std::regex hex("^#[0-9a-fA-F]{6}$");
    if (!v.is_string() || !std::regex_match(v.get<std::string>(), hex)) throw std::invalid_argument(where + " must look like #rrggbb");
    return v;
  }
  if (f.type == "path") {
    if (!v.is_string() || v.get<std::string>().size() > 1024) throw std::invalid_argument(where + " must be a path of at most 1024 characters");
    return v;
  }
  throw std::invalid_argument("unknown field type " + f.type);
}
}  // namespace

const std::vector<StageSchema>& stageSchemas() {
  static const std::vector<StageSchema> all = build();
  return all;
}

const StageSchema* findStageSchema(std::string_view kind) {
  for (auto& s : stageSchemas()) if (s.kind == kind) return &s;
  return nullptr;
}

json schemaToJson(const StageSchema& s) {
  json fields = json::array();
  for (auto& f : s.fields) {
    json j{{"key", f.key}, {"type", f.type}, {"default", f.def}, {"labelVi", f.labelVi}, {"labelEn", f.labelEn}};
    if (f.type == "number") {
      j["min"] = f.min;
      j["max"] = f.max;
      j["step"] = f.step;
    }
    if (f.type == "enum") {
      j["options"] = f.options;
      j["optionLabelsVi"] = f.optionLabelsVi;
      j["optionLabelsEn"] = f.optionLabelsEn;
    }
    fields.push_back(j);
  }
  return {{"kind", s.kind}, {"labelVi", s.labelVi}, {"labelEn", s.labelEn}, {"fields", fields}};
}

json normalizeStageParams(std::string_view kind, const json& params) {
  const auto* s = findStageSchema(kind);
  if (!s) throw std::invalid_argument("unknown stage '" + std::string(kind) + "'");
  if (!params.is_object()) throw std::invalid_argument("stage params must be an object");
  for (auto& [key, value] : params.items()) {
    if (std::none_of(s->fields.begin(), s->fields.end(), [&](const StageField& f) { return f.key == key; })) {
      throw std::invalid_argument("unknown setting '" + key + "' for stage " + s->kind);
    }
  }
  json out = json::object();
  for (auto& f : s->fields) out[f.key] = params.contains(f.key) ? checkField(f, params[f.key]) : f.def;
  return out;
}
}  // namespace bc
```

- [ ] **Step 4: Store stages in the show and add the commands**

In `src/core/show.h`, add to `CameraConfig` (before `operator==`): `nlohmann::json stages = nlohmann::json::object();`.

In `src/core/show.cpp`:
- `#include "core/stage_params.h"`.
- `cameraToJson`: add `{"stages", c.stages}` to the returned object.
- `cameraFromJson`: after reading outputs:
```cpp
  const auto stages = j.value("stages", json::object());
  if (!stages.is_object()) throw ShowError("stages must be an object");
  for (auto& [kind, params] : stages.items()) {
    try {
      c.stages[kind] = normalizeStageParams(kind, params);
    } catch (const std::invalid_argument& e) {
      throw ShowError("camera " + c.id + ": " + e.what());
    }
  }
```

In `src/engine/command_router.cpp`, `#include "core/stage_params.h"` and add before the `getShow` line (outside the show lock is fine for `getStageSchemas`):
```cpp
  if (cmd == "getStageSchemas") {
    json out = json::array();
    for (auto& s : stageSchemas()) out.push_back(schemaToJson(s));
    return out;
  }
```
and inside the locked branch chain:
```cpp
  } else if (cmd == "setStage") {
    auto& c = findCam(str(args, "cameraId"));
    const auto stage = str(args, "stage");
    const auto& params = obj(args, "params");
    json merged = c.stages.value(stage, json::object());
    for (auto& [k, v] : params.items()) merged[k] = v;
    try {
      c.stages[stage] = normalizeStageParams(stage, merged);
    } catch (const std::invalid_argument& e) {
      throw CmdError("invalid", e.what());
    }
```
Note `findCam` is checked first, so an unknown camera answers `not-found` before any param validation.

- [ ] **Step 5: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*Stage*,*stage*,Show*,CommandRouter*"`
Expected: all pass (the M1 Show/CommandRouter cases too).

- [ ] **Step 6: Commit**

```bash
git add src/core src/engine/command_router.cpp tests CMakeLists.txt
git commit -m "feat(core): stage parameter schemas, show storage and setStage command"
```

### Task 2: Stage guard and the two-half camera tick

**Files:**
- Create: `src/core/stage_guard.h`, `src/core/stage_guard.cpp`
- Modify: `src/engine/camera.h`, `src/engine/camera.cpp`, `src/engine/engine.cpp`, `CMakeLists.txt`
- Test: `tests/stage_guard_test.cpp`; M1's `tests/engine_test.cpp` must pass unchanged

**Interfaces:**
- Consumes: `ErrorInfo` (M1 Task 4), `normalizeStageParams` (Task 1), M1 `Camera`/`Engine`.
- Produces:
  - `bc::StageGuard(Policy = {})` with `Policy { int overrunTicks = 30; std::chrono::milliseconds retryAfter{5000}; }`: `bool shouldRun(time_point) const`, `void succeeded(nanoseconds took, nanoseconds budget, time_point now)`, `void failed(std::string message, time_point now)`, `bool bypassed() const`, `const std::optional<ErrorInfo>& error() const`, `std::string stateName(bool enabled) const` → `"off" | "active" | "bypassed"`.
  - `Camera::beginTick(time_point now, const TextureRef& slate) -> TextureRef` (the linear frame of this tick, never null), `bool Camera::wantsMatte(time_point now) const`, `const nlohmann::json& Camera::matteParams() const`, `const nlohmann::json& Camera::backgroundParams() const`, `void Camera::endTick(uint64_t index, const TickSchedule&, time_point now, const TextureRef& rawAlpha)` (rawAlpha may be null), `StageGuard& Camera::matteGuard()`, `StageGuard& Camera::backgroundGuard()`. **`Camera::tick` is removed**; the engine calls the halves.
  - State: `/cameras/<id>/stages/<kind>/state` (`off|active|bypassed`) and `/cameras/<id>/stages/<kind>/error` (`null|{code,message}`), written by `publishStatus()`.

Guard rules (spec §3.6): an exception → bypass now (`stage-failed`, the exception text). `overrunTicks` consecutive over-budget runs → bypass (`stage-overrun`). While bypassed the stage does not run until `retryAfter` has passed; then one **trial** run: within budget → active again (error cleared); over budget or failing → bypassed again for another `retryAfter`.

- [ ] **Step 1: Write the failing guard tests**

`tests/stage_guard_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/stage_guard.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST_CASE("StageGuard: a healthy stage stays active") {
  bc::StageGuard g;
  auto t = Clock::now();
  CHECK(g.shouldRun(t));
  g.succeeded(5ms, 10ms, t);
  CHECK_FALSE(g.bypassed());
  CHECK(g.stateName(true) == "active");
  CHECK(g.stateName(false) == "off");
}

TEST_CASE("StageGuard: sustained overrun bypasses, then retries after 5 s") {
  bc::StageGuard g;
  auto t = Clock::now();
  for (int i = 0; i < 29; ++i) g.succeeded(20ms, 10ms, t);
  CHECK_FALSE(g.bypassed());
  g.succeeded(20ms, 10ms, t);
  CHECK(g.bypassed());
  REQUIRE(g.error());
  CHECK(g.error()->code == "stage-overrun");
  CHECK_FALSE(g.shouldRun(t + 4s));
  CHECK(g.shouldRun(t + 5s));
}

TEST_CASE("StageGuard: one in-budget run clears the overrun streak") {
  bc::StageGuard g;
  auto t = Clock::now();
  for (int i = 0; i < 29; ++i) g.succeeded(20ms, 10ms, t);
  g.succeeded(5ms, 10ms, t);
  for (int i = 0; i < 29; ++i) g.succeeded(20ms, 10ms, t);
  CHECK_FALSE(g.bypassed());
}

TEST_CASE("StageGuard: a failed trial bypasses again at once; a good trial recovers") {
  bc::StageGuard g;
  auto t = Clock::now();
  g.failed("boom", t);
  CHECK(g.bypassed());
  CHECK(g.error()->code == "stage-failed");
  CHECK(g.error()->message == "boom");
  g.succeeded(20ms, 10ms, t + 5s);  // trial over budget
  CHECK(g.bypassed());
  CHECK_FALSE(g.shouldRun(t + 9s));
  CHECK(g.shouldRun(t + 10s));
  g.succeeded(5ms, 10ms, t + 10s);  // good trial
  CHECK_FALSE(g.bypassed());
  CHECK_FALSE(g.error());
}
```

Add it to `bc_tests` and `src/core/stage_guard.cpp` to `bc_core`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "StageGuard*"`
Expected: build FAILS — `core/stage_guard.h` not found.

- [ ] **Step 3: Implement the guard**

`src/core/stage_guard.h`:
```cpp
#pragma once
#include <chrono>
#include <optional>
#include <string>

#include "core/signal_state.h"

namespace bc {
// Per-camera, per-stage health (spec §3.6): bypass on failure or sustained overrun,
// retry after a pause. Used only from the tick thread.
class StageGuard {
 public:
  using Clock = std::chrono::steady_clock;
  struct Policy {
    int overrunTicks = 30;
    std::chrono::milliseconds retryAfter{5000};
  };
  StageGuard() = default;
  explicit StageGuard(Policy p) : policy_(p) {}

  bool shouldRun(Clock::time_point now) const { return !bypassed_ || now >= retryAt_; }
  void succeeded(std::chrono::nanoseconds took, std::chrono::nanoseconds budget, Clock::time_point now);
  void failed(std::string message, Clock::time_point now);
  bool bypassed() const { return bypassed_; }
  const std::optional<ErrorInfo>& error() const { return error_; }
  std::string stateName(bool enabled) const { return !enabled ? "off" : bypassed_ ? "bypassed" : "active"; }

 private:
  void bypass(ErrorInfo e, Clock::time_point now);
  Policy policy_;
  bool bypassed_ = false;
  int overruns_ = 0;
  Clock::time_point retryAt_{};
  std::optional<ErrorInfo> error_;
};
}  // namespace bc
```

`src/core/stage_guard.cpp`:
```cpp
#include "core/stage_guard.h"

namespace bc {
namespace {
std::string ms(std::chrono::nanoseconds d) {
  return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1000.0).substr(0, 5);
}
}  // namespace

void StageGuard::bypass(ErrorInfo e, Clock::time_point now) {
  bypassed_ = true;
  error_ = std::move(e);
  retryAt_ = now + policy_.retryAfter;
  overruns_ = 0;
}

void StageGuard::succeeded(std::chrono::nanoseconds took, std::chrono::nanoseconds budget, Clock::time_point now) {
  if (took > budget) {
    // A trial run (we were bypassed) gets no second chance; otherwise count the streak.
    if (bypassed_ || ++overruns_ >= policy_.overrunTicks) {
      bypass({"stage-overrun", "took " + ms(took) + " ms per tick; the budget is " + ms(budget) + " ms"}, now);
    }
    return;
  }
  overruns_ = 0;
  bypassed_ = false;
  error_.reset();
}

void StageGuard::failed(std::string message, Clock::time_point now) { bypass({"stage-failed", std::move(message)}, now); }
}  // namespace bc
```

- [ ] **Step 4: Split the camera tick**

In `src/engine/camera.h`:
- add `#include "core/stage_guard.h"` and `#include "core/stage_params.h"`;
- replace the `tick(...)` declaration with:
```cpp
  // First half (tick thread): newest frame → linear texture, or the slate. Never null.
  TextureRef beginTick(std::chrono::steady_clock::time_point now, const TextureRef& slate);
  // True when this camera's matte is enabled and its guard lets it run this tick.
  bool wantsMatte(std::chrono::steady_clock::time_point now) const;
  const nlohmann::json& matteParams() const { return matteParams_; }
  const nlohmann::json& backgroundParams() const { return backgroundParams_; }
  // Second half: rawAlpha (null = no matte this tick) → stages → readback → fan-out.
  void endTick(uint64_t index, const TickSchedule& sched, std::chrono::steady_clock::time_point now,
               const TextureRef& rawAlpha);
  StageGuard& matteGuard() { return matteGuard_; }
  StageGuard& backgroundGuard() { return backgroundGuard_; }
```
- add private members:
```cpp
  void loadStageParams();
  nlohmann::json matteParams_;
  nlohmann::json backgroundParams_;
  StageGuard matteGuard_;
  StageGuard backgroundGuard_;
  TextureRef tickFrame_;  // this tick's linear frame, between the halves
  std::optional<std::chrono::steady_clock::time_point> tickArrival_;
  FeedPhase tickPhase_ = FeedPhase::NoSignal;
```

In `src/engine/camera.cpp`:
- Add:
```cpp
void Camera::loadStageParams() {
  matteParams_ = normalizeStageParams("matte", cfg_.stages.value("matte", nlohmann::json::object()));
  backgroundParams_ = normalizeStageParams("background", cfg_.stages.value("background", nlohmann::json::object()));
}

bool Camera::wantsMatte(Clock::time_point now) const {
  return matteParams_.value("enabled", false) && matteGuard_.shouldRun(now);
}
```
- Call `loadStageParams();` at the end of the constructor body and at the end of `reconfigure` (after `cfg_ = next;`). When the matte gets disabled or its params change in `reconfigure`, reset its guard: `if (!(next.stages == oldStages)) { matteGuard_ = StageGuard{}; backgroundGuard_ = StageGuard{}; }` where `oldStages` is a copy of `cfg_.stages` taken at the top of `reconfigure`.
- Replace `Camera::tick` with the two halves. `beginTick` holds the first part of the old `tick` body:
```cpp
TextureRef Camera::beginTick(Clock::time_point now, const TextureRef& slate) {
  auto pick = sync_.pick(now);
  tickArrival_.reset();
  tickPhase_ = pick.phase;
  if (pick.frame) {
    if (pick.fresh || !lastConverted_) {
      const auto& f = pick.frame->frame;
      lastConverted_ = conv_.toLinear(*f.tex, f.layout, f.fullRange);
      if (pick.fresh) tickArrival_ = pick.frame->arrival;
    }
  } else {
    lastConverted_.reset();
  }
  tickFrame_ = lastConverted_ ? lastConverted_ : slate;
  return tickFrame_;
}

void Camera::endTick(uint64_t index, const TickSchedule& sched, Clock::time_point now, const TextureRef& rawAlpha) {
  (void)now;
  (void)rawAlpha;  // Task 4 runs the stages here
  TextureRef frame = tickFrame_;
  const bool hasAlpha = false;
  lastFrame_ = frame;

  auto audio = std::make_shared<std::vector<float>>(audio_.pull(sched.audioFramesForTick(index, 48000)));
  bool anyCpu = false;
  for (auto& s : outputs_) anyCpu = anyCpu || (s.out && s.out->needsCpu());
  std::shared_ptr<const CpuImage> cpu;
  if (anyCpu) cpu = std::make_shared<CpuImage>(conv_.readback(*conv_.toBgra8(*frame, frame->width, frame->height)));

  OutFrame out{index, sched.rate(), frame, cpu, audio, 48000, 2, hasAlpha};
  for (auto& s : outputs_) {
    if (s.out) s.out->submit(out);
  }

  std::lock_guard l(statsMutex_);
  phase_ = tickPhase_;
  if (tickArrival_) {
    ++freshThisSecond_;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - *tickArrival_).count();
    latencyEwmaMs_ = latencyEwmaMs_ == 0 ? ms : latencyEwmaMs_ * 0.9 + ms * 0.1;
  }
}
```
- In `publishStatus()`, after the phase line, add:
```cpp
  const bool matteOn = matteParams_.value("enabled", false);
  const bool bgOn = backgroundParams_.value("mode", std::string("none")) != "none";
  state_.set(path("/stages/matte/state"), matteGuard_.stateName(matteOn));
  state_.set(path("/stages/matte/error"), errorToJson(matteOn ? matteGuard_.error() : std::nullopt));
  state_.set(path("/stages/background/state"), backgroundGuard_.stateName(bgOn));
  state_.set(path("/stages/background/error"), errorToJson(bgOn ? backgroundGuard_.error() : std::nullopt));
```
(`matteGuard_` is read here from the tick thread too — `publishStatus` already runs on the tick thread under the engine lock, so no extra locking.)

- [ ] **Step 5: Drive the halves from the engine**

In `src/engine/engine.cpp`, replace the per-camera loop inside `run()` (the block that calls `cam->tick(...)`) with:
```cpp
      // Half 1: every camera picks and converts.
      for (auto& cam : cameras_) {
        try {
          cam->beginTick(now, slate_);
        } catch (const std::exception& e) {
          state_.set("/cameras/" + cam->config().id + "/tickError", e.what());
        }
      }
      // (M2 Task 4: one batched matte call goes here.)
      // Half 2: stages, readback, fan-out.
      for (auto& cam : cameras_) {
        const auto& id = cam->config().id;
        try {
          cam->endTick(index, sched, now, nullptr);
        } catch (const std::exception& e) {
          state_.set("/cameras/" + id + "/tickError", e.what());
        }
        const std::string phase = toString(cam->stats().phase);
        auto& last = lastPhase_[id];
        if (last != phase) {
          last = phase;
          state_.set("/cameras/" + id + "/phase", phase);
        }
        frames.emplace_back(id, cam->lastFrame());
      }
```

- [ ] **Step 6: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "StageGuard*,Engine*"`
Expected: 4 StageGuard cases and all 8 M1 Engine cases pass. Then the whole suite once.

- [ ] **Step 7: Commit**

```bash
git add src tests CMakeLists.txt
git commit -m "refactor(engine): split the camera tick around a matte batch; add stage guard"
```

### Task 3: Compositor — alpha refine and the background modes

**Files:**
- Create: `src/gpu/compositor.h`, `src/gpu/compositor.cpp`
- Create: `src/gpu/shaders/alpha_threshold.hlsl`, `alpha_morph.hlsl`, `alpha_blur.hlsl`, `bg_downsample.hlsl`, `bg_blur.hlsl`, `bg_composite.hlsl`
- Modify: `CMakeLists.txt` (six `bc_shader(... cs_5_0)` lines; `src/gpu/compositor.cpp` in `bc_gpu`; test)
- Test: `tests/compositor_test.cpp`

**Interfaces:**
- Consumes: `GpuDevice`, `TexturePool`, `TextureRef`, `Converter` (tests), `kLinearFormat` (M1 Task 5).
- Produces:
  - `bc::MatteRefine { float choke = 0, feather = 1, threshold = 0.5f; }`, `bc::MatteRefine bc::refineFrom(const nlohmann::json& matteParams)`.
  - `enum class bc::BackgroundMode { None, Keyed, Blur, Replace }`, `bc::BackgroundSettings { BackgroundMode mode = None; float blur = 12; std::array<float,3> colorLinear{}; TextureRef image; }`, `bc::BackgroundSettings bc::backgroundFrom(const nlohmann::json& backgroundParams)` (does **not** load `image`; the camera loads it), `std::array<float,3> bc::hexToLinear(std::string_view)`.
  - `bc::Compositor(GpuDevice&, TexturePool&)`: `TextureRef refine(const GpuTexture& rawAlpha, uint32_t w, uint32_t h, const MatteRefine&)` → R16F alpha at `w×h`; `TextureRef apply(const TextureRef& frame, const TextureRef& alpha, const BackgroundSettings&)` → RGBA16F linear.

`apply` rules: `None` returns `frame` itself. A null `alpha` (no matte this tick) returns `frame` itself for every mode — the camera stays opaque. `Keyed` → `(fg.rgb, alpha)` (straight alpha). `Blur` → masked blur: quarter-resolution sum of `fg·(1−a)` and `(1−a)`, separable Gaussian with σ = `blur × height/1080 / 4`, normalised, then `lerp(bg, fg, a)` with alpha 1. `Replace` → `lerp(image or colour, fg, a)` with alpha 1. Colours are `#rrggbb` sRGB-ish, linearised with the pipeline's gamma 2.4.

Refine rules: threshold → `smoothstep(t−0.1, t+0.1, raw)` resampled to the frame size; choke `c` > 0 erodes (min filter, radius c), < 0 dilates (max, radius −c); feather = separable Gaussian σ = feather px (0 = none). All radii in frame pixels.

- [ ] **Step 1: Write the failing tests**

`tests/compositor_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/device.h"

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};

  // 16x4 R8 alpha: columns < split are `left`, the rest `right`.
  bc::TextureRef alphaStep(uint8_t left, uint8_t right, uint32_t w = 16, uint32_t h = 4, uint32_t split = 8) {
    std::vector<uint8_t> px(w * h);
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) px[y * w + x] = x < split ? left : right;
    auto t = pool.acquire(w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    auto lock = gpu->lock();
    gpu->context()->UpdateSubresource(t->tex.Get(), 0, nullptr, px.data(), w, 0);
    return t;
  }
  // Solid linear frame, or a two-colour split frame.
  bc::TextureRef frame(std::array<float, 4> c, uint32_t w = 16, uint32_t h = 4) { return conv.solid(w, h, c); }
  bc::TextureRef split(std::array<uint8_t, 4> leftBgra, std::array<uint8_t, 4> rightBgra, uint32_t w, uint32_t h) {
    std::vector<uint8_t> px(w * h * 4);
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        auto& c = x < w / 2 ? leftBgra : rightBgra;
        std::copy(c.begin(), c.end(), px.begin() + (y * w + x) * 4);
      }
    return conv.toLinear(*conv.uploadBgra8(px.data(), w, h, w * 4), bc::PixelLayout::BGRA8, true);
  }
  bc::CpuImage read(const bc::TextureRef& t) { return conv.readback(*conv.toBgra8(*t, t->width, t->height)); }
  // Alpha of a refined matte, through a keyed white frame.
  std::vector<int> alphaRow(const bc::TextureRef& alpha) {
    bc::BackgroundSettings keyed{bc::BackgroundMode::Keyed};
    auto img = read(comp.apply(frame({1, 1, 1, 1}, alpha->width, alpha->height), alpha, keyed));
    std::vector<int> row;
    for (uint32_t x = 0; x < img.width; ++x) row.push_back(img.bgra[x * 4 + 3]);
    return row;
  }
};
}  // namespace

TEST_CASE("hexToLinear and parameter parsing") {
  auto w = bc::hexToLinear("#ffffff");
  CHECK(w[0] == doctest::Approx(1.0));
  auto g = bc::hexToLinear("#808080");
  CHECK(g[1] == doctest::Approx(0.191).epsilon(0.03));
  auto bg = bc::backgroundFrom({{"mode", "blur"}, {"blur", 20}, {"color", "#000000"}, {"image", ""}});
  CHECK(bg.mode == bc::BackgroundMode::Blur);
  CHECK(bg.blur == doctest::Approx(20));
  auto r = bc::refineFrom({{"enabled", true}, {"quality", "quality"}, {"choke", -2}, {"feather", 1.5}, {"threshold", 0.4}});
  CHECK(r.choke == doctest::Approx(-2));
  CHECK(r.feather == doctest::Approx(1.5));
  CHECK(r.threshold == doctest::Approx(0.4));
}

TEST_CASE("Compositor: refine with no choke or feather keeps a hard edge") {
  Rig r;
  auto a = r.comp.refine(*r.alphaStep(0, 255), 16, 4, {0, 0, 0.5f});
  auto row = r.alphaRow(a);
  CHECK(row[7] <= 2);
  CHECK(row[8] >= 253);
}

TEST_CASE("Compositor: positive choke shrinks the person, negative grows it") {
  Rig r;
  auto shrunk = r.alphaRow(r.comp.refine(*r.alphaStep(0, 255), 16, 4, {2, 0, 0.5f}));
  CHECK(shrunk[9] <= 2);
  CHECK(shrunk[10] >= 253);
  auto grown = r.alphaRow(r.comp.refine(*r.alphaStep(0, 255), 16, 4, {-2, 0, 0.5f}));
  CHECK(grown[6] >= 253);
  CHECK(grown[5] <= 2);
}

TEST_CASE("Compositor: feather softens the edge") {
  Rig r;
  auto row = r.alphaRow(r.comp.refine(*r.alphaStep(0, 255), 16, 4, {0, 2, 0.5f}));
  CHECK(row[7] > 25);
  CHECK(row[7] < 128);
  CHECK(row[8] > 128);
  CHECK(row[8] < 230);
  CHECK(row[0] <= 2);
  CHECK(row[15] >= 253);
}

TEST_CASE("Compositor: none and missing alpha leave the frame untouched") {
  Rig r;
  auto f = r.frame({0.3f, 0.3f, 0.3f, 1});
  CHECK(r.comp.apply(f, r.alphaStep(0, 255), {bc::BackgroundMode::None}) == f);
  CHECK(r.comp.apply(f, nullptr, {bc::BackgroundMode::Blur}) == f);
}

TEST_CASE("Compositor: replace with a colour behind the person") {
  Rig r;
  auto alpha = r.comp.refine(*r.alphaStep(0, 255), 16, 4, {0, 0, 0.5f});
  bc::BackgroundSettings s{bc::BackgroundMode::Replace};
  s.colorLinear = bc::hexToLinear("#ff0000");
  auto img = r.read(r.comp.apply(r.frame({0, 1, 0, 1}), alpha, s));
  CHECK(img.bgra[2] >= 253);         // x=0: red background
  CHECK(img.bgra[1] <= 2);
  CHECK(img.bgra[15 * 4 + 1] >= 253);  // x=15: green person
  CHECK(img.bgra[15 * 4 + 2] <= 2);
  CHECK(img.bgra[3] == 255);         // replace is opaque
}

TEST_CASE("Compositor: masked blur has no halo of person colour") {
  Rig r;
  const uint32_t w = 128, h = 32;
  auto f = r.split({0, 0, 255, 255}, {0, 255, 0, 255}, w, h);  // left red background, right green person
  auto alpha = r.comp.refine(*r.alphaStep(0, 255, w, h, w / 2), w, h, {0, 0, 0.5f});
  bc::BackgroundSettings s{bc::BackgroundMode::Blur};
  s.blur = 8 * 1080.0f / h;  // σ = 8 px at this height
  auto img = r.read(r.comp.apply(f, alpha, s));
  for (uint32_t x = 0; x < w / 2; ++x) {
    const auto* p = &img.bgra[(h / 2 * w + x) * 4];
    CHECK(p[1] <= 6);    // no green bled into the background
    CHECK(p[2] >= 249);  // still red
  }
  const auto* person = &img.bgra[(h / 2 * w + w - 1) * 4];
  CHECK(person[1] >= 253);
}
```

Add it to `bc_tests`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Compositor*,hexToLinear*"`
Expected: build FAILS — `gpu/compositor.h` not found.

- [ ] **Step 3: Write the shaders**

All six share this constant buffer (declare it at the top of each file):
```hlsl
cbuffer Params : register(b0) {
  float4 gA;
  float4 gB;
  uint2 gOutSize;
  uint2 gSrcSize;
};
```

`src/gpu/shaders/alpha_threshold.hlsl` (gA.x = threshold):
```hlsl
Texture2D<float> gRaw : register(t0);
RWTexture2D<float> gOut : register(u0);
SamplerState gLinear : register(s0);
// + Params

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(gOutSize);
  float raw = gRaw.SampleLevel(gLinear, uv, 0);
  gOut[id.xy] = smoothstep(gA.x - 0.1, gA.x + 0.1, raw);
}
```

`src/gpu/shaders/alpha_morph.hlsl` (gA.xy = direction, gA.z = radius, gA.w = 1 dilate / 0 erode):
```hlsl
Texture2D<float> gSrc : register(t0);
RWTexture2D<float> gOut : register(u0);
// + Params

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  int r = (int)gA.z;
  bool dilate = gA.w > 0.5;
  float v = dilate ? 0.0 : 1.0;
  for (int i = -r; i <= r; ++i) {
    int2 p = clamp(int2(id.xy) + int2(gA.xy) * i, int2(0, 0), int2(gOutSize) - 1);
    float s = gSrc[p];
    v = dilate ? max(v, s) : min(v, s);
  }
  gOut[id.xy] = v;
}
```

`src/gpu/shaders/alpha_blur.hlsl` (gA.xy = direction, gA.z = sigma):
```hlsl
Texture2D<float> gSrc : register(t0);
RWTexture2D<float> gOut : register(u0);
// + Params

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float sigma = gA.z;
  if (sigma < 0.01) {
    gOut[id.xy] = gSrc[id.xy];
    return;
  }
  int r = min((int)ceil(sigma * 3.0), 24);
  float sum = 0, wsum = 0;
  for (int i = -r; i <= r; ++i) {
    int2 p = clamp(int2(id.xy) + int2(gA.xy) * i, int2(0, 0), int2(gOutSize) - 1);
    float w = exp(-(i * i) / (2.0 * sigma * sigma));
    sum += w * gSrc[p];
    wsum += w;
  }
  gOut[id.xy] = sum / wsum;
}
```

`src/gpu/shaders/bg_downsample.hlsl` (out = quarter size; gSrcSize = frame size):
```hlsl
Texture2D<float4> gFrame : register(t0);
Texture2D<float> gAlpha : register(t1);
RWTexture2D<float4> gOut : register(u0);
// + Params

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float4 acc = 0;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      int2 p = min(int2(id.xy) * 4 + int2(x, y), int2(gSrcSize) - 1);
      float w = 1.0 - gAlpha[p];
      acc += float4(gFrame[p].rgb * w, w);
    }
  gOut[id.xy] = acc / 16.0;  // premultiplied background colour + background weight
}
```

`src/gpu/shaders/bg_blur.hlsl` (gA.xy = direction, gA.z = sigma in quarter-res pixels):
```hlsl
Texture2D<float4> gSrc : register(t0);
RWTexture2D<float4> gOut : register(u0);
// + Params

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float sigma = max(gA.z, 0.5);
  int r = min((int)ceil(sigma * 3.0), 40);
  float4 sum = 0;
  float wsum = 0;
  for (int i = -r; i <= r; ++i) {
    int2 p = clamp(int2(id.xy) + int2(gA.xy) * i, int2(0, 0), int2(gOutSize) - 1);
    float w = exp(-(i * i) / (2.0 * sigma * sigma));
    sum += w * gSrc[p];
    wsum += w;
  }
  gOut[id.xy] = sum / wsum;
}
```

`src/gpu/shaders/bg_composite.hlsl` (gA.x = mode: 1 keyed, 2 blur, 3 replace colour, 4 replace image; gB.rgb = colour):
```hlsl
Texture2D<float4> gFrame : register(t0);
Texture2D<float> gAlpha : register(t1);
Texture2D<float4> gBg : register(t2);
RWTexture2D<float4> gOut : register(u0);
SamplerState gLinear : register(s0);
// + Params

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float4 fg = gFrame[id.xy];
  float a = gAlpha[id.xy];
  int mode = (int)gA.x;
  if (mode == 1) {
    gOut[id.xy] = float4(fg.rgb, a);
    return;
  }
  float2 uv = (float2(id.xy) + 0.5) / float2(gOutSize);
  float3 bg;
  if (mode == 2) {
    float4 b = gBg.SampleLevel(gLinear, uv, 0);
    bg = b.w > 1e-4 ? b.rgb / b.w : fg.rgb;
  } else if (mode == 4) {
    bg = gBg.SampleLevel(gLinear, uv, 0).rgb;
  } else {
    bg = gB.rgb;
  }
  gOut[id.xy] = float4(lerp(bg, fg.rgb, a), 1.0);
}
```

In `CMakeLists.txt`, after the M1 shader lines:
```cmake
foreach(s alpha_threshold alpha_morph alpha_blur bg_downsample bg_blur bg_composite)
  bc_shader(${s} cs_5_0)
endforeach()
```
(Put this **before** `add_library(bc_gpu ...)` so `${BC_SHADER_HEADERS}` includes them.)

- [ ] **Step 4: Implement the compositor**

`src/gpu/compositor.h`:
```cpp
#pragma once
#include <array>
#include <string_view>

#include <nlohmann/json.hpp>

#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
struct MatteRefine {
  float choke = 0;
  float feather = 1;
  float threshold = 0.5f;
};
MatteRefine refineFrom(const nlohmann::json& matteParams);

enum class BackgroundMode { None, Keyed, Blur, Replace };
struct BackgroundSettings {
  BackgroundMode mode = BackgroundMode::None;
  float blur = 12;                         // px at 1080p
  std::array<float, 3> colorLinear{0, 0, 0};
  TextureRef image;                        // linear RGBA16F, or null = use colorLinear
};
BackgroundSettings backgroundFrom(const nlohmann::json& backgroundParams);
std::array<float, 3> hexToLinear(std::string_view hex);

// spec §5.1 matte controls + §5.2 background modes, on the shared device.
class Compositor {
 public:
  Compositor(GpuDevice& gpu, TexturePool& pool);
  TextureRef refine(const GpuTexture& rawAlpha, uint32_t w, uint32_t h, const MatteRefine& r);
  TextureRef apply(const TextureRef& frame, const TextureRef& alpha, const BackgroundSettings& s);

 private:
  struct alignas(16) Params {
    float a[4];
    float b[4];
    uint32_t outW, outH, srcW, srcH;
  };
  void run(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
           ID3D11UnorderedAccessView* uav, const Params& p);
  TextureRef alphaTex(uint32_t w, uint32_t h);
  GpuDevice& gpu_;
  TexturePool& pool_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> threshold_, morph_, alphaBlur_, down_, bgBlur_, composite_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};
}  // namespace bc
```

`src/gpu/compositor.cpp`:
```cpp
#include "gpu/compositor.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "alpha_blur.h"
#include "alpha_morph.h"
#include "alpha_threshold.h"
#include "bg_blur.h"
#include "bg_composite.h"
#include "bg_downsample.h"
#include "gpu/converter.h"

namespace bc {
MatteRefine refineFrom(const nlohmann::json& p) {
  return {p.value("choke", 0.0f), p.value("feather", 1.0f), p.value("threshold", 0.5f)};
}

std::array<float, 3> hexToLinear(std::string_view hex) {
  auto byte = [&](size_t i) { return std::stoi(std::string(hex.substr(i, 2)), nullptr, 16) / 255.0f; };
  if (hex.size() != 7 || hex[0] != '#') return {0, 0, 0};
  return {std::pow(byte(1), 2.4f), std::pow(byte(3), 2.4f), std::pow(byte(5), 2.4f)};
}

BackgroundSettings backgroundFrom(const nlohmann::json& p) {
  BackgroundSettings s;
  const auto mode = p.value("mode", std::string("none"));
  s.mode = mode == "keyed" ? BackgroundMode::Keyed
         : mode == "blur" ? BackgroundMode::Blur
         : mode == "replace" ? BackgroundMode::Replace
                             : BackgroundMode::None;
  s.blur = p.value("blur", 12.0f);
  s.colorLinear = hexToLinear(p.value("color", std::string("#101418")));
  return s;
}

Compositor::Compositor(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {
  auto* d = gpu_.device();
  auto mk = [&](const BYTE* code, size_t n, Microsoft::WRL::ComPtr<ID3D11ComputeShader>& out) {
    if (FAILED(d->CreateComputeShader(code, n, nullptr, &out))) throw std::runtime_error("CreateComputeShader failed");
  };
  mk(g_alpha_threshold, sizeof(g_alpha_threshold), threshold_);
  mk(g_alpha_morph, sizeof(g_alpha_morph), morph_);
  mk(g_alpha_blur, sizeof(g_alpha_blur), alphaBlur_);
  mk(g_bg_downsample, sizeof(g_bg_downsample), down_);
  mk(g_bg_blur, sizeof(g_bg_blur), bgBlur_);
  mk(g_bg_composite, sizeof(g_bg_composite), composite_);
  D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  if (FAILED(d->CreateBuffer(&bd, nullptr, &cb_))) throw std::runtime_error("CreateBuffer failed");
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(d->CreateSamplerState(&sd, &sampler_))) throw std::runtime_error("CreateSamplerState failed");
}

void Compositor::run(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                     ID3D11UnorderedAccessView* uav, const Params& p) {
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  D3D11_MAPPED_SUBRESOURCE m{};
  ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
  std::memcpy(m.pData, &p, sizeof(p));
  ctx->Unmap(cb_.Get(), 0);
  ctx->CSSetShader(cs, nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx->CSSetSamplers(0, 1, sampler_.GetAddressOf());
  ctx->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.begin());
  ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx->Dispatch((p.outW + 7) / 8, (p.outH + 7) / 8, 1);
  ID3D11ShaderResourceView* nulls[3] = {};
  ID3D11UnorderedAccessView* nullUav = nullptr;
  ctx->CSSetShaderResources(0, 3, nulls);
  ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

TextureRef Compositor::alphaTex(uint32_t w, uint32_t h) {
  return pool_.acquire(w, h, DXGI_FORMAT_R16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
}

TextureRef Compositor::refine(const GpuTexture& raw, uint32_t w, uint32_t h, const MatteRefine& r) {
  Params p{};
  p.outW = w;
  p.outH = h;
  p.srcW = raw.width;
  p.srcH = raw.height;
  auto cur = alphaTex(w, h);
  p.a[0] = r.threshold;
  run(threshold_.Get(), {raw.srv.Get()}, cur->uav.Get(), p);

  const int radius = static_cast<int>(std::lround(std::abs(r.choke)));
  if (radius > 0) {
    const float dilate = r.choke < 0 ? 1.0f : 0.0f;
    for (auto [dx, dy] : {std::pair{1.0f, 0.0f}, std::pair{0.0f, 1.0f}}) {
      auto next = alphaTex(w, h);
      Params m = p;
      m.a[0] = dx;
      m.a[1] = dy;
      m.a[2] = static_cast<float>(radius);
      m.a[3] = dilate;
      run(morph_.Get(), {cur->srv.Get()}, next->uav.Get(), m);
      cur = next;
    }
  }
  if (r.feather > 0.01f) {
    for (auto [dx, dy] : {std::pair{1.0f, 0.0f}, std::pair{0.0f, 1.0f}}) {
      auto next = alphaTex(w, h);
      Params b = p;
      b.a[0] = dx;
      b.a[1] = dy;
      b.a[2] = r.feather;
      run(alphaBlur_.Get(), {cur->srv.Get()}, next->uav.Get(), b);
      cur = next;
    }
  }
  return cur;
}

TextureRef Compositor::apply(const TextureRef& frame, const TextureRef& alpha, const BackgroundSettings& s) {
  if (s.mode == BackgroundMode::None || !alpha) return frame;
  const uint32_t w = frame->width, h = frame->height;
  auto out = pool_.acquire(w, h, Converter::kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = w;
  p.outH = h;
  p.srcW = w;
  p.srcH = h;
  ID3D11ShaderResourceView* bgSrv = nullptr;
  TextureRef keepAlive;
  switch (s.mode) {
    case BackgroundMode::Keyed:
      p.a[0] = 1;
      break;
    case BackgroundMode::Blur: {
      const uint32_t qw = (w + 3) / 4, qh = (h + 3) / 4;
      auto q = pool_.acquire(qw, qh, Converter::kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
      Params d{};
      d.outW = qw;
      d.outH = qh;
      d.srcW = w;
      d.srcH = h;
      run(down_.Get(), {frame->srv.Get(), alpha->srv.Get()}, q->uav.Get(), d);
      const float sigma = s.blur * static_cast<float>(h) / 1080.0f / 4.0f;
      for (auto [dx, dy] : {std::pair{1.0f, 0.0f}, std::pair{0.0f, 1.0f}}) {
        auto next = pool_.acquire(qw, qh, Converter::kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Params b = d;
        b.a[0] = dx;
        b.a[1] = dy;
        b.a[2] = sigma;
        run(bgBlur_.Get(), {q->srv.Get()}, next->uav.Get(), b);
        q = next;
      }
      keepAlive = q;
      bgSrv = q->srv.Get();
      p.a[0] = 2;
      break;
    }
    case BackgroundMode::Replace:
      if (s.image) {
        keepAlive = s.image;
        bgSrv = s.image->srv.Get();
        p.a[0] = 4;
      } else {
        p.a[0] = 3;
        p.b[0] = s.colorLinear[0];
        p.b[1] = s.colorLinear[1];
        p.b[2] = s.colorLinear[2];
      }
      break;
    case BackgroundMode::None:
      return frame;
  }
  run(composite_.Get(), {frame->srv.Get(), alpha->srv.Get(), bgSrv}, out->uav.Get(), p);
  return out;
}
}  // namespace bc
```

Cost note: blur at quarter resolution keeps 4 cameras at 1080p60 inside budget; the alpha refine runs 1 + up to 4 passes at frame size with ≤ 17 or ≤ 49 taps.

- [ ] **Step 5: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Compositor*,hexToLinear*"`
Expected: 7 cases pass (also once with `BC_FORCE_WARP=1`).

- [ ] **Step 6: Commit**

```bash
git add src/gpu tests CMakeLists.txt
git commit -m "feat(gpu): alpha refine (threshold, choke, feather) and keyed/blur/replace compositor"
```

### Task 4: Matte service — backend interface, batched matte per tick, stages in the camera, alpha out

**Files:**
- Create: `src/engine/matte.h`, `src/engine/matte.cpp`, `tests/support/test_matte.h`
- Modify: `src/core/stage_guard.h/.cpp` (an `ErrorInfo` overload of `failed`), `src/engine/camera.h/.cpp`, `src/engine/engine.h/.cpp`, `src/engine/engine_main.cpp`, `tests/support/fake_output.h` (centre pixel), `tests/engine_test.cpp` and `tests/e2e_m1_test.cpp` (new `Engine` constructor), `CMakeLists.txt`
- Test: `tests/engine_matte_test.cpp`

**Interfaces:**
- Consumes: `StageGuard` (Task 2), `Compositor`, `refineFrom`, `backgroundFrom`, `BackgroundMode` (Task 3), `loadImageBgra`, `Converter` (M1).
- Produces:
  - `enum class bc::MatteQuality { Quality, Performance }`, `bc::MatteJob { std::string cameraId; TextureRef frame; MatteQuality quality; TextureRef alpha; }`.
  - `class bc::MatteBackend { virtual std::string name() const = 0; virtual void process(std::vector<MatteJob*>& jobs) = 0; virtual void forget(const std::string& cameraId) = 0; }` — `process` fills every `alpha` (any size, R8/R16F) or throws (the whole batch is then bypassed).
  - `bc::MatteBackendChoice { std::unique_ptr<MatteBackend> backend; std::string unavailableReason; }`, `bc::MatteBackendChoice bc::makeMatteBackend(GpuDevice&, TexturePool&, Converter&)` (Task 5 adds the Maxine branch).
  - `void bc::StageGuard::failed(ErrorInfo e, time_point now)`.
  - **Changed constructors:** `bc::Engine(Converter&, Compositor&, StateStore&, AdapterFactory&)`, `bc::Camera(CameraConfig, AdapterFactory&, Converter&, Compositor&, StateStore&, SyncPolicy)`; `void Engine::setMatteBackend(std::unique_ptr<MatteBackend>, std::string unavailableReason)` (call before `start()`); `TextureRef Camera::tickFrame() const`.
  - State: `/engine/matteBackend` = backend name or `""`; stage states/errors per Task 2 paths; a missing backend shows `{code: "matte-unavailable", message: <reason>}`.

Per-tick flow (spec §3.4): half 1 for every camera → collect `MatteJob`s from cameras whose `wantsMatte(now)` → **one** `process()` call → each camera's guard gets `succeeded(batchTime, 0.6 × period)` or `failed` → half 2 with that camera's alpha (or null). In half 2: refine the alpha at the frame size, then the background stage (guarded, budget = period / 4 per camera), `hasAlpha = (mode == keyed && an alpha was applied)`. Matte bypassed → no alpha → the frame passes through opaque.

Background image: on `reconfigure`/construction, if `mode == "replace"` and `image` is non-empty and changed, load it (WIC → upload → linear). A load failure keeps the colour fallback and shows `{code: "background-image", message}` as the background stage error while the stage keeps running.

- [ ] **Step 1: Test helpers**

Extend `tests/support/fake_output.h`: add `std::array<uint8_t, 4> centerPixel;` to `Seen` (after `firstPixel`, initialised `{0,0,0,0}` in the aggregate) and fill it in `deliver` when a CPU image is present:
```cpp
      const size_t c = (static_cast<size_t>(f.bgra->height / 2) * f.bgra->width + f.bgra->width / 2) * 4;
      std::copy_n(f.bgra->bgra.begin() + c, 4, s.centerPixel.begin());
```

`tests/support/test_matte.h`:
```cpp
#pragma once
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/matte.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc::test {
// Alpha = 1 inside a centred rectangle (the "person"), 0 elsewhere. Can fail or be slow.
class TestMatte final : public bc::MatteBackend {
 public:
  TestMatte(bc::GpuDevice& gpu, bc::TexturePool& pool) {
    std::vector<uint8_t> px(64 * 36, 0);
    for (int y = 9; y < 27; ++y)
      for (int x = 16; x < 48; ++x) px[y * 64 + x] = 255;
    rect_ = pool.acquire(64, 36, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    auto lock = gpu.lock();
    gpu.context()->UpdateSubresource(rect_->tex.Get(), 0, nullptr, px.data(), 64, 0);
  }
  std::atomic<bool> fail{false};
  std::atomic<int> delayMs{0};
  std::string name() const override { return "test"; }
  void process(std::vector<bc::MatteJob*>& jobs) override {
    if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs.load()));
    if (fail) throw std::runtime_error("test matte failed");
    std::lock_guard l(m);
    batchSizes.push_back(jobs.size());
    for (auto* j : jobs) j->alpha = rect_;
  }
  void forget(const std::string& id) override {
    std::lock_guard l(m);
    forgotten.push_back(id);
  }
  std::mutex m;
  std::vector<size_t> batchSizes;
  std::vector<std::string> forgotten;

 private:
  bc::TextureRef rect_;
};
}  // namespace bc::test
```

- [ ] **Step 2: Write the failing tests**

`tests/engine_matte_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/engine.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/test_matte.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{conv, comp, state, factory};
  bc::test::TestMatte* matte = nullptr;
  explicit Rig(bool withMatte = true) {
    if (withMatte) {
      auto m = std::make_unique<bc::test::TestMatte>(*gpu, pool);
      matte = m.get();
      engine.setMatteBackend(std::move(m), "");
    } else {
      engine.setMatteBackend(nullptr, "background removal needs an NVIDIA RTX GPU");
    }
  }
  ~Rig() { engine.stop(); }
  json at(const std::string& ptr) {
    auto s = state.snapshot();
    auto p = json::json_pointer(ptr);
    return s.contains(p) ? s.at(p) : json();
  }
};

bc::CameraConfig cam(const std::string& id, const std::string& mode, json inParams = json::object()) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = inParams;
  c.input.params["tag"] = id;
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  c.stages = {{"matte", bc::normalizeStageParams("matte", {{"enabled", true}, {"feather", 0}})},
              {"background", bc::normalizeStageParams("background", {{"mode", mode}})}};
  return c;
}

std::chrono::milliseconds maxGap(const std::vector<bc::test::FakeOutput::Seen>& seen) {
  std::chrono::milliseconds g{0};
  for (size_t i = 1; i < seen.size(); ++i) {
    g = std::max(g, std::chrono::duration_cast<std::chrono::milliseconds>(seen[i].at - seen[i - 1].at));
  }
  return g;
}
}  // namespace

TEST_CASE("Matte: keyed output carries the person's alpha") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", "keyed"));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  auto s = r.factory.output("o-a")->seen().back();
  CHECK(s.firstPixel[3] == 0);     // corner: background
  CHECK(s.centerPixel[3] == 255);  // centre: person
  CHECK(r.at("/cameras/a/stages/matte/state") == "active");
  CHECK(r.at("/cameras/a/stages/background/state") == "active");
}

TEST_CASE("Matte: blur and replace outputs stay opaque") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", "blur"));
  show.cameras.push_back(cam("b", "replace"));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(600ms);
  CHECK(r.factory.output("o-a")->seen().back().firstPixel[3] == 255);
  CHECK(r.factory.output("o-b")->seen().back().firstPixel[3] == 255);
  REQUIRE_FALSE(r.matte->batchSizes.empty());
  CHECK(r.matte->batchSizes.back() == 2);  // one batched call for both cameras
}

TEST_CASE("Matte: a failing backend bypasses to opaque passthrough, then recovers") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", "keyed"));
  r.engine.apply(show);
  r.matte->fail = true;
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  CHECK(r.at("/cameras/a/stages/matte/state") == "bypassed");
  CHECK(r.at("/cameras/a/stages/matte/error/code") == "stage-failed");
  auto seen = r.factory.output("o-a")->seen();
  CHECK(maxGap(seen) < 50ms);
  CHECK(seen.back().firstPixel[3] == 255);  // opaque, not transparent, not black
  r.matte->fail = false;
  std::this_thread::sleep_for(5500ms);
  CHECK(r.at("/cameras/a/stages/matte/state") == "active");
  CHECK(r.factory.output("o-a")->seen().back().firstPixel[3] == 0);
}

TEST_CASE("Matte: no backend reports matte-unavailable and frames pass through") {
  Rig r(false);
  bc::Show show;
  show.cameras.push_back(cam("a", "keyed"));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  CHECK(r.at("/cameras/a/stages/matte/error/code") == "matte-unavailable");
  CHECK(r.at("/cameras/a/stages/matte/error/message").get<std::string>().find("NVIDIA") != std::string::npos);
  CHECK(r.factory.output("o-a")->seen().back().firstPixel[3] == 255);
}

TEST_CASE("Matte: a slow backend is bypassed for overrun and the tick stops overrunning") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", "keyed"));
  r.engine.apply(show);
  r.matte->delayMs = 25;  // > 60% of a 60 fps tick
  r.engine.start();
  std::this_thread::sleep_for(1500ms);
  CHECK(r.at("/cameras/a/stages/matte/error/code") == "stage-overrun");
  const auto o1 = r.engine.stats().overruns;
  std::this_thread::sleep_for(1500ms);  // still inside the 5 s retry pause
  CHECK(r.engine.stats().overruns == o1);
}

TEST_CASE("Matte: removing a camera releases its matte state; mixed sizes each get their own alpha") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", "keyed"));
  show.cameras.push_back(cam("b", "keyed", {{"width", 32}, {"height", 18}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(600ms);
  auto b = r.factory.output("o-b")->seen().back();
  CHECK(b.width == 32);
  CHECK(b.centerPixel[3] == 255);
  CHECK(b.firstPixel[3] == 0);
  show.cameras.pop_back();
  r.engine.apply(show);
  std::lock_guard l(r.matte->m);
  CHECK(r.matte->forgotten == std::vector<std::string>{"b"});
}
```

Update the M1 rigs for the new constructor: in `tests/engine_test.cpp` and `tests/e2e_m1_test.cpp` add `#include "gpu/compositor.h"`, a `bc::Compositor comp{*gpu, pool};` member (or local, declared after `conv`) and construct `bc::Engine engine{conv, comp, state, factory};` / `bc::Engine engine(conv, comp, state, factory);`.

Add `tests/engine_matte_test.cpp` to `bc_tests`; add `src/engine/matte.cpp` to `bc_engine`.

- [ ] **Step 3: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Matte*"`
Expected: build FAILS — `engine/matte.h` not found.

- [ ] **Step 4: Implement the matte interface**

`src/engine/matte.h`:
```cpp
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
enum class MatteQuality { Quality, Performance };

struct MatteJob {
  std::string cameraId;
  TextureRef frame;  // linear RGBA16F, any size
  MatteQuality quality = MatteQuality::Quality;
  TextureRef alpha;  // out: R8 or R16F, any size
};

// spec §5.1: one batched call per tick for every camera that wants a matte.
class MatteBackend {
 public:
  virtual ~MatteBackend() = default;
  virtual std::string name() const = 0;
  virtual void process(std::vector<MatteJob*>& jobs) = 0;  // throws → the whole batch is bypassed
  virtual void forget(const std::string& cameraId) = 0;    // drop per-camera temporal state
};

struct MatteBackendChoice {
  std::unique_ptr<MatteBackend> backend;
  std::string unavailableReason;  // set when backend is null
};
MatteBackendChoice makeMatteBackend(GpuDevice& gpu, TexturePool& pool, Converter& conv);
}  // namespace bc
```

`src/engine/matte.cpp`:
```cpp
#include "engine/matte.h"

namespace bc {
MatteBackendChoice makeMatteBackend(GpuDevice& gpu, TexturePool& pool, Converter& conv) {
  (void)pool;
  (void)conv;
  // Task 5 adds the Maxine branch here.
  return {nullptr, "background removal needs an NVIDIA RTX GPU with the NVIDIA Video Effects SDK; this PC uses " +
                       gpu.info().adapterName};
}
}  // namespace bc
```

`src/core/stage_guard.h/.cpp`: add `void failed(ErrorInfo e, Clock::time_point now);` implemented as `bypass(std::move(e), now);`.

- [ ] **Step 5: Run the stages in the camera**

`src/engine/camera.h`:
- `#include "gpu/compositor.h"`; constructor becomes `Camera(CameraConfig cfg, AdapterFactory& factory, Converter& conv, Compositor& comp, StateStore& state, SyncPolicy policy);`
- public: `TextureRef tickFrame() const { return tickFrame_; }`
- private: `Compositor& comp_;`, `TextureRef bgImage_;`, `std::string bgImagePath_;`, `std::optional<ErrorInfo> bgImageError_;`

`src/engine/camera.cpp`:
- constructor initialises `comp_(comp)`.
- extend `loadStageParams()`:
```cpp
  const std::string mode = backgroundParams_.value("mode", std::string("none"));
  const std::string img = backgroundParams_.value("image", std::string());
  if (mode != "replace" || img.empty()) {
    bgImage_.reset();
    bgImagePath_.clear();
    bgImageError_.reset();
  } else if (img != bgImagePath_) {
    bgImagePath_ = img;
    bgImageError_.reset();
    try {
      auto pic = loadImageBgra(std::filesystem::u8path(img));
      auto up = conv_.uploadBgra8(pic.bgra.data(), pic.width, pic.height, pic.width * 4);
      bgImage_ = conv_.toLinear(*up, PixelLayout::BGRA8, true);
    } catch (const std::exception& e) {
      bgImage_.reset();
      bgImageError_ = ErrorInfo{"background-image", std::string("cannot load the background image; using the colour: ") + e.what()};
    }
  }
```
(add `#include "gpu/image_io.h"` and `<filesystem>`).
- replace the top of `endTick` (the `(void)` lines and `TextureRef frame = tickFrame_; const bool hasAlpha = false;`) with:
```cpp
  TextureRef frame = tickFrame_;
  bool hasAlpha = false;
  TextureRef alpha;
  if (rawAlpha) {
    try {
      alpha = comp_.refine(*rawAlpha, frame->width, frame->height, refineFrom(matteParams_));
    } catch (const std::exception& e) {
      matteGuard_.failed(e.what(), now);
    }
  }
  auto bg = backgroundFrom(backgroundParams_);
  if (bg.mode != BackgroundMode::None && alpha && backgroundGuard_.shouldRun(now)) {
    const auto t0 = Clock::now();
    try {
      bg.image = bgImage_;
      frame = comp_.apply(frame, alpha, bg);
      hasAlpha = bg.mode == BackgroundMode::Keyed;
      // ponytail: a fixed quarter-tick budget per camera; share by camera count if 8+ cameras run.
      backgroundGuard_.succeeded(Clock::now() - t0, sched.rate().period() / 4, now);
    } catch (const std::exception& e) {
      backgroundGuard_.failed(e.what(), now);
      frame = tickFrame_;
      hasAlpha = false;
    }
  }
```
- in `publishStatus`, use the image error when the guard has none:
```cpp
  auto bgErr = backgroundGuard_.error() ? backgroundGuard_.error() : bgImageError_;
  state_.set(path("/stages/background/error"), errorToJson(bgOn ? bgErr : std::nullopt));
```
(replacing the background error line from Task 2).

- [ ] **Step 6: Batch the matte in the engine**

`src/engine/engine.h`: `#include "engine/matte.h"` and `#include "gpu/compositor.h"`; constructor `Engine(Converter& conv, Compositor& comp, StateStore& state, AdapterFactory& factory);`; public `void setMatteBackend(std::unique_ptr<MatteBackend> backend, std::string unavailableReason);`; private `Compositor& comp_; std::unique_ptr<MatteBackend> matte_; std::string matteUnavailable_;`.

`src/engine/engine.cpp`:
- constructor initialises `comp_(comp)`; `apply` constructs cameras with `std::make_unique<Camera>(cfg, factory_, conv_, comp_, state_, policy())`.
- ```cpp
  void Engine::setMatteBackend(std::unique_ptr<MatteBackend> backend, std::string unavailableReason) {
    std::lock_guard l(mutex_);
    matte_ = std::move(backend);
    matteUnavailable_ = std::move(unavailableReason);
    state_.set("/engine/matteBackend", matte_ ? matte_->name() : std::string());
  }
  ```
- in `apply`, when a camera is removed (the `existing` leftovers loop) also call `if (matte_) matte_->forget(id);`.
- in `run()`, replace the `// (M2 Task 4: one batched matte call goes here.)` comment and the second loop's `cam->endTick(index, sched, now, nullptr);` with:
```cpp
      std::vector<MatteJob> jobs;
      std::vector<Camera*> jobCams;
      for (auto& cam : cameras_) {
        if (!cam->wantsMatte(now)) continue;
        const bool perf = cam->matteParams().value("quality", std::string("quality")) == "performance";
        jobs.push_back({cam->config().id, cam->tickFrame(), perf ? MatteQuality::Performance : MatteQuality::Quality, nullptr});
        jobCams.push_back(cam.get());
      }
      std::map<Camera*, TextureRef> alphas;
      if (!jobs.empty() && !matte_) {
        for (auto* c : jobCams) c->matteGuard().failed(ErrorInfo{"matte-unavailable", matteUnavailable_}, now);
      } else if (!jobs.empty()) {
        std::vector<MatteJob*> ptrs;
        for (auto& j : jobs) ptrs.push_back(&j);
        const auto t0 = Clock::now();
        try {
          matte_->process(ptrs);
          const auto took = Clock::now() - t0;
          const auto budget = std::chrono::duration_cast<std::chrono::nanoseconds>(sched.rate().period() * 6 / 10);
          for (size_t i = 0; i < jobs.size(); ++i) {
            if (!jobs[i].alpha) {
              jobCams[i]->matteGuard().failed("the matte backend returned no matte", now);
              continue;
            }
            jobCams[i]->matteGuard().succeeded(took, budget, now);
            if (!jobCams[i]->matteGuard().bypassed()) alphas[jobCams[i]] = jobs[i].alpha;
          }
        } catch (const std::exception& e) {
          for (auto* c : jobCams) c->matteGuard().failed(e.what(), now);
        }
      }
```
and in the half-2 loop call `cam->endTick(index, sched, now, alphas.count(cam.get()) ? alphas[cam.get()] : nullptr);`.

Note: a trial run that overruns is bypassed again by `succeeded`, and its alpha is discarded by the `bypassed()` check, so one slow trial never emits half-processed frames.

- [ ] **Step 7: Wire the real engine**

`src/engine/engine_main.cpp`: after `bc::Converter conv(*gpu, pool);` add `bc::Compositor comp(*gpu, pool);`; construct `bc::Engine engine(conv, comp, state, factory);`; before `engine.apply(...)` add:
```cpp
  auto matte = bc::makeMatteBackend(*gpu, pool, conv);
  engine.setMatteBackend(std::move(matte.backend), matte.unavailableReason);
```
(includes: `gpu/compositor.h`, `engine/matte.h`).

- [ ] **Step 8: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Matte*,Engine*"`
Expected: 6 Matte cases and the 8 Engine cases pass. Then the whole suite.

- [ ] **Step 9: Commit**

```bash
git add src tests CMakeLists.txt
git commit -m "feat(engine): batched matte service with stage guards, background stage and keyed alpha out"
```

### Task 5: Maxine AI Green Screen backend (NVIDIA-only, untested on the build PC)

**Files:**
- Create: `cmake/FindMaxine.cmake`, `src/engine/maxine_matte.h`, `src/engine/maxine_matte.cpp`
- Modify: `CMakeLists.txt` (`BC_WITH_MAXINE`), `src/engine/matte.cpp` (Maxine branch), `tests/support/requires.h/.cpp` (`Req::Maxine`)
- Test: `tests/maxine_matte_test.cpp`

**Interfaces:**
- Consumes: `MatteBackend`, `MatteJob`, `MatteQuality`, `makeMatteBackend` (Task 4); `Converter::toBgra8`, `Converter::readback`, `TexturePool` (M1).
- Produces: `bc::MaxineMatte(GpuDevice&, TexturePool&, Converter&, std::filesystem::path modelDir)` implementing `MatteBackend` (name `"maxine"`); `Req::Maxine` (missing unless `BC_WITH_MAXINE=1` **and** the GPU is NVIDIA); CMake option `BC_WITH_MAXINE`.

SDK facts (from NVIDIA's `VFX-SDK-Samples` v1.3.0.1, `apps/BatchEffectApp/BatchAigsEffectApp.cpp`): headers `nvVideoEffects.h`, `nvVFXGreenScreen.h`, `nvCVImage.h`, `nvCVStatus.h`; effect `NVVFX_FX_GREEN_SCREEN`; setup `NvVFX_CreateEffect`, `NvVFX_SetString(NVVFX_MODEL_DIRECTORY)`, `NvVFX_SetImage(NVVFX_INPUT_IMAGE / NVVFX_OUTPUT_IMAGE, <first image of the batch>)`, `NvVFX_CudaStreamCreate`, `NvVFX_SetCudaStream(NVVFX_CUDA_STREAM)`, `NvVFX_SetU32(NVVFX_MODE)` (0 = quality, 1 = performance), `NvVFX_SetU32(NVVFX_MAX_NUMBER_STREAMS, n)`, `NvVFX_SetU32(NVVFX_MODEL_BATCH, n > 1 ? 8 : 1)`, `NvVFX_Load`, `NvVFX_AllocateState` per stream; per run `NvVFX_SetU32(NVVFX_BATCH_SIZE, k)`, `NvVFX_SetStateObjectHandleArray(NVVFX_STATE, states)`, `NvVFX_Run(eff, 0)`. Batched images are one tall `NvCVImage` (`height × batch`) with views from `NvCVImage_InitView(view, full, 0, height * n, width, height)`. Input BGR U8 chunky on the GPU, output A U8 chunky on the GPU. Errors: `NvCV_GetErrorStringFromCode`. The SDK ships `nvvfx/src/nvVideoEffectsProxy.cpp` and `nvCVImageProxy.cpp`, which load the DLLs at run time — compile them in, no import library needed.

Design: the matte runs at **1280×720** for every camera. Frames are staged through CPU memory (`toBgra8` 1280×720 → `readback` → `NvCVImage_Transfer` CPU BGRA → GPU BGR) and the alpha comes back the same way (GPU A → CPU → R8 texture). ponytail: CPU staging costs ~1–2 ms per camera; switch to `NvCVImage` D3D11 interop if the SDK header provides it and the budget needs it.

- [ ] **Step 1: Find the SDK in CMake**

`cmake/FindMaxine.cmake`:
```cmake
# NVIDIA Video Effects SDK (Maxine) core + the green-screen feature. Optional (spec §7.2).
# Set VFXSDK_ROOT (environment or cache) to the SDK folder.
set(_vfx_root "$ENV{VFXSDK_ROOT}")
if(VFXSDK_ROOT)
  set(_vfx_root "${VFXSDK_ROOT}")
endif()
find_path(MAXINE_INCLUDE_DIR nvVideoEffects.h PATHS "${_vfx_root}/nvvfx/include" NO_DEFAULT_PATH)
find_file(MAXINE_VFX_PROXY nvVideoEffectsProxy.cpp PATHS "${_vfx_root}/nvvfx/src" NO_DEFAULT_PATH)
find_file(MAXINE_IMAGE_PROXY nvCVImageProxy.cpp PATHS "${_vfx_root}/nvvfx/src" NO_DEFAULT_PATH)
file(GLOB _gs_dirs "${_vfx_root}/features/nvvfxgreenscreen*/include")
find_path(MAXINE_GREENSCREEN_INCLUDE_DIR nvVFXGreenScreen.h PATHS ${_gs_dirs} "${_vfx_root}/nvvfx/include" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Maxine DEFAULT_MSG MAXINE_INCLUDE_DIR MAXINE_VFX_PROXY MAXINE_IMAGE_PROXY MAXINE_GREENSCREEN_INCLUDE_DIR)
set(MAXINE_ROOT "${_vfx_root}")
```

In `CMakeLists.txt` next to the NDI option:
```cmake
find_package(Maxine)
option(BC_WITH_MAXINE "Build the Maxine matte backend" ${Maxine_FOUND})
if(BC_WITH_MAXINE AND NOT Maxine_FOUND)
  message(WARNING "BC_WITH_MAXINE=ON but the NVIDIA Video Effects SDK was not found; turning it off")
  set(BC_WITH_MAXINE OFF)
endif()
message(STATUS "BC_WITH_MAXINE=${BC_WITH_MAXINE}")
```
and after `bc_engine`:
```cmake
if(BC_WITH_MAXINE)
  target_sources(bc_engine PRIVATE src/engine/maxine_matte.cpp ${MAXINE_VFX_PROXY} ${MAXINE_IMAGE_PROXY})
  target_include_directories(bc_engine PUBLIC ${MAXINE_INCLUDE_DIR} ${MAXINE_GREENSCREEN_INCLUDE_DIR})
  target_compile_definitions(bc_engine PUBLIC BC_WITH_MAXINE=1 BC_MAXINE_ROOT="${MAXINE_ROOT}")
else()
  target_compile_definitions(bc_engine PUBLIC BC_WITH_MAXINE=0)
endif()
```
If the proxies need extra include folders or definitions (they include `nvCVImage.h` etc.), add exactly what the compiler asks for and ledger it.

- [ ] **Step 2: Requirement and the failing test**

`tests/support/requires.h`: add `Maxine` to `enum class Req`. `requires.cpp`:
```cpp
    case Req::Maxine: {
      reason = "needs BC_WITH_MAXINE (NVIDIA Video Effects SDK) and an NVIDIA GPU";
      if (BC_WITH_MAXINE == 0) return true;
      try {
        return !bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware)->isNvidia();
      } catch (...) {
        return true;
      }
    }
```

`tests/maxine_matte_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_MAXINE
#include "engine/maxine_matte.h"
#include "gpu/device.h"

TEST_CASE("MaxineMatte: a 4-camera batch returns a 1280x720 matte per camera" BC_REQUIRES(bc::test::Req::Maxine)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::MaxineMatte m(*gpu, pool, conv, bc::MaxineMatte::defaultModelDir());
  std::vector<bc::MatteJob> jobs;
  for (int i = 0; i < 4; ++i) jobs.push_back({"cam" + std::to_string(i), conv.solid(1920, 1080, {0.3f, 0.25f, 0.2f, 1}), bc::MatteQuality::Quality, nullptr});
  std::vector<bc::MatteJob*> ptrs;
  for (auto& j : jobs) ptrs.push_back(&j);
  for (int run = 0; run < 3; ++run) m.process(ptrs);
  for (auto& j : jobs) {
    REQUIRE(j.alpha);
    CHECK(j.alpha->width == 1280);
    CHECK(j.alpha->height == 720);
  }
  m.forget("cam0");
  ptrs.erase(ptrs.begin());
  m.process(ptrs);  // a smaller batch after a camera left
}
#else
TEST_CASE("MaxineMatte: a 4-camera batch returns a 1280x720 matte per camera" BC_REQUIRES(bc::test::Req::Maxine)) {}
#endif
```

Add it to `bc_tests`. Run `-Filter "MaxineMatte*"`: expected **skipped** on the AMD build PC (with `BC_WITH_MAXINE=OFF` it is the empty skipped case). Ledger the skip reason.

- [ ] **Step 3: Implement the backend**

`src/engine/maxine_matte.h`:
```cpp
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "engine/matte.h"

namespace bc {
// NVIDIA Maxine AI Green Screen, batched (spec §5.1). NVIDIA RTX only.
// UNTESTED ON HARDWARE until an RTX machine runs tests/maxine_matte_test.cpp (spec §7.2).
class MaxineMatte final : public MatteBackend {
 public:
  static constexpr unsigned kWidth = 1280, kHeight = 720;
  MaxineMatte(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::filesystem::path modelDir);
  ~MaxineMatte() override;
  static std::filesystem::path defaultModelDir();  // %VFXSDK_ROOT%\bin\models, else <exe dir>\models
  std::string name() const override { return "maxine"; }
  void process(std::vector<MatteJob*>& jobs) override;
  void forget(const std::string& cameraId) override;

 private:
  void build(unsigned maxStreams, MatteQuality quality);
  void release();
  GpuDevice& gpu_;
  TexturePool& pool_;
  Converter& conv_;
  std::filesystem::path modelDir_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace bc
```

`src/engine/maxine_matte.cpp`:
```cpp
#include "engine/maxine_matte.h"

#include <windows.h>

#include <cstdlib>
#include <stdexcept>

#include "nvCVImage.h"
#include "nvVFXGreenScreen.h"
#include "nvVideoEffects.h"

namespace bc {
namespace {
void check(NvCV_Status s, const char* what) {
  if (s != NVCV_SUCCESS) throw std::runtime_error(std::string("Maxine ") + what + ": " + NvCV_GetErrorStringFromCode(s));
}
NvCVImage* nth(NvCVImage* full, NvCVImage* view, unsigned n, unsigned height) {
  NvCVImage_InitView(view, full, 0, static_cast<int>(height * n), full->width, height);
  return view;
}
}  // namespace

struct MaxineMatte::Impl {
  NvVFX_Handle eff = nullptr;
  CUstream stream = nullptr;
  NvCVImage src{}, dst{}, tmp{}, cpuIn{}, cpuOut{};
  unsigned maxStreams = 0;
  MatteQuality quality = MatteQuality::Quality;
  std::map<std::string, NvVFX_StateObjectHandle> states;
  std::vector<uint8_t> alphaBytes;
};

std::filesystem::path MaxineMatte::defaultModelDir() {
  char* root = nullptr;
  size_t n = 0;
  _dupenv_s(&root, &n, "VFXSDK_ROOT");
  std::filesystem::path p = root ? std::filesystem::path(root) / "bin" / "models" : std::filesystem::path(BC_MAXINE_ROOT) / "bin" / "models";
  free(root);
  if (std::filesystem::exists(p)) return p;
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path() / "models";
}

MaxineMatte::MaxineMatte(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::filesystem::path modelDir)
    : gpu_(gpu), pool_(pool), conv_(conv), modelDir_(std::move(modelDir)), impl_(std::make_unique<Impl>()) {
  build(4, MatteQuality::Quality);
}

MaxineMatte::~MaxineMatte() { release(); }

void MaxineMatte::release() {
  for (auto& [id, st] : impl_->states) NvVFX_DeallocateState(impl_->eff, st);
  impl_->states.clear();
  if (impl_->eff) NvVFX_DestroyEffect(impl_->eff);
  if (impl_->stream) NvVFX_CudaStreamDestroy(impl_->stream);
  impl_->eff = nullptr;
  impl_->stream = nullptr;
  NvCVImage_Dealloc(&impl_->src);
  NvCVImage_Dealloc(&impl_->dst);
  NvCVImage_Dealloc(&impl_->tmp);
}

void MaxineMatte::build(unsigned maxStreams, MatteQuality quality) {
  release();
  auto& m = *impl_;
  m.maxStreams = maxStreams;
  m.quality = quality;
  check(NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &m.eff), "CreateEffect");
  check(NvCVImage_Alloc(&m.src, kWidth, kHeight * maxStreams, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "alloc input");
  check(NvCVImage_Alloc(&m.dst, kWidth, kHeight * maxStreams, NVCV_A, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "alloc output");
  const std::string models = modelDir_.string();
  check(NvVFX_SetString(m.eff, NVVFX_MODEL_DIRECTORY, models.c_str()), "model directory");
  NvCVImage view;
  check(NvVFX_SetImage(m.eff, NVVFX_INPUT_IMAGE, nth(&m.src, &view, 0, kHeight)), "input image");
  check(NvVFX_SetImage(m.eff, NVVFX_OUTPUT_IMAGE, nth(&m.dst, &view, 0, kHeight)), "output image");
  check(NvVFX_CudaStreamCreate(&m.stream), "stream");
  check(NvVFX_SetCudaStream(m.eff, NVVFX_CUDA_STREAM, m.stream), "set stream");
  check(NvVFX_SetU32(m.eff, NVVFX_MODE, quality == MatteQuality::Performance ? 1u : 0u), "mode");
  check(NvVFX_SetU32(m.eff, NVVFX_MAX_NUMBER_STREAMS, maxStreams), "max streams");
  check(NvVFX_SetU32(m.eff, NVVFX_MODEL_BATCH, maxStreams > 1 ? 8u : 1u), "model batch");
  check(NvVFX_Load(m.eff), "load");
}

void MaxineMatte::forget(const std::string& id) {
  auto it = impl_->states.find(id);
  if (it == impl_->states.end()) return;
  NvVFX_DeallocateState(impl_->eff, it->second);
  impl_->states.erase(it);
}

void MaxineMatte::process(std::vector<MatteJob*>& jobs) {
  if (jobs.empty()) return;
  auto& m = *impl_;
  // ponytail: one quality per batch (the first job's); mixed quality settings follow the first camera.
  const MatteQuality q = jobs.front()->quality;
  if (jobs.size() > m.maxStreams || q != m.quality) build(std::max<unsigned>(4, static_cast<unsigned>(jobs.size())), q);

  std::vector<NvVFX_StateObjectHandle> batchStates;
  NvCVImage view;
  for (unsigned i = 0; i < jobs.size(); ++i) {
    auto& job = *jobs[i];
    auto& st = m.states[job.cameraId];
    if (!st) check(NvVFX_AllocateState(m.eff, &st), "allocate state");
    batchStates.push_back(st);
    auto img = conv_.readback(*conv_.toBgra8(*job.frame, kWidth, kHeight));
    check(NvCVImage_Init(&m.cpuIn, kWidth, kHeight, static_cast<int>(kWidth * 4), img.bgra.data(), NVCV_BGRA, NVCV_U8,
                         NVCV_CHUNKY, NVCV_CPU),
          "wrap input");
    check(NvCVImage_Transfer(&m.cpuIn, nth(&m.src, &view, i, kHeight), 1.0f, m.stream, &m.tmp), "upload");
  }
  check(NvVFX_SetU32(m.eff, NVVFX_BATCH_SIZE, static_cast<unsigned>(jobs.size())), "batch size");
  check(NvVFX_SetStateObjectHandleArray(m.eff, NVVFX_STATE, batchStates.data()), "states");
  check(NvVFX_Run(m.eff, 0), "run");

  m.alphaBytes.resize(static_cast<size_t>(kWidth) * kHeight);
  for (unsigned i = 0; i < jobs.size(); ++i) {
    check(NvCVImage_Init(&m.cpuOut, kWidth, kHeight, static_cast<int>(kWidth), m.alphaBytes.data(), NVCV_A, NVCV_U8,
                         NVCV_CHUNKY, NVCV_CPU),
          "wrap output");
    check(NvCVImage_Transfer(nth(&m.dst, &view, i, kHeight), &m.cpuOut, 1.0f, m.stream, &m.tmp), "download");
    auto tex = pool_.acquire(kWidth, kHeight, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    {
      auto lock = gpu_.lock();
      gpu_.context()->UpdateSubresource(tex->tex.Get(), 0, nullptr, m.alphaBytes.data(), kWidth, 0);
    }
    jobs[i]->alpha = tex;
  }
}
}  // namespace bc
```

Note: `NvCVImage_Transfer` to a CPU destination is synchronous for the copy out (the sample relies on it the same way); if the SDK documents otherwise, add `NvVFX_CudaStreamSynchronize`-equivalent before reading `alphaBytes` and ledger it.

`src/engine/matte.cpp` — add the Maxine branch at the top of `makeMatteBackend`:
```cpp
#if BC_WITH_MAXINE
  if (gpu.isNvidia()) {
    try {
      return {std::make_unique<MaxineMatte>(gpu, pool, conv, MaxineMatte::defaultModelDir()), ""};
    } catch (const std::exception& e) {
      return {nullptr, std::string("the NVIDIA background removal could not start: ") + e.what()};
    }
  }
#endif
```
(with `#if BC_WITH_MAXINE #include "engine/maxine_matte.h" #endif` at the top).

- [ ] **Step 4: Build both ways**

1. `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1` — on the build PC the configure log says `BC_WITH_MAXINE=OFF` unless the owner installed the SDK; the build must succeed.
2. If `VFXSDK_ROOT` is set on this PC, the build compiles `maxine_matte.cpp`; fix compile errors only against the installed headers and ledger each fix. If it is not set, ledger "Maxine backend not compiled — SDK absent" — the planner will compile it later.

- [ ] **Step 5: Run the suite**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1`
Expected: everything passes; `MaxineMatte*` reported as skipped with its reason.

- [ ] **Step 6: Commit**

```bash
git add cmake/FindMaxine.cmake src/engine tests CMakeLists.txt
git commit -m "feat(engine): Maxine AI Green Screen batch backend (NVIDIA-only, compile-gated)"
```

### Task 6: UI — camera detail with stage panels, output toggles and removal

**Files:**
- Create: `ui/src/components/StageForm.tsx`, `ui/src/components/ConfirmDialog.tsx`, `ui/src/screens/CameraDetail.tsx`
- Modify: `ui/src/i18n.ts` (export `lang`; new keys), `ui/src/api/protocol.ts` (stage types), `ui/src/screens/Multiview.tsx` (open a camera), `ui/src/App.tsx` (selected camera)
- Test: `ui/src/components/StageForm.test.tsx`, `ui/src/screens/CameraDetail.test.tsx`

**Interfaces:**
- Consumes: commands `getStageSchemas`, `setStage`, `updateOutput`, `removeOutput`, `removeCamera` (Task 1, M1 Task 11); state `/cameras/<id>/stages/<kind>/{state,error}` (Task 2); `CameraConfig.stages` in the show JSON (Task 1).
- Produces: `StageForm({schema, values, onChange})`, `ConfirmDialog({open, title, body, confirmLabel, onConfirm, onClose})`, `CameraDetail({client, cameraId, onBack})`; `Multiview` gains an `onOpen(cameraId)` prop; protocol types `StageField`, `StageSchemaJson`, and `CameraState.stages`.

Copy (Vietnamese / English): stage states `active` "Đang chạy" / "Running", `bypassed` "Đã bỏ qua" / "Bypassed", `off` "Không dùng" / "Not used"; "Quay lại" / "Back"; "Xử lý" / "Processing"; "Xoá đầu ra" / "Remove output" with body "Xoá đầu ra này? Nguồn nhận sẽ mất hình ngay." / "Remove this output? Its receiver loses picture now."; "Xoá camera" / "Remove camera" with body "Xoá camera này cùng mọi đầu ra của nó?" / "Remove this camera and all its outputs?"; "Bật" / "On"; hint "Chế độ nền chỉ có tác dụng khi bật Tách nền." / "Background modes only work with background removal on."

- [ ] **Step 1: Write the failing tests**

`ui/src/components/StageForm.test.tsx`:
```tsx
import { fireEvent, render, screen } from '@testing-library/react';

import type { StageSchemaJson } from '../api/protocol';
import { StageForm } from './StageForm';

const schema: StageSchemaJson = {
  kind: 'matte',
  labelVi: 'Tách nền',
  labelEn: 'Background removal',
  fields: [
    { key: 'enabled', type: 'bool', default: false, labelVi: 'Bật', labelEn: 'Enabled' },
    { key: 'quality', type: 'enum', default: 'quality', labelVi: 'Chất lượng', labelEn: 'Quality', options: ['quality', 'performance'], optionLabelsVi: ['Chất lượng cao', 'Hiệu năng'], optionLabelsEn: ['High quality', 'Performance'] },
    { key: 'choke', type: 'number', default: 0, labelVi: 'Co viền (px)', labelEn: 'Choke (px)', min: -8, max: 8, step: 1 },
  ],
};

test('renders every field with its Vietnamese label', () => {
  render(<StageForm schema={schema} values={{ enabled: false, quality: 'quality', choke: 0 }} onChange={() => {}} />);
  expect(screen.getByLabelText('Bật')).toBeInTheDocument();
  expect(screen.getByText('Chất lượng')).toBeInTheDocument();
  expect(screen.getByText('Co viền (px)')).toBeInTheDocument();
});

test('toggling a switch reports only that key', () => {
  const onChange = vi.fn();
  render(<StageForm schema={schema} values={{ enabled: false, quality: 'quality', choke: 0 }} onChange={onChange} />);
  fireEvent.click(screen.getByLabelText('Bật'));
  expect(onChange).toHaveBeenCalledWith({ enabled: true });
});
```

`ui/src/screens/CameraDetail.test.tsx`:
```tsx
import { render, screen, waitFor } from '@testing-library/react';

import { EngineClient } from '../api/client';
import { FakeWs } from '../test/fakeWs';
import { CameraDetail } from './CameraDetail';

test('shows stage state, the stage error, and schema panels', async () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({
    type: 'snapshot',
    state: {
      cameras: {
        a: {
          name: 'Sân khấu',
          phase: 'live',
          input: { state: 'live', error: null },
          stages: {
            matte: { state: 'bypassed', error: { code: 'matte-unavailable', message: 'cần GPU NVIDIA RTX' } },
            background: { state: 'off', error: null },
          },
        },
      },
    },
    show: { cameras: [{ id: 'a', name: 'Sân khấu', input: { kind: 'srt', params: {} }, outputs: [], stages: {} }] },
  });
  render(<CameraDetail client={c} cameraId="a" onBack={() => {}} />);
  const req = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'getStageSchemas');
  expect(req).toBeDefined();
  FakeWs.last.receive({
    id: req.id,
    ok: true,
    result: [
      { kind: 'matte', labelVi: 'Tách nền', labelEn: 'Background removal', fields: [{ key: 'enabled', type: 'bool', default: false, labelVi: 'Bật', labelEn: 'Enabled' }] },
      { kind: 'background', labelVi: 'Nền', labelEn: 'Background', fields: [] },
    ],
  });
  await waitFor(() => expect(screen.getByText('Tách nền')).toBeInTheDocument());
  expect(screen.getByText('Đã bỏ qua')).toBeInTheDocument();
  expect(screen.getByRole('alert')).toHaveTextContent('cần GPU NVIDIA RTX');
});
```

Run `PATH="/c/Program Files/nodejs:$PATH" npm test` in `ui/` — Expected: FAIL (modules missing).

- [ ] **Step 2: Types and strings**

`ui/src/api/protocol.ts` — add:
```ts
export interface StageField {
  key: string;
  type: 'number' | 'enum' | 'bool' | 'color' | 'path';
  default: unknown;
  labelVi: string;
  labelEn: string;
  min?: number;
  max?: number;
  step?: number;
  options?: string[];
  optionLabelsVi?: string[];
  optionLabelsEn?: string[];
}
export interface StageSchemaJson {
  kind: string;
  labelVi: string;
  labelEn: string;
  fields: StageField[];
}
export interface StageState {
  state: 'off' | 'active' | 'bypassed';
  error: ErrorInfo | null;
}
```
and in `CameraState` add `stages?: Record<string, StageState>;`, in `CameraConfig` add `stages?: Record<string, Record<string, unknown>>;`.

`ui/src/i18n.ts` — change `const lang = ...` to `export const lang = ...`; add to `vi` and `en` (same keys, the copy above): `'stage.active'`, `'stage.bypassed'`, `'stage.off'`, `'action.back'`, `'action.removeOutput'`, `'action.removeCamera'`, `'confirm.removeOutput'`, `'confirm.removeCamera'`, `'camera.stages'`, `'output.enabled'`, `'background.needsMatte'`.

- [ ] **Step 3: Implement the components**

`ui/src/components/StageForm.tsx`:
```tsx
import { FormControlLabel, MenuItem, Slider, Stack, Switch, TextField, Typography } from '@mui/material';

import type { StageField, StageSchemaJson } from '../api/protocol';
import { lang } from '../i18n';

const label = (f: { labelVi: string; labelEn: string }) => (lang === 'en' ? f.labelEn : f.labelVi);

// Builds a stage panel from the engine's schema (spec §5.4).
export function StageForm({ schema, values, onChange }: { schema: StageSchemaJson; values: Record<string, unknown>; onChange: (patch: Record<string, unknown>) => void }) {
  const value = (f: StageField) => (values[f.key] ?? f.default) as never;
  return (
    <Stack spacing={1.5}>
      {schema.fields.map((f) => {
        switch (f.type) {
          case 'bool':
            return (
              <FormControlLabel
                key={f.key}
                control={<Switch checked={Boolean(value(f))} onChange={(e) => onChange({ [f.key]: e.target.checked })} />}
                label={label(f)}
              />
            );
          case 'enum':
            return (
              <TextField key={f.key} select size="small" label={label(f)} value={value(f)} onChange={(e) => onChange({ [f.key]: e.target.value })}>
                {(f.options ?? []).map((o, i) => (
                  <MenuItem key={o} value={o}>
                    {(lang === 'en' ? f.optionLabelsEn : f.optionLabelsVi)?.[i] ?? o}
                  </MenuItem>
                ))}
              </TextField>
            );
          case 'number':
            return (
              <div key={f.key}>
                <Typography variant="body2">{label(f)}</Typography>
                <Slider
                  size="small"
                  min={f.min}
                  max={f.max}
                  step={f.step}
                  value={Number(value(f))}
                  valueLabelDisplay="auto"
                  onChange={(_, v) => onChange({ [f.key]: v as number })}
                />
              </div>
            );
          case 'color':
            return (
              <TextField key={f.key} size="small" type="color" label={label(f)} value={value(f)} onChange={(e) => onChange({ [f.key]: e.target.value })} />
            );
          case 'path':
            return (
              <TextField key={f.key} size="small" label={label(f)} value={value(f)} placeholder="C:\…\nen.png" onChange={(e) => onChange({ [f.key]: e.target.value })} />
            );
        }
      })}
    </Stack>
  );
}
```

`ui/src/components/ConfirmDialog.tsx`:
```tsx
import { Button, Dialog, DialogActions, DialogContent, DialogTitle, Typography } from '@mui/material';

import { t } from '../i18n';

// UX contract: destructive actions confirm with a verb button; secondary left, primary right.
export function ConfirmDialog(props: { open: boolean; title: string; body: string; confirmLabel: string; onConfirm: () => void; onClose: () => void }) {
  return (
    <Dialog open={props.open} onClose={props.onClose}>
      <DialogTitle>{props.title}</DialogTitle>
      <DialogContent>
        <Typography>{props.body}</Typography>
      </DialogContent>
      <DialogActions>
        <Button onClick={props.onClose}>{t('action.cancel')}</Button>
        <Button color="error" variant="contained" onClick={props.onConfirm}>
          {props.confirmLabel}
        </Button>
      </DialogActions>
    </Dialog>
  );
}
```

`ui/src/screens/CameraDetail.tsx`:
```tsx
import { Box, Button, Card, CardContent, Chip, Stack, Switch, Typography } from '@mui/material';
import { useEffect, useRef, useState } from 'react';

import type { EngineClient } from '../api/client';
import type { StageSchemaJson } from '../api/protocol';
import { useEngine } from '../api/useEngine';
import { ConfirmDialog } from '../components/ConfirmDialog';
import { ErrorBlock } from '../components/ErrorBlock';
import { SignalChip } from '../components/SignalChip';
import { StageForm } from '../components/StageForm';
import { lang, t } from '../i18n';
import { PreviewCanvas } from '../preview/PreviewCanvas';

const stateColor = { active: 'success', bypassed: 'warning', off: 'default' } as const;

export function CameraDetail({ client, cameraId, onBack }: { client: EngineClient; cameraId: string; onBack: () => void }) {
  const { state, show } = useEngine(client);
  const [schemas, setSchemas] = useState<StageSchemaJson[]>([]);
  const [stageErrors, setStageErrors] = useState<Record<string, string>>({});
  const [confirm, setConfirm] = useState<{ kind: 'output'; id: string } | { kind: 'camera' } | null>(null);
  const timers = useRef<Record<string, ReturnType<typeof setTimeout>>>({});
  const pending = useRef<Record<string, Record<string, unknown>>>({});

  useEffect(() => {
    client.request<StageSchemaJson[]>('getStageSchemas').then(setSchemas).catch(() => setSchemas([]));
  }, [client]);
  useEffect(() => client.setPreviewSubscription([cameraId], 'large'), [client, cameraId]);

  const cam = show?.cameras.find((c) => c.id === cameraId);
  const cs = state.cameras?.[cameraId];
  if (!cam) return null;

  // Sliders fire many changes; send one setStage per stage every 150 ms.
  function change(kind: string, patch: Record<string, unknown>) {
    pending.current[kind] = { ...pending.current[kind], ...patch };
    clearTimeout(timers.current[kind]);
    timers.current[kind] = setTimeout(() => {
      const params = pending.current[kind];
      pending.current[kind] = {};
      client
        .request('setStage', { cameraId, stage: kind, params })
        .then(() => setStageErrors((e) => ({ ...e, [kind]: '' })))
        .catch((err: Error) => setStageErrors((e) => ({ ...e, [kind]: err.message })));
    }, 150);
  }

  const matteOn = Boolean(cam.stages?.matte?.enabled);
  return (
    <Box sx={{ p: 2 }}>
      <Stack direction="row" spacing={2} alignItems="center" sx={{ mb: 2 }}>
        <Button onClick={onBack}>{t('action.back')}</Button>
        <Typography variant="h4" sx={{ flex: 1 }}>
          {cam.name}
        </Typography>
        <SignalChip state={cs?.input?.state} />
        <Button color="error" onClick={() => setConfirm({ kind: 'camera' })}>
          {t('action.removeCamera')}
        </Button>
      </Stack>
      {cs?.input?.error && <ErrorBlock message={cs.input.error.message} />}
      <Box sx={{ display: 'grid', gap: 2, gridTemplateColumns: { md: '2fr 1fr' } }}>
        <PreviewCanvas client={client} cameraId={cameraId} size="large" />
        <Stack spacing={2}>
          <Typography variant="h6">{t('camera.stages')}</Typography>
          {schemas.map((s) => {
            const st = cs?.stages?.[s.kind];
            return (
              <Card key={s.kind} variant="outlined">
                <CardContent>
                  <Stack direction="row" alignItems="center" sx={{ mb: 1 }}>
                    <Typography variant="subtitle1" sx={{ flex: 1 }}>
                      {lang === 'en' ? s.labelEn : s.labelVi}
                    </Typography>
                    {st && <Chip size="small" color={stateColor[st.state]} label={t(`stage.${st.state}`)} />}
                  </Stack>
                  {st?.error && <ErrorBlock message={st.error.message} />}
                  {stageErrors[s.kind] && <ErrorBlock message={stageErrors[s.kind]} />}
                  {s.kind === 'background' && !matteOn && (
                    <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>
                      {t('background.needsMatte')}
                    </Typography>
                  )}
                  <StageForm schema={s} values={cam.stages?.[s.kind] ?? {}} onChange={(p) => change(s.kind, p)} />
                </CardContent>
              </Card>
            );
          })}
          <Typography variant="h6">{t('camera.outputs')}</Typography>
          {cam.outputs.map((o) => (
            <Stack key={o.id} direction="row" spacing={1} alignItems="center">
              <Typography sx={{ flex: 1 }}>{o.kind === 'ndi' ? t('output.ndi') : o.kind === 'fullscreen' ? t('output.fullscreen') : o.kind}</Typography>
              <SignalChip state={cs?.outputs?.[o.id]?.state} />
              <Switch
                checked={o.enabled}
                inputProps={{ 'aria-label': t('output.enabled') }}
                onChange={(e) => client.request('updateOutput', { cameraId, id: o.id, enabled: e.target.checked }).catch(() => {})}
              />
              <Button size="small" color="error" onClick={() => setConfirm({ kind: 'output', id: o.id })}>
                {t('action.removeOutput')}
              </Button>
            </Stack>
          ))}
        </Stack>
      </Box>
      <ConfirmDialog
        open={confirm !== null}
        title={confirm?.kind === 'camera' ? t('action.removeCamera') : t('action.removeOutput')}
        body={confirm?.kind === 'camera' ? t('confirm.removeCamera') : t('confirm.removeOutput')}
        confirmLabel={confirm?.kind === 'camera' ? t('action.removeCamera') : t('action.removeOutput')}
        onClose={() => setConfirm(null)}
        onConfirm={() => {
          if (confirm?.kind === 'camera') client.request('removeCamera', { id: cameraId }).then(onBack).catch(() => {});
          else if (confirm?.kind === 'output') client.request('removeOutput', { cameraId, id: confirm.id }).catch(() => {});
          setConfirm(null);
        }}
      />
    </Box>
  );
}
```

- [ ] **Step 4: Open a camera from the multiview**

`ui/src/screens/Multiview.tsx`: add a prop `onOpen?: (cameraId: string) => void` to the component signature, and make the camera name clickable:
```tsx
                  <Typography variant="h6" sx={{ flex: 1, cursor: onOpen ? 'pointer' : 'default' }} onClick={() => onOpen?.(cam.id)}>
                    {cam.name}
                  </Typography>
```

`ui/src/App.tsx` — in `Connected`, add `const [selected, setSelected] = useState<string | null>(null);` (import `useState`) and replace `<Multiview client={client} />` with:
```tsx
      {selected ? (
        <CameraDetail client={client} cameraId={selected} onBack={() => setSelected(null)} />
      ) : (
        <Multiview client={client} onOpen={setSelected} />
      )}
```
(import `CameraDetail`).

- [ ] **Step 5: Run the UI tests and build**

In `ui/`: `PATH="/c/Program Files/nodejs:$PATH" npm test` — expected all pass (M1's 12 + 3 new). `npm run build` — no type errors.

- [ ] **Step 6: Commit**

```bash
git add ui/src
git commit -m "feat(ui): camera detail with schema-driven stage panels, output toggles and removal"
```

### Task 7: M2 acceptance — 4× 1080p60 with matte + background inside the budget; follow-ups

**Files:**
- Create: `tests/e2e_m2_test.cpp`
- Modify: `docs/superpowers/follow-ups.md`, `CMakeLists.txt`

**Interfaces:**
- Consumes: everything above plus M1's `HybridFactory` pattern (re-declare it locally in this test file — test files do not share symbols), `TestMatte` (Task 4), `writeTestTs`/`TsPusher` (M1).
- Produces: spec §9 M2 evidence on the build PC: the pipeline cost of 4 cameras × (refine + blur/keyed + readback) at 1080p60 with a **test** matte backend. The Maxine inference cost itself can only be measured on an RTX machine — the report must say so.

- [ ] **Step 1: Write the acceptance test**

`tests/e2e_m2_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/engine.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_matte.h"
#include "support/test_media.h"

using namespace std::chrono_literals;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
struct HybridFactory : bc::AdapterFactory {
  bc::DefaultAdapterFactory real;
  bc::test::FakeFactory fake;
  HybridFactory(bc::GpuDevice& g, bc::TexturePool& p, bc::Converter& c) : real(g, p), fake(c) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override { return real.makeInput(cfg); }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig& cam) override {
    return fake.makeOutput(cfg, cam);
  }
};
}  // namespace

TEST_CASE("M2 acceptance: 4x 1080p60 with matte refine and background stages, no overruns" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  bc::test::TempDir dir;
  const auto clip = dir.path / "1080p60.ts";
  bc::test::writeTestTs(clip, 1920, 1080, 60, 10);
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::Compositor comp(*gpu, pool);
  bc::StateStore state;
  HybridFactory factory(*gpu, pool, conv);
  bc::Engine engine(conv, comp, state, factory);
  engine.setMatteBackend(std::make_unique<bc::test::TestMatte>(*gpu, pool), "");

  const char* modes[] = {"blur", "blur", "keyed", "replace"};
  bc::Show show;
  for (int i = 0; i < 4; ++i) {
    bc::CameraConfig c;
    c.id = "cam" + std::to_string(i);
    c.name = c.id;
    c.input.kind = "srt";
    c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47931 + i}};
    c.outputs.push_back({"out" + std::to_string(i), "fake", false, true, {}});
    c.stages = {{"matte", bc::normalizeStageParams("matte", {{"enabled", true}, {"choke", 1}, {"feather", 2}})},
                {"background", bc::normalizeStageParams("background", {{"mode", modes[i]}, {"blur", 30}})}};
    show.cameras.push_back(c);
  }
  engine.apply(show);
  engine.start();
  std::vector<std::unique_ptr<bc::test::TsPusher>> pushers;
  for (int i = 0; i < 4; ++i) {
    pushers.push_back(std::make_unique<bc::test::TsPusher>(clip, "srt://127.0.0.1:" + std::to_string(47931 + i) + "?mode=caller", true));
  }
  auto live = [&] {
    auto s = state.snapshot();
    for (int i = 0; i < 4; ++i) {
      auto p = json::json_pointer("/cameras/cam" + std::to_string(i) + "/phase");
      if (!s.contains(p) || s.at(p) != "live") return false;
    }
    return true;
  };
  const auto warm = Clock::now() + 20s;
  while (!live() && Clock::now() < warm) std::this_thread::sleep_for(100ms);
  REQUIRE(live());
  std::this_thread::sleep_for(3s);

  const auto o0 = engine.stats().overruns;
  std::this_thread::sleep_for(20s);
  const auto st = engine.stats();
  CHECK(st.overruns - o0 == 0);
  auto snap = state.snapshot();
  for (int i = 0; i < 4; ++i) {
    const auto id = "cam" + std::to_string(i);
    MESSAGE(id << " (" << modes[i] << "): latency " << st.cameras.at(id).latencyMs << " ms");
    CHECK(st.cameras.at(id).latencyMs <= 50.0);
    CHECK(snap["cameras"][id]["stages"]["matte"]["state"] == "active");
    CHECK(snap["cameras"][id]["stages"]["background"]["state"] == "active");
  }
  auto keyed = factory.fake.output("out2")->seen().back();
  CHECK(keyed.firstPixel[3] == 0);
  CHECK(keyed.centerPixel[3] == 255);
  for (auto& p : pushers) p->stop();
  engine.stop();
}
```

Add it to `bc_tests`.

- [ ] **Step 2: Run it**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "M2 acceptance*"`
Expected: PASS on the RX 6600 with per-camera latency lines in the log. If it overruns, do not loosen bounds: report per-stage timings (temporary `MESSAGE`s, removed before commit) as the finding.

- [ ] **Step 3: Update the follow-ups**

Append to `docs/superpowers/follow-ups.md`:
```markdown
## M2
- Maxine AI Green Screen backend is **untested on hardware**; its only test (`tests/maxine_matte_test.cpp`) skips without an RTX GPU + `VFXSDK_ROOT` (`src/engine/maxine_matte.cpp`).
- Maxine frames are staged through CPU memory (~1–2 ms per camera); use NvCVImage D3D11 interop if the budget needs it (`src/engine/maxine_matte.cpp`).
- One matte quality per batch: the first camera's setting wins (`src/engine/maxine_matte.cpp`, `process`).
- Background stage budget is a fixed quarter tick per camera (`src/engine/camera.cpp`, `endTick`).
- The M2 acceptance measures pipeline cost with a test matte; the real inference cost needs an RTX run (`tests/e2e_m2_test.cpp`).
```
Add any `ponytail:` comment or deferred minor from this run.

- [ ] **Step 4: Full verification**

1. `scripts/test.ps1` (all) — record passed/skipped and skip reasons.
2. `ui/`: `npm test` and `npm run build`.
3. `BC_FORCE_WARP=1` run of `bc_tests.exe` — record.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e_m2_test.cpp docs/superpowers/follow-ups.md CMakeLists.txt
git commit -m "test: M2 acceptance (4x1080p60 with matte and background stages) and follow-ups"
```
