#include "engine/control_server.h"

#include <windows.h>
#include <bcrypt.h>

#include <ixwebsocket/IXWebSocketServer.h>

#include <stdexcept>

namespace bc {
bool tokenMatches(std::string_view expected, std::string_view given) {
  if (expected.empty() || expected.size() != given.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < expected.size(); ++i) diff |= static_cast<unsigned char>(expected[i] ^ given[i]);
  return diff == 0;
}

std::optional<std::string> tokenFromUri(std::string_view uri) {
  auto q = uri.find('?');
  if (q == std::string_view::npos) return std::nullopt;
  auto query = uri.substr(q + 1);
  while (!query.empty()) {
    auto amp = query.find('&');
    auto part = query.substr(0, amp);
    if (part.rfind("token=", 0) == 0) return std::string(part.substr(6));
    if (amp == std::string_view::npos) break;
    query = query.substr(amp + 1);
  }
  return std::nullopt;
}

std::string randomToken() {
  unsigned char bytes[16];
  if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    throw std::runtime_error("BCryptGenRandom failed");
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  for (auto b : bytes) {
    s += kHex[b >> 4];
    s += kHex[b & 15];
  }
  return s;
}

ControlServer::ControlServer(int port, std::string token) : port_(port), token_(std::move(token)) {}

ControlServer::~ControlServer() { stop(); }

void ControlServer::start() {
  server_ = std::make_unique<ix::WebSocketServer>(port_, "127.0.0.1");
  server_->disablePerMessageDeflate();
  server_->setOnClientMessageCallback(
      [this](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
        const ClientId id = state->getId();
        switch (msg->type) {
          case ix::WebSocketMessageType::Open: {
            auto token = tokenFromUri(msg->openInfo.uri);
            if (!token || !tokenMatches(token_, *token)) {
              ws.close(4401, "unauthorized");
              return;
            }
            {
              std::lock_guard l(m_);
              clients_[id] = &ws;
            }
            if (handlers_.open) handlers_.open(id);
            break;
          }
          case ix::WebSocketMessageType::Message: {
            {
              std::lock_guard l(m_);
              if (!clients_.contains(id)) return;  // never authorised
            }
            if (!msg->binary && handlers_.text) ws.send(handlers_.text(id, msg->str));
            break;
          }
          case ix::WebSocketMessageType::Close: {
            bool known;
            {
              std::lock_guard l(m_);
              known = clients_.erase(id) > 0;
            }
            if (known && handlers_.close) handlers_.close(id);
            break;
          }
          default:
            break;
        }
      });
  auto res = server_->listen();
  if (!res.first) throw std::runtime_error("cannot listen on 127.0.0.1:" + std::to_string(port_) + ": " + res.second);
  server_->start();
}

void ControlServer::stop() {
  if (!server_) return;
  server_->stop();
  server_.reset();
  std::lock_guard l(m_);
  clients_.clear();
}

bool ControlServer::send(const ClientId& id, const std::string& text) {
  std::lock_guard l(m_);
  auto it = clients_.find(id);
  if (it == clients_.end()) return false;
  return it->second->send(text).success;
}

bool ControlServer::sendBinary(const ClientId& id, const std::vector<uint8_t>& data) {
  std::lock_guard l(m_);
  auto it = clients_.find(id);
  if (it == clients_.end()) return false;
  return it->second->sendBinary(std::string(data.begin(), data.end())).success;
}

void ControlServer::broadcast(const std::string& text) {
  std::lock_guard l(m_);
  for (auto& [id, ws] : clients_) ws->send(text);
}
}  // namespace bc
