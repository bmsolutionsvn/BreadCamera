#pragma once
#include <chrono>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace bc {
struct FfmpegInputOptions {
  std::string url;
  std::map<std::string, std::string> avOptions;  // protocol/format options (secrets live here)
  bool reconnect = true;
  bool allowHwDecode = true;
  bool realtime = false;  // pace reads by timestamps (file inputs in tests)
  std::chrono::milliseconds readTimeout{2000};  // no packet this long = stalled
};

// Validates show params for the "srt" input kind (spec §4.2). Throws std::invalid_argument.
FfmpegInputOptions srtOptionsFromParams(const nlohmann::json& params);
}  // namespace bc
