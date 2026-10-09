#include <doctest/doctest.h>

#include "io/srt_params.h"

using nlohmann::json;

TEST_CASE("srtOptionsFromParams: listener defaults") {
  auto o = bc::srtOptionsFromParams({{"mode", "listener"}, {"port", 9000}});
  CHECK(o.url == "srt://0.0.0.0:9000");
  CHECK(o.avOptions.at("mode") == "listener");
  CHECK(o.avOptions.at("latency") == "120000");
  CHECK_FALSE(o.avOptions.contains("passphrase"));
}

TEST_CASE("srtOptionsFromParams: caller with secrets in options, not in the URL") {
  auto o = bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "10.0.0.5"}, {"port", 7001},
                                     {"latencyMs", 200}, {"passphrase", "0123456789"}, {"streamId", "feed/abc"}});
  CHECK(o.url == "srt://10.0.0.5:7001");
  CHECK(o.avOptions.at("latency") == "200000");
  CHECK(o.avOptions.at("passphrase") == "0123456789");
  CHECK(o.avOptions.at("streamid") == "feed/abc");
}

TEST_CASE("srtOptionsFromParams: a full URL (BreadCall feed) is used as-is") {
  auto o = bc::srtOptionsFromParams({{"url", "srt://media.breadcall.io.vn:9000?streamid=read:room/feed"}});
  CHECK(o.url == "srt://media.breadcall.io.vn:9000?streamid=read:room/feed");
  CHECK(o.avOptions.at("mode") == "caller");
}

TEST_CASE("srtOptionsFromParams: rejects bad input with readable messages") {
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"port", 9000}}), std::invalid_argument);  // no mode
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "caller"}, {"port", 9000}}), std::invalid_argument);  // no host
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "listener"}, {"port", 70000}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "listener"}, {"port", "9000"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "listener"}, {"port", 9000}, {"latencyMs", 5}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "listener"}, {"port", 9000}, {"passphrase", "short"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "rendezvous"}, {"port", 9000}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"url", "rtmp://x"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams(json::array()), std::invalid_argument);
}
