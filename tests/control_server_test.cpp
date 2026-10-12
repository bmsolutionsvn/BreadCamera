#include <doctest/doctest.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <thread>

#include "engine/command_router.h"
#include "engine/control_server.h"

using namespace std::chrono_literals;

TEST_CASE("tokenFromUri and tokenMatches") {
  CHECK(bc::tokenFromUri("/?token=abc123") == "abc123");
  CHECK(bc::tokenFromUri("/?x=1&token=abc&y=2") == "abc");
  CHECK_FALSE(bc::tokenFromUri("/"));
  CHECK(bc::tokenMatches("abc", "abc"));
  CHECK_FALSE(bc::tokenMatches("abc", "abd"));
  CHECK_FALSE(bc::tokenMatches("abc", "ab"));
  CHECK_FALSE(bc::tokenMatches("abc", ""));
  CHECK(bc::randomToken().size() == 32);
  CHECK(bc::randomToken() != bc::randomToken());
}

TEST_CASE("ControlServer: refuses a wrong token and answers a right one") {
  ix::initNetSystem();
  bc::ControlServer server(47820, "goodtoken");
  std::atomic<int> opens{0};
  server.setHandlers({[&](const bc::ClientId&) { ++opens; }, [](const bc::ClientId&) {},
                      [](const bc::ClientId&, const std::string& text) { return "echo:" + text; }});
  server.start();

  ix::WebSocket bad;
  bad.setUrl("ws://127.0.0.1:47820/?token=wrong");
  bad.disableAutomaticReconnection();
  std::atomic<int> closeCode{0};
  bad.setOnMessageCallback([&](const ix::WebSocketMessagePtr& m) {
    if (m->type == ix::WebSocketMessageType::Close) closeCode = m->closeInfo.code;
  });
  bad.start();
  for (int i = 0; i < 100 && closeCode == 0; ++i) std::this_thread::sleep_for(20ms);
  CHECK(closeCode == 4401);
  bad.stop();
  CHECK(opens == 0);

  ix::WebSocket good;
  good.setUrl("ws://127.0.0.1:47820/?token=goodtoken");
  good.disableAutomaticReconnection();
  std::atomic<bool> open{false};
  std::string reply;
  std::mutex m;
  good.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) open = true;
    if (msg->type == ix::WebSocketMessageType::Message) {
      std::lock_guard l(m);
      reply = msg->str;
    }
  });
  good.start();
  for (int i = 0; i < 100 && !open; ++i) std::this_thread::sleep_for(20ms);
  REQUIRE(open);
  good.send("hello");
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard l(m);
      if (!reply.empty()) break;
    }
    std::this_thread::sleep_for(20ms);
  }
  {
    std::lock_guard l(m);
    CHECK(reply == "echo:hello");
  }
  CHECK(opens == 1);
  good.stop();
  server.stop();
}

// Review focus: hostile control messages must get an error response on the wire
// itself - never a crash, never a connection that stops answering.
TEST_CASE("ControlServer: hostile messages over the socket get error replies, never a hang") {
  ix::initNetSystem();
  struct Fx : bc::CommandRouter::Effects {
    void showChanged(const bc::Show&) override {}
    void subscribePreview(const bc::ClientId&, std::vector<std::string>, bc::PreviewSize) override {}
    void requestKeyframe(const std::string&, bc::PreviewSize) override {}
    nlohmann::json listMonitors() override { return nlohmann::json::array(); }
    nlohmann::json findChart(const std::string&) override { return nlohmann::json::object(); }
    nlohmann::json calibrateColor(const std::string&, const std::string&, const bc::ChartQuad&) override { return nlohmann::json::object(); }
    void forgetColor(const std::string&) override {}
    void shutdown() override {}
  } fx;
  bc::CommandRouter router({}, fx);
  bc::ControlServer server(47821, "wiretoken");
  server.setHandlers({[](const bc::ClientId&) {}, [](const bc::ClientId&) {},
                      [&](const bc::ClientId& id, const std::string& text) { return router.handle(id, text).dump(); }});
  server.start();

  ix::WebSocket ws;
  ws.setUrl("ws://127.0.0.1:47821/?token=wiretoken");
  ws.disableAutomaticReconnection();
  std::mutex m;
  std::vector<std::string> replies;
  std::atomic<bool> open{false};
  ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) open = true;
    if (msg->type == ix::WebSocketMessageType::Message) {
      std::lock_guard l(m);
      replies.push_back(msg->str);
    }
  });
  ws.start();
  for (int i = 0; i < 150 && !open; ++i) std::this_thread::sleep_for(20ms);
  REQUIRE(open);

  auto ask = [&](const std::string& payload, const char* wantCode) {
    {
      std::lock_guard l(m);
      replies.clear();
    }
    ws.send(payload);
    for (int i = 0; i < 150; ++i) {
      {
        std::lock_guard l(m);
        if (!replies.empty()) break;
      }
      std::this_thread::sleep_for(20ms);
    }
    std::string got;
    {
      std::lock_guard l(m);
      REQUIRE_FALSE(replies.empty());  // a reply always comes back: no hung connection
      got = replies.front();
    }
    auto j = nlohmann::json::parse(got, nullptr, false);
    REQUIRE(j.is_object());
    if (wantCode) CHECK(j["error"]["code"] == wantCode);
    else CHECK(j["ok"] == true);
  };

  ask("not json", "bad-json");
  ask(std::string(100'000, ' '), "too-large");
  ask(R"({"id":9,"cmd":"fly"})", "unknown-command");
  ask(R"({"id":10,"cmd":"ping"})", nullptr);  // still alive after the garbage
  ws.stop();
  server.stop();
}
