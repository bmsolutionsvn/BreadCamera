#include "engine/command_router.h"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <stdexcept>

#include "core/chart.h"
#include "core/stage_params.h"
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

ChartQuad quadFrom(const json& args) {
  const auto& c = args.contains("corners") ? args["corners"] : json();
  if (!c.is_array() || c.size() != 4) throw CmdError("invalid", "corners must be four [x, y] points");
  ChartQuad q;
  for (size_t i = 0; i < 4; ++i) {
    if (!c[i].is_array() || c[i].size() != 2 || !c[i][0].is_number() || !c[i][1].is_number()) {
      throw CmdError("invalid", "corners must be four [x, y] points");
    }
    q.c[i] = {c[i][0].get<double>(), c[i][1].get<double>()};
  }
  if (!validQuad(q)) throw CmdError("invalid", "place the four corners on the chart");
  return q;
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

// A show may hold each virtual camera number once, so the second camera that asks for the same
// one is refused (spec §4.3). Needs the whole show, so it is not part of validateOutput.
void checkVcamFree(const Show& show, const OutputConfig& o) {
  if (o.kind != "vcam") return;
  const int n = o.params["index"].get<int>();
  for (auto& c : show.cameras)
    for (auto& other : c.outputs)
      if (other.id != o.id && other.kind == "vcam" && other.params.value("index", 0) == n) {
        throw CmdError("invalid", "virtual camera " + std::to_string(n) + " is already used by camera " + c.name);
      }
}

// A capture device belongs to one camera only, so the second camera that asks for the same
// device is refused (spec §4.2). Needs the whole show, so it is not part of validateInput.
void checkDeviceFree(const Show& show, const InputConfig& in, const std::string& selfCameraId) {
  if (in.kind != "uvc") return;
  const auto device = in.params.value("device", std::string());
  for (auto& c : show.cameras)
    if (c.id != selfCameraId && c.input.kind == "uvc" && c.input.params.value("device", std::string()) == device) {
      throw CmdError("invalid", "device " + device + " is already used by camera " + c.name);
    }
}

// spec D9: a Match camera may not match itself, an unknown camera, or a camera whose own match
// chain (directly or through others) leads back to it. Needs the whole show, so it is not part of
// normalizeStageParams. Removing a matched camera is allowed: its matchers hold.
void checkMatchTarget(const Show& show, const CameraConfig& self, const json& colorParams) {
  if (colorParams.value("mode", std::string("calibrate")) != "match") return;
  const std::string to = colorParams.value("matchTo", std::string());
  if (to.empty()) return;  // Auto balance
  if (to == self.id) throw CmdError("invalid", "A camera cannot match itself.");
  const auto* target = show.findCamera(to);
  if (!target) throw CmdError("invalid", "No camera " + to + ".");
  std::string cur = to;
  for (int guard = 0; guard < 256; ++guard) {
    const auto* next = show.findCamera(cur);
    if (!next) break;
    const std::string matchTo = next->stages.value("color", json::object()).value("matchTo", std::string());
    if (matchTo.empty()) break;
    if (matchTo == self.id) {
      throw CmdError("invalid", target->name + " already matches this camera, directly or through another camera.");
    }
    cur = matchTo;
  }
}

PreviewSize sizeArg(const json& args) {
  auto s = parsePreviewSize(args.value("size", std::string("small")));
  if (!s) throw CmdError("invalid", "size must be small or large");
  return *s;
}

void formatFrom(const json& f, OutputConfig& o) {
  if (!f.is_object()) throw CmdError("bad-request", "format must be an object");
  // One frame size per camera: a legacy width/height is ignored, not validated (spec §4.3).
  o.halfRate = f.value("halfRate", false);
}

std::string strParam(const json& p, const char* key, size_t minLen, size_t maxLen, const char* what) {
  if (!p.contains(key) || !p[key].is_string()) throw std::invalid_argument(std::string(what) + " is required");
  auto s = p[key].get<std::string>();
  if (s.size() < minLen || s.size() > maxLen)
    throw std::invalid_argument(std::string(what) + " must be " + std::to_string(minLen) + " to " + std::to_string(maxLen) + " characters");
  return s;
}
int intParam(const json& p, const char* key, int lo, int hi, const char* what) {
  if (!p.contains(key) || !p[key].is_number_integer()) throw std::invalid_argument(std::string(what) + " is required (a whole number)");
  const int v = p[key].get<int>();
  if (v < lo || v > hi) throw std::invalid_argument(std::string(what) + " must be " + std::to_string(lo) + " to " + std::to_string(hi));
  return v;
}
void optInt(const json& p, const char* key, int lo, int hi, const char* what) {
  if (!p.contains(key)) return;
  if (!p[key].is_number_integer()) throw std::invalid_argument(std::string(what) + " must be a whole number");
  const int v = p[key].get<int>();
  if (v != 0 && (v < lo || v > hi))
    throw std::invalid_argument(std::string(what) + " must be 0 (auto) or " + std::to_string(lo) + " to " + std::to_string(hi));
}
}  // namespace

