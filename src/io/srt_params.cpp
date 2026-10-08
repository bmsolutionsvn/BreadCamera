#include "io/srt_params.h"

#include <stdexcept>

namespace bc {
namespace {
using nlohmann::json;

int intIn(const json& p, const char* key, int def, int lo, int hi) {
  if (!p.contains(key)) return def;
  if (!p[key].is_number_integer()) throw std::invalid_argument(std::string(key) + " must be a whole number");
  int v = p[key].get<int>();
  if (v < lo || v > hi) {
    throw std::invalid_argument(std::string(key) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
  }
  return v;
}

std::string strOr(const json& p, const char* key, const std::string& def) {
  if (!p.contains(key)) return def;
  if (!p[key].is_string()) throw std::invalid_argument(std::string(key) + " must be text");
  return p[key].get<std::string>();
}
}  // namespace

FfmpegInputOptions srtOptionsFromParams(const json& p) {
  if (!p.is_object()) throw std::invalid_argument("SRT settings must be an object");
  FfmpegInputOptions o;
  const std::string url = strOr(p, "url", "");
  std::string mode = strOr(p, "mode", url.empty() ? "" : "caller");
  if (mode != "caller" && mode != "listener") throw std::invalid_argument("mode must be caller or listener");
  if (!url.empty()) {
    if (url.rfind("srt://", 0) != 0 || url.size() > 2048) throw std::invalid_argument("url must start with srt://");
    o.url = url;
  } else {
    if (!p.contains("port")) throw std::invalid_argument("port is required");
    const int port = intIn(p, "port", 0, 1, 65535);
    std::string host = strOr(p, "host", mode == "listener" ? "0.0.0.0" : "");
    if (host.empty()) throw std::invalid_argument("host is required in caller mode");
    o.url = "srt://" + host + ":" + std::to_string(port);
  }
  o.avOptions["mode"] = mode;
  o.avOptions["latency"] = std::to_string(intIn(p, "latencyMs", 120, 20, 8000) * 1000);
  const std::string pass = strOr(p, "passphrase", "");
  if (!pass.empty()) {
    if (pass.size() < 10 || pass.size() > 79) throw std::invalid_argument("passphrase must be 10 to 79 characters");
    o.avOptions["passphrase"] = pass;
  }
  const std::string sid = strOr(p, "streamId", "");
  if (sid.size() > 512) throw std::invalid_argument("streamId must be at most 512 characters");
  if (!sid.empty()) o.avOptions["streamid"] = sid;
  return o;
}
}  // namespace bc
