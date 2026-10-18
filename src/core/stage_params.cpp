#include "core/stage_params.h"

#include <algorithm>
#include <regex>
#include <stdexcept>

namespace bc {
namespace {
using nlohmann::json;

StageField number(std::string key, double min, double max, double step, double def, std::string label) {
  StageField f;
  f.key = std::move(key);
  f.type = "number";
  f.min = min;
  f.max = max;
  f.step = step;
  f.def = def;
  f.label = std::move(label);
  return f;
}

StageField enumeration(std::string key, std::vector<std::string> options, std::vector<std::string> labels, std::string def,
                       std::string label) {
  StageField f;
  f.key = std::move(key);
  f.type = "enum";
  f.options = std::move(options);
  f.optionLabels = std::move(labels);
  f.def = std::move(def);
  f.label = std::move(label);
  return f;
}

StageField simple(std::string key, std::string type, json def, std::string label) {
  StageField f;
  f.key = std::move(key);
  f.type = std::move(type);
  f.def = std::move(def);
  f.label = std::move(label);
  return f;
}

std::vector<StageSchema> build() {
  StageSchema cleanup{"cleanup", "Clean up",
                      "Removes compression blockiness and webcam noise. Needs an NVIDIA RTX card.", "cleanup", {},
                      json{{"enabled", true}}, {}};
  cleanup.fields = {
      simple("enabled", "bool", false, "On"),
  };
  StageSchema upscale{"upscale", "Upscale",
                      "Raises a low-resolution camera to 720p or 1080p. AI on NVIDIA RTX cards; a sharp standard "
                      "upscaler on other GPUs.",
                      "upscale", {}, json{{"enabled", true}, {"target", "1080"}}, {}};
  upscale.fields = {
      simple("enabled", "bool", false, "On"),
      enumeration("target", {"720", "1080"}, {"720p", "1080p"}, "1080", "Target"),
  };
  StageSchema color{"color", "Colour",
                    "Calibrate this camera once against a colour chart (printed card or tablet); the correction is then locked.",
                    "color", {}, json{{"enabled", true}}, {}};
  auto hide = [](StageField f) {
    f.hidden = true;
    return f;
  };
  color.fields = {
      simple("enabled", "bool", false, "On"),
      hide(number("exposure", -3, 3, 0.05, 0, "Exposure (EV)")),
      hide(number("temperature", -100, 100, 1, 0, "Temperature")),
      hide(number("tint", -100, 100, 1, 0, "Tint")),
      hide(number("lift", -0.2, 0.2, 0.005, 0, "Lift")),
      hide(number("gamma", 0.5, 2, 0.01, 1, "Gamma")),
      hide(number("gain", 0.5, 2, 0.01, 1, "Gain")),
      hide(number("saturation", 0, 2, 0.01, 1, "Saturation")),
      hide(simple("lut", "path", "", "LUT")),
      hide(simple("previousLut", "path", "", "Previous LUT")),
      hide(enumeration("chart", {"card", "tablet"}, {"Printed card", "Tablet"}, "card", "Chart")),
      hide(number("deltaE", 0, 100, 0.01, 0, "Colour error after")),
      hide(number("deltaEBefore", 0, 100, 0.01, 0, "Colour error before")),
      hide(number("calibratedAt", 0, 4102444800.0, 1, 0, "Calibrated at")),
      hide(simple("reference", "bool", false, "Tablet reference")),
  };
  StageSchema matte{"matte", "Background removal",
                    "AI person cut-out without a green screen (NVIDIA Maxine, or ONNX on other GPUs).", "matte", {},
                    json{{"enabled", true}}, {}};
  matte.fields = {
      simple("enabled", "bool", false, "On"),
      enumeration("quality", {"quality", "performance"}, {"High quality", "Performance"}, "quality", "Quality"),
      number("choke", -8, 8, 1, 0, "Choke (px)"),
      number("feather", 0, 8, 0.5, 1, "Feather (px)"),
      number("threshold", 0, 1, 0.01, 0.5, "Threshold"),
  };
  StageSchema background{"background", "Background",
                         "Blur the background, replace it with an image or colour, or send fill + key so the switcher keys the person.",
                         "bg", {"matte"}, json{{"enabled", true}, {"mode", "blur"}}, {}};
  background.fields = {
      simple("enabled", "bool", true, "On"),
      enumeration("mode", {"none", "keyed", "blur", "replace"}, {"None", "Keyed (fill + key)", "Blur", "Replace"}, "none",
                  "Background mode"),
      number("blur", 1, 50, 1, 12, "Blur (px)"),
      simple("color", "color", "#101418", "Colour"),
      simple("image", "path", "", "Image"),
  };
  return {cleanup, upscale, color, matte, background};
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
  if (f.type == "text") {
    if (!v.is_string() || v.get<std::string>().size() > 64) throw std::invalid_argument(where + " must be text of at most 64 characters");
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

json schemaToJson(const StageSchema& s, const std::string& unavailable) {
  json fields = json::array();
  for (auto& f : s.fields) {
    json j{{"key", f.key}, {"type", f.type}, {"default", f.def}, {"label", f.label}};
    if (f.type == "number") {
      j["min"] = f.min;
      j["max"] = f.max;
      j["step"] = f.step;
    }
    if (f.type == "enum") {
      j["options"] = f.options;
      j["optionLabels"] = f.optionLabels;
    }
    if (f.hidden) j["hidden"] = true;
    fields.push_back(j);
  }
  json out = {{"kind", s.kind},     {"label", s.label},       {"description", s.description}, {"icon", s.icon},
              {"requires", s.needs}, {"onAdd", s.onAdd}, {"fields", fields}};
  if (!unavailable.empty()) out["unavailable"] = unavailable;
  return out;
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
