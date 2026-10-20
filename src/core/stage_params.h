#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace bc {
struct StageField {
  std::string key;
  std::string type;  // number | enum | bool | color | path | text
  double min = 0, max = 0, step = 0;
  std::vector<std::string> options;
  std::vector<std::string> optionLabels;
  nlohmann::json def;
  std::string label;
  bool hidden = false;  // kept in the schema (calibration writes it) but not shown in the UI
};

// spec §5.4: every stage has a parameter schema; the UI builds its group from it and
// lists it in Add effect. Effects are per-camera add-ins.
struct StageSchema {
  std::string kind;
  std::string label;
  std::string description;
  std::string icon;                   // UI icon name
  std::vector<std::string> needs;     // stage kinds that must be added first (JSON: "requires"; `requires` is a C++20 keyword)
  nlohmann::json onAdd;               // params the UI sends when the operator adds it
  std::vector<StageField> fields;     // fields[0] is always "enabled"
};

// In pipeline order (spec §3.2).
const std::vector<StageSchema>& stageSchemas();
const StageSchema* findStageSchema(std::string_view kind);
nlohmann::json schemaToJson(const StageSchema& s, const std::string& unavailable = "");
// Fills defaults; throws std::invalid_argument on unknown keys, wrong types or bad values.
nlohmann::json normalizeStageParams(std::string_view kind, const nlohmann::json& params);
}  // namespace bc
