#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "core/secrets.h"
#include "core/show.h"
#include "core/stage_params.h"
#include "core/utf8.h"

namespace {
bc::Show sampleShow() {
  bc::Show s;
  bc::CameraConfig cam;
  cam.id = "cam1";
  cam.name = "Cam 1";
  cam.input.kind = "srt";
  cam.input.params = {{"mode", "listener"}, {"port", 9000}, {"passphrase", "supersecret123"}};
  cam.outputs.push_back({"out1", "ndi", false, true, {{"name", "BreadCamera - Cam 1"}}});
  s.cameras.push_back(cam);
  return s;
}
}  // namespace

TEST_CASE("Secrets: DPAPI round-trip, and garbage is rejected") {
  auto enc = bc::protectSecret("hello world");
  CHECK(enc.find("hello") == std::string::npos);
  CHECK(bc::unprotectSecret(enc) == "hello world");
  CHECK_THROWS(bc::unprotectSecret("not-base64!!"));
}

TEST_CASE("Show: JSON round-trip encrypts secrets on disk") {
  auto s = sampleShow();
  auto j = bc::showToJson(s, bc::SecretMode::Encrypt);
  auto text = j.dump();
  CHECK(text.find("supersecret123") == std::string::npos);
  CHECK(j["cameras"][0]["input"]["params"]["passphrase"].contains("$dpapi"));
  CHECK(bc::showFromJson(j) == s);
}

TEST_CASE("Show: redacted JSON never carries the secret") {
  auto j = bc::showToJson(sampleShow(), bc::SecretMode::Redact);
  CHECK(j["cameras"][0]["input"]["params"]["passphrase"] == nlohmann::json{{"set", true}});
  bc::Show empty = sampleShow();
  empty.cameras[0].input.params["passphrase"] = "";
  auto j2 = bc::showToJson(empty, bc::SecretMode::Redact);
  CHECK(j2["cameras"][0]["input"]["params"]["passphrase"] == nlohmann::json{{"set", false}});
}

TEST_CASE("Show: validation rejects bad files") {
  auto good = bc::showToJson(sampleShow(), bc::SecretMode::Plain);
  auto bad = good;
  bad["schemaVersion"] = 2;
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  bad = good;
  bad["settings"]["rate"] = "30/1";
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  bad = good;
  bad["settings"]["holdMs"] = 70000;
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  bad = good;
  bad["cameras"].push_back(bad["cameras"][0]);  // duplicate id
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  bad = good;
  bad["cameras"][0]["input"]["params"]["passphrase"] = {{"set", true}};
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  CHECK_THROWS_AS(bc::showFromJson(nlohmann::json::array()), bc::ShowError);
}

TEST_CASE("Show: outputFromJson reports ShowError, not nlohmann exceptions") {
  nlohmann::json good = {{"id", "out1"}, {"kind", "ndi"}};
  auto o = bc::outputFromJson(good);
  CHECK(o.id == "out1");
  CHECK(o.kind == "ndi");
  CHECK(o.keyed == false);
  CHECK(o.enabled == true);
  CHECK(o.params == nlohmann::json::object());

  nlohmann::json badKeyed = good;
  badKeyed["keyed"] = "no";
  CHECK_THROWS_AS(bc::outputFromJson(badKeyed), bc::ShowError);

  nlohmann::json badEnabled = good;
  badEnabled["enabled"] = "yes";
  CHECK_THROWS_AS(bc::outputFromJson(badEnabled), bc::ShowError);
}

TEST_CASE("Show: newId gives 8 lowercase hex chars and differs") {
  auto a = bc::newId(), b = bc::newId();
  CHECK(a.size() == 8);
  CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(a != b);
}

TEST_CASE("Show: resolution and output format round-trip, and old files load with defaults") {
  auto s = sampleShow();
  s.settings.width = 1280;
  s.settings.height = 720;
  s.cameras[0].outputs[0].width = 960;
  s.cameras[0].outputs[0].height = 540;
  s.cameras[0].outputs[0].halfRate = true;
  auto j = bc::showToJson(s, bc::SecretMode::Plain);
  CHECK(j["settings"]["resolution"] == "1280x720");
  CHECK(j["cameras"][0]["outputs"][0]["format"] == nlohmann::json{{"width", 960}, {"height", 540}, {"halfRate", true}});
  CHECK(bc::showFromJson(j) == s);

  j["settings"].erase("resolution");
  j["cameras"][0]["outputs"][0].erase("format");
  auto old = bc::showFromJson(j);
  CHECK(old.settings.width == 1920);
  CHECK(old.settings.height == 1080);
  CHECK(old.cameras[0].outputs[0].width == 0);
  CHECK_FALSE(old.cameras[0].outputs[0].halfRate);
}

TEST_CASE("Show: unsupported sizes are rejected") {
  auto j = bc::showToJson(sampleShow(), bc::SecretMode::Plain);
  j["settings"]["resolution"] = "3840x2160";
  CHECK_THROWS_AS(bc::showFromJson(j), bc::ShowError);
  j["settings"]["resolution"] = "1920x1080";
  j["cameras"][0]["outputs"][0]["format"] = {{"width", 800}, {"height", 600}, {"halfRate", false}};
  CHECK_THROWS_AS(bc::showFromJson(j), bc::ShowError);
}

TEST_CASE("Show: Unicode names survive the file unchanged") {
  auto s = sampleShow();
  s.cameras[0].name = "Sân khấu trái – Cam 2 📷";
  auto back = bc::showFromJson(nlohmann::json::parse(bc::showToJson(s, bc::SecretMode::Plain).dump()));
  CHECK(back.cameras[0].name == "Sân khấu trái – Cam 2 📷");
  CHECK(bc::toUtf8(bc::toWide(back.cameras[0].name)) == back.cameras[0].name);
  CHECK(bc::utf8Length("Sân khấu") == 8);
}

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
