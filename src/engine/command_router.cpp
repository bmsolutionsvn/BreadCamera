#include "engine/command_router.h"

#include <windows.h>

#include <algorithm>
#include <stdexcept>

#include "core/utf8.h"
#include "io/srt_params.h"

namespace bc {
namespace {
using nlohmann::json;

struct CmdError : std::runtime_error {
  std::string code;
  CmdError(std::string c, const std::string& msg) : std::runtime_error(msg), code(std::move(c)) {}
};

const json& obj(const json& args, const char* key) {
  if (!args.contains(key) || !args[key].is_object()) throw CmdError("bad-request", std::string(key) + " must be an object");
  return args[key];
}
std::string str(const json& args, const char* key) {
  if (!args.contains(key) || !args[key].is_string()) throw CmdError("bad-request", std::string(key) + " must be text");
  return args[key].get<std::string>();
}
std::string name(const json& args) {
  auto n = str(args, "name");
  if (n.empty() || utf8Length(n) > 80) throw CmdError("invalid", "name must be 1 to 80 characters");
  return n;
}

// Replaces {"set":true} with the stored secret and {"set":false} with "".
json mergeSecrets(json incoming, const json& stored) {
  for (auto key : kSecretKeys) {
    const std::string k(key);
    if (!incoming.contains(k) || !incoming[k].is_object()) continue;
    const bool keep = incoming[k].value("set", false);
    incoming[k] = keep && stored.contains(k) ? stored[k] : json("");
  }
  return incoming;
}

InputConfig inputFrom(const json& j, const json& storedParams = json::object()) {
  InputConfig in;
  in.kind = str(j, "kind");
  in.params = j.value("params", json::object());
  if (!in.params.is_object()) throw CmdError("bad-request", "params must be an object");
  in.params = mergeSecrets(in.params, storedParams);
  try {
    validateInput(in);
  } catch (const std::invalid_argument& e) {
    throw CmdError("invalid", e.what());
  }
  return in;
}

void checkOutput(const OutputConfig& o) {
  try {
    validateOutput(o);
  } catch (const std::invalid_argument& e) {
    throw CmdError("invalid", e.what());
  }
}

PreviewSize sizeArg(const json& args) {
  auto s = parsePreviewSize(args.value("size", std::string("small")));
  if (!s) throw CmdError("invalid", "size must be small or large");
  return *s;
}

void formatFrom(const json& f, OutputConfig& o) {
  if (!f.is_object()) throw CmdError("bad-request", "format must be an object");
  o.width = f.value("width", 0);
  o.height = f.value("height", 0);
  o.halfRate = f.value("halfRate", false);
  if (!isOutputSize(o.width, o.height)) throw CmdError("invalid", "output size must be same as show, 1280x720 or 960x540");
}
}  // namespace

void validateInput(const InputConfig& in) {
  if (in.kind == "srt") {
    srtOptionsFromParams(in.params);
    return;
  }
  throw std::invalid_argument("unknown input kind '" + in.kind + "'");
}

void validateOutput(const OutputConfig& o) {
  if (o.kind == "ndi") {
    if (o.params.contains("name")) {
      if (!o.params["name"].is_string()) throw std::invalid_argument("NDI name must be text");
      const auto n = o.params["name"].get<std::string>();
      if (n.empty() || n.size() > 200) throw std::invalid_argument("NDI name must be 1 to 200 characters");
    }
    return;
  }
  if (o.kind == "fullscreen") {
    if (o.params.contains("monitor") && (!o.params["monitor"].is_number_integer() || o.params["monitor"].get<int>() < 0)) {
      throw std::invalid_argument("monitor must be a whole number from 0");
    }
    return;
  }
  throw std::invalid_argument("unknown output kind '" + o.kind + "'");
}

Show CommandRouter::show() const {
  std::lock_guard l(m_);
  return show_;
}

json CommandRouter::handle(const ClientId& client, std::string_view text) {
  json id = nullptr;
  try {
    if (text.size() > 65536) throw CmdError("too-large", "message is larger than 64 KiB");
    json req;
    try {
      req = json::parse(text);
    } catch (const json::exception&) {
      throw CmdError("bad-json", "message is not valid JSON");
    }
    if (!req.is_object()) throw CmdError("bad-request", "message must be a JSON object");
    if (req.contains("id") && (req["id"].is_number() || req["id"].is_string())) id = req["id"];
    if (!req.contains("cmd") || !req["cmd"].is_string()) throw CmdError("bad-request", "cmd must be text");
    const json args = req.value("args", json::object());
    if (!args.is_object()) throw CmdError("bad-request", "args must be an object");
    json result = dispatch(client, req["cmd"].get<std::string>(), args);
    return {{"id", id}, {"ok", true}, {"result", result}};
  } catch (const CmdError& e) {
    return {{"id", id}, {"ok", false}, {"error", {{"code", e.code}, {"message", e.what()}}}};
  } catch (const json::exception& e) {
    return {{"id", id}, {"ok", false}, {"error", {{"code", "bad-request"}, {"message", e.what()}}}};
  } catch (const std::exception& e) {
    return {{"id", id}, {"ok", false}, {"error", {{"code", "invalid"}, {"message", e.what()}}}};
  }
}

json CommandRouter::dispatch(const ClientId& client, const std::string& cmd, const json& args) {
  if (cmd == "ping") return {{"pong", true}, {"pid", GetCurrentProcessId()}};
  if (cmd == "listMonitors") return fx_.listMonitors();
  if (cmd == "shutdown") {
    fx_.shutdown();
    return json::object();
  }
  if (cmd == "subscribePreview") {
    if (!args.contains("cameraIds") || !args["cameraIds"].is_array()) throw CmdError("bad-request", "cameraIds must be a list");
    std::vector<std::string> ids;
    for (auto& v : args["cameraIds"]) {
      if (!v.is_string()) throw CmdError("bad-request", "cameraIds must be text");
      ids.push_back(v.get<std::string>());
    }
    fx_.subscribePreview(client, std::move(ids), sizeArg(args));
    return json::object();
  }
  if (cmd == "requestKeyframe") {
    fx_.requestKeyframe(str(args, "cameraId"), sizeArg(args));
    return json::object();
  }

  std::lock_guard l(m_);
  if (cmd == "getShow") return showToJson(show_, SecretMode::Redact);

  Show next = show_;
  json result = json::object();
  auto findCam = [&](const std::string& camId) -> CameraConfig& {
    auto* c = next.findCamera(camId);
    if (!c) throw CmdError("not-found", "no camera " + camId);
    return *c;
  };
  if (cmd == "addCamera") {
    CameraConfig c;
    c.id = newId();
    c.name = name(args);
    c.input = inputFrom(obj(args, "input"));
    next.cameras.push_back(c);
    result = {{"id", c.id}};
  } else if (cmd == "updateCamera") {
    auto& c = findCam(str(args, "id"));
    if (args.contains("name")) c.name = name(args);
    if (args.contains("input")) c.input = inputFrom(obj(args, "input"), c.input.params);
  } else if (cmd == "removeCamera") {
    const auto camId = str(args, "id");
    findCam(camId);
    std::erase_if(next.cameras, [&](const CameraConfig& c) { return c.id == camId; });
  } else if (cmd == "addOutput") {
    auto& c = findCam(str(args, "cameraId"));
    const auto& o = obj(args, "output");
    OutputConfig out;
    out.id = newId();
    out.kind = str(o, "kind");
    out.keyed = o.value("keyed", false);
    out.enabled = o.value("enabled", true);
    out.params = o.value("params", json::object());
    if (!out.params.is_object()) throw CmdError("bad-request", "params must be an object");
    if (o.contains("format")) formatFrom(o["format"], out);
    checkOutput(out);
    c.outputs.push_back(out);
    result = {{"id", out.id}};
  } else if (cmd == "updateOutput") {
    auto& c = findCam(str(args, "cameraId"));
    const auto outId = str(args, "id");
    auto it = std::find_if(c.outputs.begin(), c.outputs.end(), [&](const OutputConfig& o) { return o.id == outId; });
    if (it == c.outputs.end()) throw CmdError("not-found", "no output " + outId);
    if (args.contains("keyed")) it->keyed = args["keyed"].get<bool>();
    if (args.contains("enabled")) it->enabled = args["enabled"].get<bool>();
    if (args.contains("params")) it->params = obj(args, "params");
    if (args.contains("format")) formatFrom(args["format"], *it);
    checkOutput(*it);
  } else if (cmd == "removeOutput") {
    auto& c = findCam(str(args, "cameraId"));
    const auto outId = str(args, "id");
    const auto before = c.outputs.size();
    std::erase_if(c.outputs, [&](const OutputConfig& o) { return o.id == outId; });
    if (c.outputs.size() == before) throw CmdError("not-found", "no output " + outId);
  } else if (cmd == "stopAllOutputs") {
    for (auto& c : next.cameras)
      for (auto& o : c.outputs) o.enabled = false;
  } else if (cmd == "setSettings") {
    if (args.contains("rate")) {
      auto r = parseShowRate(str(args, "rate"));
      if (!r) throw CmdError("invalid", "rate must be 50/1, 60000/1001 or 60/1");
      next.settings.rate = *r;
    }
    if (args.contains("resolution")) {
      auto res = parseShowResolution(str(args, "resolution"));
      if (!res) throw CmdError("invalid", "resolution must be 1280x720 or 1920x1080");
      next.settings.width = res->first;
      next.settings.height = res->second;
    }
    if (args.contains("holdMs")) {
      if (!args["holdMs"].is_number_integer()) throw CmdError("bad-request", "holdMs must be a whole number");
      const int h = args["holdMs"].get<int>();
      if (h < 0 || h > 60000) throw CmdError("invalid", "holdMs must be 0 to 60000");
      next.settings.holdMs = h;
    }
    if (args.contains("slatePath")) next.settings.slatePath = str(args, "slatePath");
  } else {
    throw CmdError("unknown-command", "unknown command '" + cmd + "'");
  }
  show_ = std::move(next);
  fx_.showChanged(show_);
  return result;
}
}  // namespace bc