void validateInput(const InputConfig& in) {
  if (in.kind == "srt") {
    srtOptionsFromParams(in.params);
    return;
  }
  if (in.kind == "ndi") {
    strParam(in.params, "source", 1, 256, "NDI source");
    if (in.params.contains("hx") && !in.params["hx"].is_boolean()) throw std::invalid_argument("NDI HX must be true or false");
    return;
  }
  if (in.kind == "omt") {
    // OMT and OMT HX share one source name; the receiver reads the mode from the sender.
    strParam(in.params, "source", 1, 256, "OMT source");
    return;
  }
  if (in.kind == "uvc") {
    strParam(in.params, "device", 1, 512, "device");
    optInt(in.params, "width", 640, 3840, "width");
    optInt(in.params, "height", 360, 2160, "height");
    optInt(in.params, "fps", 15, 60, "fps");
    return;
  }
  if (in.kind == "decklink") {
    intParam(in.params, "device", 0, 15, "DeckLink card");
    return;
  }
  if (in.kind == "aja") {
    intParam(in.params, "device", 0, 7, "AJA card");
    intParam(in.params, "channel", 1, 8, "AJA channel");
    return;
  }
  if (in.kind == "breadcall") {
    // C1: one camera per enabled guest slot. Validation is compile-independent, so it holds
    // whether or not BC_WITH_LIVECALL built the input (the factory reports the missing build).
    strParam(in.params, "url", 1, 512, "BreadCall room URL");
    strParam(in.params, "token", 1, 2048, "BreadCall token");
    intParam(in.params, "slot", 1, 8, "BreadCall slot");
    return;
  }
  throw std::invalid_argument("unknown input kind '" + in.kind + "'");
}

