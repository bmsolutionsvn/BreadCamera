#include "core/show.h"

#include <algorithm>
#include <random>
#include <set>

#include "core/secrets.h"
#include "core/stage_params.h"

namespace bc {
namespace {
using nlohmann::json;

bool isSecretKey(const std::string& key) {
  return std::find(kSecretKeys.begin(), kSecretKeys.end(), key) != kSecretKeys.end();
}

json secretsOut(const json& params, SecretMode mode) {
  json out = params;
  for (auto& [key, value] : out.items()) {
    if (!isSecretKey(key) || !value.is_string()) continue;
    const auto plain = value.get<std::string>();
    switch (mode) {
      case SecretMode::Encrypt:
        if (!plain.empty()) value = json{{"$dpapi", protectSecret(plain)}};
        break;
      case SecretMode::Redact:
        value = json{{"set", !plain.empty()}};
        break;
      case SecretMode::Plain:
        break;
    }
  }
  return out;
}

json secretsIn(const json& params) {
  if (!params.is_object()) throw ShowError("params must be an object");
  json out = params;
  for (auto& [key, value] : out.items()) {
    if (!isSecretKey(key)) continue;
    if (value.is_string()) continue;
    if (value.is_object() && value.size() == 1 && value.contains("$dpapi") &&
        value["$dpapi"].is_string()) {
      try {
        value = unprotectSecret(value["$dpapi"].get<std::string>());
      } catch (const std::exception& e) {
        throw ShowError(std::string("cannot decrypt ") + key + ": " + e.what());
      }
      continue;
    }
    throw ShowError("secret '" + key + "' must be a string or {\"$dpapi\": ...}");
  }
  return out;
}

std::string str(const json& j, const char* key) {
  if (!j.contains(key) || !j[key].is_string()) throw ShowError(std::string("missing string '") + key + "'");
  return j[key].get<std::string>();
}
}  // namespace

CameraConfig* Show::findCamera(std::string_view id) {
  for (auto& c : cameras) if (c.id == id) return &c;
  return nullptr;
}
const CameraConfig* Show::findCamera(std::string_view id) const {
  for (auto& c : cameras) if (c.id == id) return &c;
  return nullptr;
}

json outputToJson(const OutputConfig& o) {
  return {{"id", o.id},
          {"kind", o.kind},
          {"keyed", o.keyed},
          {"enabled", o.enabled},
          {"params", o.params},
          {"format", {{"halfRate", o.halfRate}}}};
}

OutputConfig outputFromJson(const json& j) {
  try {
    if (!j.is_object()) throw ShowError("output must be an object");
    OutputConfig o;
    o.id = str(j, "id");
    o.kind = str(j, "kind");
    if (o.id.empty() || o.kind.empty()) throw ShowError("output id and kind must not be empty");
    o.keyed = j.value("keyed", false);
    o.enabled = j.value("enabled", true);
    o.params = j.value("params", json::object());
    if (!o.params.is_object()) throw ShowError("output params must be an object");
    auto f = j.value("format", json::object());
    if (!f.is_object()) throw ShowError("output format must be an object");
    // A legacy per-output width/height is ignored, not an error: one frame size per camera.
    o.halfRate = f.value("halfRate", false);
    return o;
  } catch (const nlohmann::json::exception& e) {
    throw ShowError(e.what());
  }
}

json cameraToJson(const CameraConfig& c, SecretMode mode) {
  json outs = json::array();
  for (auto& o : c.outputs) outs.push_back(outputToJson(o));
  return {{"id", c.id},
          {"name", c.name},
          {"input", {{"kind", c.input.kind}, {"params", secretsOut(c.input.params, mode)}}},
          {"outputs", outs},
          {"stages", c.stages}};
}

CameraConfig cameraFromJson(const json& j) {
  try {
    if (!j.is_object()) throw ShowError("camera must be an object");
    CameraConfig c;
    c.id = str(j, "id");
    c.name = str(j, "name");
    if (c.id.empty()) throw ShowError("camera id must not be empty");
    if (!j.contains("input") || !j["input"].is_object()) throw ShowError("camera input missing");
    c.input.kind = str(j["input"], "kind");
    c.input.params = secretsIn(j["input"].value("params", json::object()));
    std::set<std::string> ids;
    for (auto& o : j.value("outputs", json::array())) {
      auto out = outputFromJson(o);
      if (!ids.insert(out.id).second) throw ShowError("duplicate output id " + out.id);
      c.outputs.push_back(std::move(out));
    }
    const auto stages = j.value("stages", json::object());
    if (!stages.is_object()) throw ShowError("stages must be an object");
    for (auto& [kind, params] : stages.items()) {
      try {
        c.stages[kind] = normalizeStageParams(kind, params);
      } catch (const std::invalid_argument& e) {
        throw ShowError("camera " + c.id + ": " + e.what());
      }
    }
    return c;
  } catch (const nlohmann::json::exception& e) {
    throw ShowError(e.what());
  }
}

json showToJson(const Show& s, SecretMode mode) {
  json cams = json::array();
  for (auto& c : s.cameras) cams.push_back(cameraToJson(c, mode));
  return {{"schemaVersion", Show::kSchemaVersion},
          {"settings",
           {{"rate", s.settings.rate.str()},
            {"holdMs", s.settings.holdMs},
            {"slatePath", s.settings.slatePath}}},
          {"cameras", cams}};
}

Show showFromJson(const json& j) {
  try {
    if (!j.is_object()) throw ShowError("show must be a JSON object");
    if (j.value("schemaVersion", 0) != Show::kSchemaVersion) {
      throw ShowError("unsupported schemaVersion " + j.value("schemaVersion", json(0)).dump());
    }
    Show s;
    auto settings = j.value("settings", json::object());
    auto rate = parseShowRate(settings.value("rate", std::string("60/1")));
    if (!rate) throw ShowError("settings.rate must be 15, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60, 119.88 or 120 fps");
    s.settings.rate = *rate;
    // A legacy settings.resolution is ignored, not an error: a show has no frame size any more.
    s.settings.holdMs = settings.value("holdMs", 5000);
    if (s.settings.holdMs < 0 || s.settings.holdMs > 60000) throw ShowError("settings.holdMs must be 0..60000");
    s.settings.slatePath = settings.value("slatePath", std::string());
    std::set<std::string> ids;
    for (auto& cj : j.value("cameras", json::array())) {
      auto cam = cameraFromJson(cj);
      if (!ids.insert(cam.id).second) throw ShowError("duplicate camera id " + cam.id);
      s.cameras.push_back(std::move(cam));
    }
    return s;
  } catch (const nlohmann::json::exception& e) {
    throw ShowError(e.what());
  }
}

std::string newId() {
  static thread_local std::mt19937 rng{std::random_device{}()};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string id(8, '0');
  for (auto& ch : id) ch = kHex[rng() % 16];
  return id;
}
}  // namespace bc
