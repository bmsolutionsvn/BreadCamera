#include <doctest/doctest.h>

#include "core/utf8.h"
#include "engine/command_router.h"

using nlohmann::json;

namespace {
struct FakeFx : bc::CommandRouter::Effects {
  int changes = 0;
  bc::Show last;
  bool quit = false;
  void showChanged(const bc::Show& s) override {
    ++changes;
    last = s;
  }
  void subscribePreview(const bc::ClientId&, std::vector<std::string>, bc::PreviewSize) override {}
  void requestKeyframe(const std::string&, bc::PreviewSize) override {}
  json listMonitors() override { return json::array({{{"index", 0}}}); }
  json listNdiSources() override { return json::array({"STUDIO (vMix - Output 1)"}); }
  json listCaptureDevices() override { return json::array({{{"name", "Cam Link 4K"}, {"id", "\\\\?\\usb#vid_0fd9"}}}); }
  json listDeckLinkDevices() override {
    return json::array({{{"index", 0}, {"name", "DeckLink Quad HDMI"}, {"hasInput", true}, {"hasOutput", true}, {"externalKeying", true}}});
  }
  json listAjaDevices() override { return json::array({{{"index", 0}, {"name", "Corvid 44"}}}); }
  std::string lastChart;
  std::vector<std::string> forgotten;
  json findChart(const std::string&) override { return {{"available", false}, {"found", false}}; }
  json calibrateColor(const std::string&, const std::string& chart, const bc::ChartQuad&) override {
    lastChart = chart;
    return {{"lutPath", "C:/luts/x.cube"}, {"deltaEBefore", 7.4}, {"deltaE", 1.1}, {"rating", "good"}, {"reference", false}};
  }
  void forgetColor(const std::string& id) override { forgotten.push_back(id); }
  void shutdown() override { quit = true; }
};

json call(bc::CommandRouter& r, json req) { return r.handle("c1", req.dump()); }

const json kSrt = {{"kind", "srt"}, {"params", {{"mode", "listener"}, {"port", 9000}, {"passphrase", "0123456789"}}}};
}  // namespace

TEST_CASE("CommandRouter: addCamera validates, assigns an id and redacts secrets") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto res = call(r, {{"id", 1}, {"cmd", "addCamera"}, {"args", {{"name", "Cam 1"}, {"input", kSrt}}}});
  REQUIRE(res["ok"] == true);
  CHECK(res["id"] == 1);
  const auto id = res["result"]["id"].get<std::string>();
  CHECK(id.size() == 8);
  CHECK(fx.changes == 1);
  CHECK(fx.last.cameras[0].input.params["passphrase"] == "0123456789");
  auto shown = call(r, {{"id", 2}, {"cmd", "getShow"}});
  CHECK(shown["result"]["cameras"][0]["input"]["params"]["passphrase"] == json{{"set", true}});
}

TEST_CASE("CommandRouter: invalid input is refused and the show is unchanged") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  json bad = kSrt;
  bad["params"]["passphrase"] = "short";
  auto res = call(r, {{"id", 1}, {"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", bad}}}});
  CHECK(res["ok"] == false);
  CHECK(res["error"]["code"] == "invalid");
  CHECK(res["error"]["message"].get<std::string>().find("passphrase") != std::string::npos);
  CHECK(fx.changes == 0);
  CHECK(r.show().cameras.empty());
  auto noName = call(r, {{"id", 2}, {"cmd", "addCamera"}, {"args", {{"name", ""}, {"input", kSrt}}}});
  CHECK(noName["error"]["code"] == "invalid");
}

TEST_CASE("CommandRouter: updateCamera keeps or clears a redacted secret") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto id = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  json keep = kSrt;
  keep["params"]["passphrase"] = {{"set", true}};
  keep["params"]["port"] = 9001;
  REQUIRE(call(r, {{"cmd", "updateCamera"}, {"args", {{"id", id}, {"input", keep}}}})["ok"] == true);
  CHECK(r.show().cameras[0].input.params["passphrase"] == "0123456789");
  CHECK(r.show().cameras[0].input.params["port"] == 9001);
  json clear = kSrt;
  clear["params"]["passphrase"] = {{"set", false}};
  REQUIRE(call(r, {{"cmd", "updateCamera"}, {"args", {{"id", id}, {"input", clear}}}})["ok"] == true);
  CHECK(r.show().cameras[0].input.params["passphrase"] == "");
}