void validateOutput(const OutputConfig& o) {
  if (o.kind == "ndi") {
    if (o.params.contains("name")) {
      if (!o.params["name"].is_string()) throw std::invalid_argument("NDI name must be text");
      const auto n = o.params["name"].get<std::string>();
      if (n.empty() || utf8Length(n) > 200) throw std::invalid_argument("NDI name must be 1 to 200 characters");
    }
    return;
  }
  if (o.kind == "fullscreen") {
    if (o.params.contains("monitor") && (!o.params["monitor"].is_number_integer() || o.params["monitor"].get<int>() < 0)) {
      throw std::invalid_argument("monitor must be a whole number from 0");
    }
    return;
  }
  if (o.kind == "srt") {
    srtOutputOptionsFromParams(o.params);
    return;
  }
  if (o.kind == "omt") {
    if (o.params.contains("mode")) {
      if (!o.params["mode"].is_string()) throw std::invalid_argument("OMT mode must be text");
      const auto m = o.params["mode"].get<std::string>();
      if (m != "hx" && m != "full") throw std::invalid_argument("OMT mode must be 'hx' or 'full'");
    }
    // The codec picks Hx's video codec; plain OMT (full) is codec-less, so a codec on a full
    // output is accepted and ignored, not rejected (spec A4.3).
    if (o.params.contains("codec")) {
      if (!o.params["codec"].is_string()) throw std::invalid_argument("OMT codec must be text");
      const auto c = o.params["codec"].get<std::string>();
      if (c != "hevc" && c != "h264") throw std::invalid_argument("OMT codec must be 'hevc' or 'h264'");
    }
    if (o.params.contains("name")) {
      if (!o.params["name"].is_string()) throw std::invalid_argument("OMT name must be text");
      const auto n = o.params["name"].get<std::string>();
      if (n.empty() || utf8Length(n) > 200) throw std::invalid_argument("OMT name must be 1 to 200 characters");
    }
    return;
  }
  if (o.kind == "decklink") {
    intParam(o.params, "device", 0, 15, "DeckLink card");
    if (o.halfRate) throw std::invalid_argument("this output type has a fixed rate (spec §4.3)");
    return;
  }
  if (o.kind == "aja") {
    intParam(o.params, "device", 0, 7, "AJA card");
    intParam(o.params, "channel", 1, 8, "AJA channel");
    if (o.halfRate) throw std::invalid_argument("this output type has a fixed rate (spec §4.3)");
    return;
  }
  if (o.kind == "vcam") {
    intParam(o.params, "index", 1, 4, "virtual camera number");
    if (o.params.contains("name")) strParam(o.params, "name", 1, 80, "virtual camera name");
    if (o.halfRate) throw std::invalid_argument("this output type has a fixed rate (spec §4.3)");
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
  if (cmd == "listNdiSources") return fx_.listNdiSources();
  if (cmd == "listOmtSources") return fx_.listOmtSources();
  if (cmd == "listCaptureDevices") return fx_.listCaptureDevices();
  if (cmd == "listDeckLinkDevices") return fx_.listDeckLinkDevices();
  if (cmd == "listAjaDevices") return fx_.listAjaDevices();
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

  if (cmd == "getStageSchemas") {
    json out = json::array();
    for (auto& s : stageSchemas()) out.push_back(schemaToJson(s, fx_.stageUnavailable(s.kind)));
    return out;
  }

  if (cmd == "findChart" || cmd == "calibrateColor") {
    const auto camId = str(args, "cameraId");
    {
      std::lock_guard l(m_);
      if (!show_.findCamera(camId)) throw CmdError("not-found", "no camera " + camId);
    }
    if (cmd == "findChart") {
      try {
        return fx_.findChart(camId);
      } catch (const std::exception& e) {
        throw CmdError("invalid", e.what());
      }
    }
    const auto chart = args.value("chart", std::string("card"));
    if (chart != "card" && chart != "tablet") throw CmdError("invalid", "chart must be card or tablet");
    const auto q = quadFrom(args);
    json res;
    try {
      res = fx_.calibrateColor(camId, chart, q);  // ~1 s, without the show lock
    } catch (const std::invalid_argument& e) {
      throw CmdError("invalid", e.what());
    }
    std::lock_guard l(m_);
    Show next = show_;
    auto* c = next.findCamera(camId);
    if (!c) throw CmdError("not-found", "camera " + camId + " was removed during the calibration");
    auto p = normalizeStageParams("color", c->stages.value("color", json::object()));
    p["previousLut"] = p["lut"];
    p["lut"] = res["lutPath"];
    p["enabled"] = true;
    p["chart"] = chart;
    p["deltaE"] = std::min(100.0, res["deltaE"].get<double>());
    p["deltaEBefore"] = std::min(100.0, res["deltaEBefore"].get<double>());
    p["calibratedAt"] = static_cast<double>(std::time(nullptr));
    p["reference"] = res.value("reference", false);
    c->stages["color"] = normalizeStageParams("color", p);
    show_ = std::move(next);
    fx_.showChanged(show_);
    return {{"deltaEBefore", res["deltaEBefore"]},
            {"deltaE", res["deltaE"]},
            {"rating", res["rating"]},
            {"reference", res.value("reference", false)}};
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
    checkDeviceFree(next, c.input, c.id);
    next.cameras.push_back(c);
    result = {{"id", c.id}};
  } else if (cmd == "updateCamera") {
    auto& c = findCam(str(args, "id"));
    if (args.contains("name")) c.name = name(args);
    if (args.contains("input")) {
      c.input = inputFrom(obj(args, "input"), c.input.params);
      checkDeviceFree(next, c.input, c.id);  // only a (re)set can clash; a rename never does
    }
  } else if (cmd == "removeCamera") {
    const auto camId = str(args, "id");
    findCam(camId);
    std::erase_if(next.cameras, [&](const CameraConfig& c) { return c.id == camId; });
    fx_.forgetColor(camId);
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
    checkVcamFree(next, out);
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
    checkVcamFree(next, *it);
  } else if (cmd == "removeOutput") {
    auto& c = findCam(str(args, "cameraId"));
    const auto outId = str(args, "id");
    const auto before = c.outputs.size();
    std::erase_if(c.outputs, [&](const OutputConfig& o) { return o.id == outId; });
    if (c.outputs.size() == before) throw CmdError("not-found", "no output " + outId);
  } else if (cmd == "setStage") {
    auto& c = findCam(str(args, "cameraId"));
    const auto stage = str(args, "stage");
    // Adding an effect this PC cannot run is refused; changing one already on the camera (a show
    // moved from an RTX PC) stays editable.
    if (!c.stages.contains(stage)) {
      const auto why = fx_.stageUnavailable(stage);
      if (!why.empty()) throw CmdError("unavailable", why);
    }
    const auto& params = obj(args, "params");
    json merged = c.stages.value(stage, json::object());
    for (auto& [k, v] : params.items()) merged[k] = v;
    json normalized;
    try {
      normalized = normalizeStageParams(stage, merged);
    } catch (const std::invalid_argument& e) {
      throw CmdError("invalid", e.what());
    }
    if (stage == "color") checkMatchTarget(next, c, normalized);
    c.stages[stage] = std::move(normalized);
  } else if (cmd == "removeStage") {
    auto& c = findCam(str(args, "cameraId"));
    const auto stage = str(args, "stage");
    if (!c.stages.contains(stage)) throw CmdError("not-found", "effect " + stage + " is not on this camera");
    c.stages.erase(stage);
  } else if (cmd == "undoColor") {
    auto& c = findCam(str(args, "cameraId"));
    auto p = normalizeStageParams("color", c.stages.value("color", json::object()));
    if (p["previousLut"] == "") throw CmdError("invalid", "nothing to undo");
    std::swap(p["lut"], p["previousLut"]);
    c.stages["color"] = normalizeStageParams("color", p);
  } else if (cmd == "clearColor") {
    auto& c = findCam(str(args, "cameraId"));
    auto old = normalizeStageParams("color", c.stages.value("color", json::object()));
    auto p = normalizeStageParams("color", json::object());
    p["enabled"] = old["enabled"];
    p["previousLut"] = old["lut"];
    c.stages["color"] = normalizeStageParams("color", p);
    fx_.forgetColor(c.id);
  } else if (cmd == "stopAllOutputs") {
    for (auto& c : next.cameras)
      for (auto& o : c.outputs) o.enabled = false;
  } else if (cmd == "setSettings") {
    if (args.contains("rate")) {
      auto r = parseShowRate(str(args, "rate"));
      if (!r) throw CmdError("invalid", "rate must be 15, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60, 119.88 or 120 fps");
      next.settings.rate = *r;
    }
    // A legacy "resolution" is ignored, not validated (there is no show-wide size any more, D3).
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
