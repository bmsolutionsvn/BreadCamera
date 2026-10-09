#pragma once
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/rate.h"

namespace bc {
struct InputConfig {
  std::string kind = "srt";
  nlohmann::json params = nlohmann::json::object();
  bool operator==(const InputConfig&) const = default;
};

struct OutputConfig {
  std::string id;
  std::string kind;  // "ndi" | "fullscreen" in M1
  bool keyed = false;
  bool enabled = true;
  nlohmann::json params = nlohmann::json::object();
  int width = 0;           // 0 x 0 = same as the show (spec §4.3)
  int height = 0;
  bool halfRate = false;   // every second tick
  bool operator==(const OutputConfig&) const = default;
};

struct CameraConfig {
  std::string id;
  std::string name;
  InputConfig input;
  std::vector<OutputConfig> outputs;
  bool operator==(const CameraConfig&) const = default;
};

struct ShowSettings {
  Rate rate{60, 1};
  int width = 1920;   // show format (spec §1.3): 1280x720 or 1920x1080
  int height = 1080;
  int holdMs = 5000;
  std::string slatePath;  // empty = built-in slate
  bool operator==(const ShowSettings&) const = default;
};

struct Show {
  static constexpr int kSchemaVersion = 1;
  ShowSettings settings;
  std::vector<CameraConfig> cameras;
  CameraConfig* findCamera(std::string_view id);
  const CameraConfig* findCamera(std::string_view id) const;
  bool operator==(const Show&) const = default;
};

class ShowError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

inline constexpr std::array<std::string_view, 1> kSecretKeys{"passphrase"};

enum class SecretMode { Encrypt, Redact, Plain };

nlohmann::json showToJson(const Show& show, SecretMode mode);
Show showFromJson(const nlohmann::json& j);  // throws ShowError
nlohmann::json cameraToJson(const CameraConfig& cam, SecretMode mode);
CameraConfig cameraFromJson(const nlohmann::json& j);  // throws ShowError
nlohmann::json outputToJson(const OutputConfig& out);
OutputConfig outputFromJson(const nlohmann::json& j);  // throws ShowError
std::optional<std::pair<int, int>> parseShowResolution(std::string_view s);  // "1280x720" | "1920x1080"
std::string resolutionStr(int w, int h);
bool isOutputSize(int w, int h);  // 0x0, 1280x720 or 960x540
std::string newId();
}  // namespace bc
