#include <doctest/doctest.h>

#include "core/secrets.h"
#include "core/show.h"

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