TEST_CASE("CommandRouter: outputs, stopAllOutputs and settings") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  auto add = call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "fullscreen"}, {"params", {{"monitor", 0}}}}}}}});
  REQUIRE(add["ok"] == true);
  CHECK(call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "teleport"}}}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "fullscreen"}, {"params", {{"monitor", -1}}}}}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", "nope"}, {"output", {{"kind", "ndi"}}}}}})["error"]["code"] == "not-found");
  REQUIRE(call(r, {{"cmd", "stopAllOutputs"}})["ok"] == true);
  CHECK(r.show().cameras[0].outputs[0].enabled == false);
  CHECK(call(r, {{"cmd", "setSettings"}, {"args", {{"rate", "30/1"}}}})["error"]["code"] == "invalid");
  REQUIRE(call(r, {{"cmd", "setSettings"}, {"args", {{"rate", "50/1"}, {"holdMs", 2000}}}})["ok"] == true);
  CHECK(r.show().settings.rate == bc::Rate{50, 1});
  CHECK(r.show().settings.holdMs == 2000);
  REQUIRE(call(r, {{"cmd", "removeOutput"}, {"args", {{"cameraId", cam}, {"id", add["result"]["id"]}}}})["ok"] == true);
  REQUIRE(call(r, {{"cmd", "removeCamera"}, {"args", {{"id", cam}}}})["ok"] == true);
  CHECK(call(r, {{"cmd", "removeCamera"}, {"args", {{"id", cam}}}})["error"]["code"] == "not-found");
}

