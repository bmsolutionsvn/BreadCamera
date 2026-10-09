#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ix {
class WebSocketServer;
class WebSocket;
}  // namespace ix

namespace bc {
using ClientId = std::string;

bool tokenMatches(std::string_view expected, std::string_view given);
std::optional<std::string> tokenFromUri(std::string_view uri);
std::string randomToken();

// WebSocket control API on 127.0.0.1 only; every connection must carry ?token= (spec §6.1).
class ControlServer {
 public:
  struct Handlers {
    std::function<void(const ClientId&)> open;
    std::function<void(const ClientId&)> close;
    std::function<std::string(const ClientId&, const std::string&)> text;  // returns the reply
  };
  ControlServer(int port, std::string token);
  ~ControlServer();
  void setHandlers(Handlers h) { handlers_ = std::move(h); }
  void start();
  void stop();
  bool send(const ClientId& id, const std::string& text);
  bool sendBinary(const ClientId& id, const std::vector<uint8_t>& data);
  void broadcast(const std::string& text);

 private:
  int port_;
  std::string token_;
  Handlers handlers_;
  std::unique_ptr<ix::WebSocketServer> server_;
  std::mutex m_;
  std::map<ClientId, ix::WebSocket*> clients_;
};
}  // namespace bc
