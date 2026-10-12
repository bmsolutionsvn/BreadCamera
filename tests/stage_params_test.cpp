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
  REQUIRE(bc::stageSchemas().size() == 3);
  CHECK(bc::stageSchemas()[0].kind == "color");  // pipeline order: colour runs first
  CHECK(bc::stageSchemas()[1].kind == "matte");
  for (auto& s : bc::stageSchemas()) CHECK(s.fields.front().key == "enabled");
}

TEST_CASE("colour stage: defaults, hidden LUT fields, order") {
  auto c = bc::normalizeStageParams("color", nlohmann::json::object());
  CHECK(c["enabled"] == false);
  CHECK(c["gamma"] == 1);
  CHECK(c["lut"] == "");
  CHECK(bc::stageSchemas().front().kind == "color");
  auto j = bc::schemaToJson(*bc::findStageSchema("color"));
  bool lutHidden = false;
  for (auto& f : j["fields"]) if (f["key"] == "lut") lutHidden = f.value("hidden", false);
  CHECK(lutHidden);
  CHECK(j["icon"] == "color");
  CHECK(j["onAdd"]["enabled"] == true);
  for (auto& f : j["fields"]) if (f["key"] != "enabled") CHECK(f.value("hidden", false));  // the UI shows only the switch
  CHECK_THROWS_AS(bc::normalizeStageParams("color", {{"exposure", 4}}), std::invalid_argument);
}