TEST_CASE("CommandRouter: hostile or malformed messages get an error, never a crash") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  CHECK(r.handle("c1", "not json")["error"]["code"] == "bad-json");
  CHECK(r.handle("c1", "[]")["error"]["code"] == "bad-request");
  CHECK(r.handle("c1", R"({"cmd":5})")["error"]["code"] == "bad-request");
  CHECK(r.handle("c1", R"({"id":7,"cmd":"addCamera","args":"x"})")["error"]["code"] == "bad-request");
  CHECK(r.handle("c1", R"({"id":7,"cmd":"addCamera","args":{"name":5}})")["error"]["code"] == "bad-request");
  auto unknown = r.handle("c1", R"({"id":7,"cmd":"fly"})");
  CHECK(unknown["error"]["code"] == "unknown-command");
  CHECK(unknown["id"] == 7);
  CHECK(r.handle("c1", std::string(100'000, ' '))["error"]["code"] == "too-large");
  CHECK(fx.changes == 0);
  CHECK(call(r, {{"cmd", "listMonitors"}})["result"].size() == 1);
  CHECK(call(r, {{"cmd", "ping"}})["result"]["pong"] == true);
  call(r, {{"cmd", "shutdown"}});
  CHECK(fx.quit);
}

namespace {
// An effect the GPU cannot run: Clean up is unavailable on this PC.
struct UnavailFx : FakeFx {
  std::string stageUnavailable(const std::string& kind) override {
    return kind == "cleanup" ? "Needs an NVIDIA RTX card." : "";
  }
};

// Records the preview effects so the two routing-only commands are pinned too.
struct RecFx : bc::CommandRouter::Effects {
  std::vector<bc::ClientId> subClients;
  std::vector<std::vector<std::string>> subIds;
  std::vector<bc::PreviewSize> subSizes;
  std::vector<std::string> keyIds;
  std::vector<bc::PreviewSize> keySizes;
  void showChanged(const bc::Show&) override {}
  void subscribePreview(const bc::ClientId& c, std::vector<std::string> ids, bc::PreviewSize s) override {
    subClients.push_back(c);
    subIds.push_back(std::move(ids));
    subSizes.push_back(s);
  }
  void requestKeyframe(const std::string& id, bc::PreviewSize s) override {
    keyIds.push_back(id);
    keySizes.push_back(s);
  }
  json listMonitors() override { return json::array(); }
  json listNdiSources() override { return json::array(); }
  json listCaptureDevices() override { return json::array(); }
  json listDeckLinkDevices() override { return json::array(); }
  json listAjaDevices() override { return json::array(); }
  json findChart(const std::string&) override { return json::object(); }
  json calibrateColor(const std::string&, const std::string&, const bc::ChartQuad&) override { return json::object(); }
  void forgetColor(const std::string&) override {}
  void shutdown() override {}
};
}  // namespace

TEST_CASE("CommandRouter: preview commands reach the effects, bad arguments do not") {
  RecFx fx;
  bc::CommandRouter r({}, fx);
  REQUIRE(call(r, {{"cmd", "subscribePreview"}, {"args", {{"cameraIds", {"a", "b"}}, {"size", "large"}}}})["ok"] == true);
  REQUIRE(fx.subIds.size() == 1);
  CHECK(fx.subClients[0] == "c1");
  const std::vector<std::string> want{"a", "b"};
  CHECK(fx.subIds[0] == want);
  CHECK(fx.subSizes[0] == bc::PreviewSize::Large);
  CHECK(call(r, {{"cmd", "subscribePreview"}, {"args", {{"cameraIds", 5}}}})["error"]["code"] == "bad-request");
  CHECK(call(r, {{"cmd", "subscribePreview"}, {"args", {{"cameraIds", {1}}}}})["error"]["code"] == "bad-request");
  CHECK(call(r, {{"cmd", "subscribePreview"}, {"args", {{"cameraIds", json::array()}, {"size", "huge"}}}})["error"]["code"] == "invalid");
  CHECK(fx.subIds.size() == 1);  // none of the bad ones reached an effect

  REQUIRE(call(r, {{"cmd", "requestKeyframe"}, {"args", {{"cameraId", "a"}, {"size", "small"}}}})["ok"] == true);
  REQUIRE(fx.keyIds.size() == 1);
  CHECK(fx.keyIds[0] == "a");
  CHECK(fx.keySizes[0] == bc::PreviewSize::Small);
  CHECK(call(r, {{"cmd", "requestKeyframe"}, {"args", {{"size", "small"}}}})["error"]["code"] == "bad-request");
  CHECK(call(r, {{"cmd", "requestKeyframe"}, {"args", {{"cameraId", 7}, {"size", "small"}}}})["error"]["code"] == "bad-request");
  CHECK(call(r, {{"cmd", "requestKeyframe"}, {"args", {{"cameraId", "a"}, {"size", "huge"}}}})["error"]["code"] == "invalid");
  CHECK(fx.keyIds.size() == 1);  // failures never fire an effect
}

TEST_CASE("CommandRouter: a legacy show resolution is ignored, and an output size is ignored") {
  FakeFx fx;
  bc::CommandRouter r(bc::Show{}, fx);
  // A legacy resolution is ignored, not validated: there is no show-wide size any more (D3).
  CHECK(call(r, {{"cmd", "setSettings"}, {"args", {{"resolution", "1280x720"}}}})["ok"] == true);
  CHECK(call(r, {{"cmd", "setSettings"}, {"args", {{"resolution", "4k"}}}})["ok"] == true);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  auto ok = call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "ndi"}, {"format", {{"width", 960}, {"height", 540}, {"halfRate", true}}}}}}}});
  REQUIRE(ok["ok"] == true);
  CHECK(r.show().cameras[0].outputs[0].halfRate);
  // A legacy per-output size is ignored, not rejected (one frame size per camera).
  CHECK(call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "ndi"}, {"format", {{"width", 1000}, {"height", 500}}}}}}}})["ok"] == true);
  REQUIRE(call(r, {{"cmd", "updateOutput"}, {"args", {{"cameraId", cam}, {"id", ok["result"]["id"]}, {"format", {{"width", 0}, {"height", 0}, {"halfRate", false}}}}}})["ok"] == true);
  CHECK_FALSE(r.show().cameras[0].outputs[0].halfRate);
}

TEST_CASE("validateOutput ignores a per-output size") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", kSrt}}}})["result"]["id"];
  // A fixed-format output (the virtual camera) used to refuse any per-output size; a legacy
  // size is now ignored, not an error (one frame size per camera).
  auto add = call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "vcam"}, {"params", {{"index", 1}}}, {"format", {{"width", 1280}, {"height", 720}, {"halfRate", false}}}}}}}});
  CHECK(add["ok"] == true);
}

