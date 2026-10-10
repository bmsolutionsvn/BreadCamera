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
  REQUIRE(bc::stageSchemas().size() == 2);
  CHECK(bc::stageSchemas()[0].kind == "matte");  // pipeline order (M3 puts color first)
  for (auto& s : bc::stageSchemas()) CHECK(s.fields.front().key == "enabled");
}
