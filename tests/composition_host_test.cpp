#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "shell/composition_host.h"

using nlohmann::json;

TEST_CASE("parseVideoLayers: accepts a well-formed message for this engine") {
  auto m = json::parse(
      R"({"type":"videoLayers","pid":42,"layers":[{"id":"cam:A","handle":"1234","x":10,"y":20,"w":320,"h":180}]})");
  auto r = bc::parseVideoLayers(m, 42);
  REQUIRE(r);
  REQUIRE(r->size() == 1);
  CHECK((*r)[0].id == "cam:A");
  CHECK((*r)[0].handle == 1234);
  CHECK((*r)[0].x == 10);
  CHECK((*r)[0].y == 20);
  CHECK((*r)[0].w == 320);
  CHECK((*r)[0].h == 180);
}

TEST_CASE("parseVideoLayers: another engine's pid is ignored") {
  CHECK_FALSE(bc::parseVideoLayers(json::parse(R"({"type":"videoLayers","pid":7,"layers":[]})"), 42));
}

TEST_CASE("parseVideoLayers: bad layers are dropped, the rest kept") {
  auto m = json::parse(R"({"type":"videoLayers","pid":42,"layers":[
    {"id":"cam:A","handle":"1234","x":10,"y":20,"w":320,"h":180},
    {"id":"cam:B","handle":"abc","x":0,"y":0,"w":320,"h":180},
    {"id":"cam:C","handle":"99","x":0,"y":0,"w":0,"h":180},
    {"id":"cam:D","handle":"99","x":0,"y":0,"w":320,"h":99999},
    {"handle":"99","x":0,"y":0,"w":320,"h":180},
    {"id":7,"handle":"99","x":0,"y":0,"w":320,"h":180},
    {"id":"cam:E","handle":99,"x":0,"y":0,"w":320,"h":180},
    {"id":"cam:F","handle":"99","x":1000000000,"y":0,"w":320,"h":180}
  ]})");
  auto r = bc::parseVideoLayers(m, 42);
  REQUIRE(r);
  REQUIRE(r->size() == 1);
  CHECK((*r)[0].id == "cam:A");
}

TEST_CASE("parseVideoLayers: at most 64 layers") {
  json layers = json::array();
  for (int i = 0; i < 1000; ++i)
    layers.push_back({{"id", "cam:" + std::to_string(i)}, {"handle", std::to_string(i + 1)}, {"x", 0}, {"y", 0},
                      {"w", 320}, {"h", 180}});
  auto m = json{{"type", "videoLayers"}, {"pid", 42}, {"layers", layers}};
  auto r = bc::parseVideoLayers(m, 42);
  REQUIRE(r);
  CHECK(r->size() == 64);
}

TEST_CASE("parseVideoLayers: malformed roots") {
  CHECK_FALSE(bc::parseVideoLayers(json::parse("[]"), 42));
  CHECK_FALSE(bc::parseVideoLayers(json::parse(R"({"type":"videoLayers","layers":[]})"), 42));
  CHECK_FALSE(bc::parseVideoLayers(json::parse(R"({"type":"videoLayers","pid":42,"layers":{}})"), 42));
  CHECK_FALSE(bc::parseVideoLayers(json::parse(R"({"type":"videoLayers","pid":"42","layers":[]})"), 42));
}