TEST_CASE("CommandRouter: names are counted in characters, not bytes") {
  FakeFx fx;
  bc::CommandRouter r(bc::Show{}, fx);
  std::string eighty;
  for (int i = 0; i < 80; ++i) eighty += "ấ";  // 3 bytes each
  CHECK(call(r, {{"cmd", "addCamera"}, {"args", {{"name", eighty}, {"input", kSrt}}}})["ok"] == true);
  CHECK(call(r, {{"cmd", "addCamera"}, {"args", {{"name", eighty + "a"}, {"input", kSrt}}}})["error"]["code"] == "invalid");
  CHECK(r.show().cameras[0].name == eighty);
}

TEST_CASE("CommandRouter: the NDI name limit counts code points, not bytes") {
  // A Vietnamese name of <= 200 characters but > 200 bytes must be accepted (spec 6.4).
  bc::OutputConfig o;
  o.kind = "ndi";
  std::string many;
  while (bc::utf8Length(many) + 16 <= 200) many += "Sân khấu trái – ";  // 16 code points, 21 bytes each
  REQUIRE(bc::utf8Length(many) <= 200);
  REQUIRE(many.size() > 200);
  o.params["name"] = many;
  CHECK_NOTHROW(bc::validateOutput(o));

  std::string tooLong = many;
  while (bc::utf8Length(tooLong) <= 200) tooLong += "ấ";
  REQUIRE(bc::utf8Length(tooLong) == 201);
  o.params["name"] = tooLong;
  CHECK_THROWS_WITH_AS(bc::validateOutput(o), "NDI name must be 1 to 200 characters", std::invalid_argument);
}

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
  CHECK(call(r, {{"cmd", "getStageSchemas"}})["result"].size() == 5);
}

TEST_CASE("CommandRouter: an unavailable effect cannot be added, but one already on a camera can be changed") {
  UnavailFx fx;
  bc::CommandRouter r({}, fx);
  bool sawCleanup = false;
  const json schemes = call(r, {{"cmd", "getStageSchemas"}})["result"];
  for (auto& s : schemes) {
    if (s["kind"] == "cleanup") {
      sawCleanup = true;
      CHECK(s["unavailable"] == "Needs an NVIDIA RTX card.");
    }
  }
  CHECK(sawCleanup);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  auto refused = call(r, {{"cmd", "setStage"}, {"args", {{"cameraId", cam}, {"stage", "cleanup"}, {"params", {{"enabled", true}}}}}});
  CHECK(refused["error"]["code"] == "unavailable");
  CHECK(refused["error"]["message"] == "Needs an NVIDIA RTX card.");
  CHECK_FALSE(r.show().cameras[0].stages.contains("cleanup"));
  // A show moved from an RTX PC already has Clean up on the camera: changing it is always allowed.
  bc::Show loaded;
  bc::CameraConfig c;
  c.id = "aaaa1111";
  c.name = "Cam";
  c.stages["cleanup"] = json{{"enabled", false}};
  loaded.cameras.push_back(c);
  bc::CommandRouter r2(loaded, fx);
  CHECK(call(r2, {{"cmd", "setStage"}, {"args", {{"cameraId", "aaaa1111"}, {"stage", "cleanup"}, {"params", {{"enabled", true}}}}}})["ok"] == true);
  CHECK(r2.show().cameras[0].stages["cleanup"]["enabled"] == true);
}

TEST_CASE("CommandRouter: removeStage takes an effect off the camera") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  REQUIRE(call(r, {{"cmd", "setStage"}, {"args", {{"cameraId", cam}, {"stage", "background"}, {"params", {{"enabled", true}, {"mode", "blur"}}}}}})["ok"] == true);
  CHECK(r.show().cameras[0].stages.contains("background"));
  REQUIRE(call(r, {{"cmd", "removeStage"}, {"args", {{"cameraId", cam}, {"stage", "background"}}}})["ok"] == true);
  CHECK_FALSE(r.show().cameras[0].stages.contains("background"));
  CHECK(call(r, {{"cmd", "removeStage"}, {"args", {{"cameraId", cam}, {"stage", "background"}}}})["error"]["code"] == "not-found");
}

