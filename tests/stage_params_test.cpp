#include <doctest/doctest.h>

#include "core/stage_params.h"

using nlohmann::json;

TEST_CASE("normalizeStageParams: fills defaults") {
  auto m = bc::normalizeStageParams("matte", json::object());
  CHECK(m == json{{"enabled", false}, {"quality", "quality"}, {"choke", 0}, {"feather", 1}, {"threshold", 0.5}});
  auto b = bc::normalizeStageParams("background", {{"mode", "blur"}});
  CHECK(b["enabled"] == true);
  CHECK(b["mode"] == "blur");
  CHECK(b["blur"] == 12);
  CHECK(b["color"] == "#101418");
  CHECK(b["image"] == "");
}

TEST_CASE("normalizeStageParams: rejects bad input") {
  CHECK_THROWS_AS(bc::normalizeStageParams("teleport", json::object()), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"choke", 9}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"choke", "big"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"quality", "ultra"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"enabled", 1}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", {{"surprise", 1}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("background", {{"color", "red"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("background", {{"image", std::string(2000, 'x')}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::normalizeStageParams("matte", json::array()), std::invalid_argument);
}

TEST_CASE("stage schemas: pipeline order, labels, add-in metadata, enabled first") {
  auto* m = bc::findStageSchema("matte");
  REQUIRE(m);
  CHECK(m->label == "Background removal");
  CHECK(m->onAdd == json{{"enabled", true}});
  auto j = bc::schemaToJson(*bc::findStageSchema("background"));
  CHECK(j["kind"] == "background");
  CHECK(j["icon"] == "bg");
  CHECK(j["requires"] == json::array({"matte"}));
  CHECK(j["onAdd"]["mode"] == "blur");
  CHECK(j["fields"][0]["key"] == "enabled");
  auto& mode = j["fields"][1];
  CHECK(mode["key"] == "mode");
  CHECK(mode["options"].size() == mode["optionLabels"].size());
  REQUIRE(bc::stageSchemas().size() == 5);
  CHECK(bc::stageSchemas()[0].kind == "cleanup");  // pipeline order: clean up runs first
  CHECK(bc::stageSchemas()[1].kind == "upscale");
  for (auto& s : bc::stageSchemas()) CHECK(s.fields.front().key == "enabled");
}

TEST_CASE("stage schemas: clean up and upscale come first, in pipeline order") {
  std::vector<std::string> kinds;
  for (auto& s : bc::stageSchemas()) kinds.push_back(s.kind);
  CHECK(kinds == std::vector<std::string>{"cleanup", "upscale", "color", "matte", "background"});
  auto up = bc::normalizeStageParams("upscale", {{"enabled", true}});
  CHECK(up["target"] == "1080");
  CHECK_THROWS(bc::normalizeStageParams("upscale", {{"target", "2160"}}));
}

TEST_CASE("schemaToJson: unavailable appears only when set") {
  auto* s = bc::findStageSchema("cleanup");
  CHECK_FALSE(bc::schemaToJson(*s).contains("unavailable"));
  CHECK(bc::schemaToJson(*s, "Needs an NVIDIA RTX card.")["unavailable"] == "Needs an NVIDIA RTX card.");
}

TEST_CASE("colour: an old colour stage loads in Calibrate mode") {
  // A show written before Match (spec D9) has no mode/matchTo: it loads as Calibrate.
  auto p = bc::normalizeStageParams("color", {{"enabled", true}});
  CHECK(p["mode"] == "calibrate");
  CHECK(p["matchTo"] == "");
}

TEST_CASE("colour stage: defaults, hidden LUT fields, order") {
  auto c = bc::normalizeStageParams("color", nlohmann::json::object());
  CHECK(c["enabled"] == false);
  CHECK(c["gamma"] == 1);
  CHECK(c["lut"] == "");
  CHECK(bc::stageSchemas()[2].kind == "color");  // colour runs after clean up and upscale
  auto j = bc::schemaToJson(*bc::findStageSchema("color"));
  bool lutHidden = false;
  for (auto& f : j["fields"]) if (f["key"] == "lut") lutHidden = f.value("hidden", false);
  CHECK(lutHidden);
  CHECK(j["icon"] == "color");
  CHECK(j["onAdd"]["enabled"] == true);
  for (auto& f : j["fields"]) if (f["key"] != "enabled") CHECK(f.value("hidden", false));  // the UI shows only the switch
  CHECK_THROWS_AS(bc::normalizeStageParams("color", {{"exposure", 4}}), std::invalid_argument);
}