TEST_CASE("CommandRouter: Match refuses itself, unknown cameras and loops") {
  // spec D9/D16: a camera may match another, never itself, never an unknown id, and never a
  // camera whose own match chain loops back to it.
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto add = [&](std::string name) {
    return call(r, {{"cmd", "addCamera"}, {"args", {{"name", name}, {"input", kSrt}}}})["result"]["id"].get<std::string>();
  };
  auto set = [&](const std::string& cam, json params) {
    return call(r, {{"cmd", "setStage"}, {"args", {{"cameraId", cam}, {"stage", "color"}, {"params", params}}}});
  };
  const auto a = add("A");
  const auto b = add("B");
  const auto c = add("C");

  auto self = set(a, {{"enabled", true}, {"mode", "match"}, {"matchTo", a}});
  CHECK(self["error"]["code"] == "invalid");
  CHECK(self["error"]["message"] == "A camera cannot match itself.");

  auto unknown = set(a, {{"enabled", true}, {"mode", "match"}, {"matchTo", "zz"}});
  CHECK(unknown["error"]["code"] == "invalid");
  CHECK(unknown["error"]["message"] == "No camera zz.");

  REQUIRE(set(a, {{"enabled", true}, {"mode", "match"}, {"matchTo", b}})["ok"] == true);
  // b -> a is refused: a already matches b.
  auto loop = set(b, {{"enabled", true}, {"mode", "match"}, {"matchTo", a}});
  CHECK(loop["error"]["code"] == "invalid");
  CHECK(loop["error"]["message"] == "A already matches this camera, directly or through another camera.");

  REQUIRE(set(b, {{"enabled", true}, {"mode", "match"}, {"matchTo", c}})["ok"] == true);
  // c -> a is refused: a -> b -> c loops back to c.
  auto loop2 = set(c, {{"enabled", true}, {"mode", "match"}, {"matchTo", a}});
  CHECK(loop2["error"]["code"] == "invalid");
  CHECK(loop2["error"]["message"] == "A already matches this camera, directly or through another camera.");

  // Removing a camera others match is allowed: the others hold (spec D12).
  CHECK(call(r, {{"cmd", "removeCamera"}, {"args", {{"id", b}}}})["ok"] == true);
}

TEST_CASE("CommandRouter: calibrateColor stores the result; undo, clear and removal") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  const json corners = json::array({json::array({0.25, 0.22}), json::array({0.75, 0.22}), json::array({0.75, 0.78}), json::array({0.25, 0.78})});
  auto res = call(r, {{"cmd", "calibrateColor"}, {"args", {{"cameraId", cam}, {"chart", "card"}, {"corners", corners}}}});
  REQUIRE(res["ok"] == true);
  CHECK(res["result"]["rating"] == "good");
  auto c = r.show().cameras[0].stages["color"];
  CHECK(c["enabled"] == true);
  CHECK(c["lut"] == "C:/luts/x.cube");
  CHECK(c["deltaE"] == 1.1);
  CHECK(c["calibratedAt"].get<double>() > 1.7e9);
  CHECK(call(r, {{"cmd", "calibrateColor"}, {"args", {{"cameraId", cam}, {"chart", "poster"}, {"corners", corners}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "calibrateColor"}, {"args", {{"cameraId", cam}, {"chart", "card"}, {"corners", json::array()}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "undoColor"}, {"args", {{"cameraId", cam}}}})["error"]["code"] == "invalid");  // previousLut was empty
  REQUIRE(call(r, {{"cmd", "clearColor"}, {"args", {{"cameraId", cam}}}})["ok"] == true);
  CHECK(r.show().cameras[0].stages["color"]["lut"] == "");
  CHECK(r.show().cameras[0].stages["color"]["previousLut"] == "C:/luts/x.cube");
  REQUIRE(call(r, {{"cmd", "undoColor"}, {"args", {{"cameraId", cam}}}})["ok"] == true);
  CHECK(r.show().cameras[0].stages["color"]["lut"] == "C:/luts/x.cube");
  call(r, {{"cmd", "removeCamera"}, {"args", {{"id", cam}}}});
  CHECK(fx.forgotten.size() == 2);  // clearColor and removeCamera
}

TEST_CASE("CommandRouter: new input and output kinds are validated") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto ok = [&](json input) { return call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", input}}}})["ok"] == true; };
  CHECK(ok({{"kind", "ndi"}, {"params", {{"source", "STUDIO (vMix - Output 1)"}}}}));
  CHECK_FALSE(ok({{"kind", "ndi"}, {"params", {{"source", ""}}}}));
  CHECK(ok({{"kind", "uvc"}, {"params", {{"device", "Cam Link 4K"}}}}));
  CHECK_FALSE(ok({{"kind", "uvc"}, {"params", {{"device", "x"}, {"fps", 240}}}}));
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", kSrt}}}})["result"]["id"];
  auto addOut = [&](json o) { return call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", o}}}}); };
  CHECK(addOut({{"kind", "srt"}, {"params", {{"mode", "listener"}, {"port", 9200}}}})["ok"] == true);
  CHECK(addOut({{"kind", "srt"}, {"params", {{"mode", "listener"}, {"port", 0}}}})["error"]["code"] == "invalid");
  CHECK(addOut({{"kind", "srt"}, {"params", {{"mode", "listener"}, {"port", 9200}, {"bitrateKbps", 999999}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "listNdiSources"}})["result"][0] == "STUDIO (vMix - Output 1)");
  CHECK(call(r, {{"cmd", "listCaptureDevices"}})["result"][0]["name"] == "Cam Link 4K");
}

TEST_CASE("CommandRouter: card and virtual camera kinds") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto addCam = [&](json input) { return call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", input}}}}); };
  CHECK(addCam({{"kind", "decklink"}, {"params", {{"device", 0}}}})["ok"] == true);
  CHECK(addCam({{"kind", "aja"}, {"params", {{"device", 0}, {"channel", 9}}}})["error"]["code"] == "invalid");
  auto a = addCam({{"kind", "aja"}, {"params", {{"device", 0}, {"channel", 1}}}})["result"]["id"];
  auto b = addCam(kSrt)["result"]["id"];
  auto out = [&](json cam, json o) { return call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", o}}}}); };
  CHECK(out(a, {{"kind", "vcam"}, {"params", {{"index", 1}}}})["ok"] == true);
  auto dup = out(b, {{"kind", "vcam"}, {"params", {{"index", 1}}}});
  CHECK(dup["error"]["code"] == "invalid");
  CHECK(dup["error"]["message"].get<std::string>().find("1") != std::string::npos);
  CHECK(out(b, {{"kind", "vcam"}, {"params", {{"index", 5}}}})["error"]["code"] == "invalid");
  CHECK(out(b, {{"kind", "decklink"}, {"keyed", true}, {"params", {{"device", 0}}}})["ok"] == true);
  CHECK(call(r, {{"cmd", "listDeckLinkDevices"}})["result"].size() == 1);
  CHECK(call(r, {{"cmd", "listAjaDevices"}})["result"].size() == 1);
}

TEST_CASE("CommandRouter: breadcall input validates the room URL, token and slot") {
  // C1: a BreadCall guest slot is one camera. Validation is compile-independent, so this
  // passes with BC_WITH_LIVECALL on or off.
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto addCam = [&](json input) { return call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", input}}}}); };
  const json ok = {{"kind", "breadcall"},
                   {"params", {{"url", "wss://livekit.example.com"}, {"token", "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ4In0.sig"}, {"slot", 2}}}};
  CHECK(addCam(ok)["ok"] == true);
  REQUIRE(r.show().cameras.size() == 1);
  CHECK(r.show().cameras[0].input.params["slot"] == 2);

  json badSlot = ok;
  badSlot["params"]["slot"] = 9;
  auto res = addCam(badSlot);
  CHECK(res["error"]["code"] == "invalid");
  CHECK(res["error"]["message"].get<std::string>().find("slot") != std::string::npos);

  json noToken = ok;
  noToken["params"].erase("token");
  auto res2 = addCam(noToken);
  CHECK(res2["error"]["code"] == "invalid");
  CHECK(res2["error"]["message"].get<std::string>().find("token") != std::string::npos);

  json textSlot = ok;
  textSlot["params"]["slot"] = "2";
  CHECK(addCam(textSlot)["error"]["code"] == "invalid");

  json noUrl = ok;
  noUrl["params"].erase("url");
  CHECK(addCam(noUrl)["error"]["code"] == "invalid");

  CHECK(r.show().cameras.size() == 1);  // only the valid one was added
}

TEST_CASE("CommandRouter: SDI and virtual camera outputs ignore a size but refuse half rate") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", kSrt}}}})["result"]["id"];
  // A per-output size is ignored (one frame size per camera)...
  auto sized = call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "vcam"}, {"params", {{"index", 1}}}, {"format", {{"width", 1280}, {"height", 720}, {"halfRate", false}}}}}}}});
  CHECK(sized["ok"] == true);
  // ...but half rate is still refused: the fixed-format outputs have a fixed rate.
  auto half = call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "vcam"}, {"params", {{"index", 2}}}, {"format", {{"halfRate", true}}}}}}}});
  CHECK(half["error"]["code"] == "invalid");
}

TEST_CASE("output kind omt validates its params") {
  using nlohmann::json;
  CHECK_NOTHROW(bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt"}));
  CHECK_NOTHROW(bc::validateOutput(
      bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "full"}, {"name", "Cam"}}}));
  CHECK_THROWS_AS(
      bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "fast"}}}),
      std::invalid_argument);
  CHECK_THROWS_AS(
      bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"name", 5}}}),
      std::invalid_argument);
  // An hx output picks HEVC or H.264; anything else is refused.
  CHECK_NOTHROW(bc::validateOutput(
      bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "hx"}, {"codec", "h264"}}}));
  CHECK_THROWS_WITH_AS(
      bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "hx"}, {"codec", "vp9"}}}),
      "OMT codec must be 'hevc' or 'h264'", std::invalid_argument);
  CHECK_THROWS_AS(
      bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"codec", 5}}}),
      std::invalid_argument);
  // Plain OMT (full) is codec-less, but a codec on a full output is accepted and ignored.
  CHECK_NOTHROW(bc::validateOutput(
      bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "full"}, {"codec", "h264"}}}));
}

TEST_CASE("CommandRouter: a capture device cannot belong to two cameras") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto addCam = [&](std::string name, json input) {
    return call(r, {{"cmd", "addCamera"}, {"args", {{"name", name}, {"input", input}}}});
  };
  const json camLink = {{"kind", "uvc"}, {"params", {{"device", "Cam Link 4K"}}}};
  auto a = addCam("Camera A", camLink)["result"]["id"];
  auto dup = addCam("Camera B", camLink);
  CHECK(dup["error"]["code"] == "invalid");
  CHECK(dup["error"]["message"].get<std::string>().find("Camera A") != std::string::npos);
  CHECK(r.show().cameras.size() == 1);
  auto b = addCam("Camera B", {{"kind", "uvc"}, {"params", {{"device", "FaceTime HD"}}}})["result"]["id"];
  REQUIRE(r.show().cameras.size() == 2);
  CHECK(addCam("Camera C", kSrt)["ok"] == true);
  auto move = call(r, {{"cmd", "updateCamera"}, {"args", {{"id", b}, {"input", camLink}}}});
  CHECK(move["error"]["code"] == "invalid");
  CHECK(move["error"]["message"].get<std::string>().find("Camera A") != std::string::npos);
  CHECK(call(r, {{"cmd", "updateCamera"}, {"args", {{"id", b}, {"input", {{"kind", "uvc"}, {"params", {{"device", "FaceTime HD"}}}}}}}})["ok"] == true);
  CHECK(call(r, {{"cmd", "updateCamera"}, {"args", {{"id", b}, {"name", "Renamed"}}}})["ok"] == true);
}

TEST_CASE("CommandRouter: a hand-edited show can rename a camera that shares a device") {
  bc::CameraConfig a;
  a.id = "a";
  a.name = "Camera A";
  a.input.kind = "uvc";
  a.input.params = {{"device", "Cam Link 4K"}};
  bc::CameraConfig b;
  b.id = "b";
  b.name = "Camera B";
  b.input.kind = "uvc";
  b.input.params = {{"device", "Cam Link 4K"}};
  bc::Show show;
  show.cameras.push_back(a);
  show.cameras.push_back(b);
  FakeFx fx;
  bc::CommandRouter r(std::move(show), fx);
  // The hand-edited show already holds two cameras on one device; a rename must still work.
  CHECK(call(r, {{"cmd", "updateCamera"}, {"args", {{"id", "b"}, {"name", "Renamed"}}}})["ok"] == true);
  CHECK(r.show().cameras[1].name == "Renamed");
  // Renaming b and moving it onto the other camera's device is still refused.
  auto move = call(r, {{"cmd", "updateCamera"}, {"args", {{"id", "b"}, {"name", "Renamed again"}, {"input", {{"kind", "uvc"}, {"params", {{"device", "Cam Link 4K"}}}}}}}});
  CHECK(move["error"]["code"] == "invalid");
}
