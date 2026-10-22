#pragma once
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/chart.h"
#include "core/show.h"
#include "engine/preview_hub.h"

namespace bc {
using ClientId = std::string;

// Throw std::invalid_argument with a user-readable message (spec §6.1, trust boundary).
void validateInput(const InputConfig& in);
void validateOutput(const OutputConfig& out);

// Applies control commands to the show. Thread-safe: each WebSocket connection calls
// handle() from its own thread.
class CommandRouter {
 public:
  class Effects {
   public:
    virtual ~Effects() = default;
    virtual void showChanged(const Show& show) = 0;
    virtual void subscribePreview(const ClientId& client, std::vector<std::string> cameraIds, PreviewSize size) = 0;
    virtual void requestKeyframe(const std::string& cameraId, PreviewSize size) = 0;
    virtual nlohmann::json listMonitors() = 0;
    virtual nlohmann::json listNdiSources() = 0;
    // OMT source names seen on the network. Non-pure: a fake without an OMT radio returns none.
    virtual nlohmann::json listOmtSources() { return nlohmann::json::array(); }
    virtual nlohmann::json listCaptureDevices() = 0;
    virtual nlohmann::json listDeckLinkDevices() = 0;
    virtual nlohmann::json listAjaDevices() = 0;
    virtual nlohmann::json findChart(const std::string& cameraId) = 0;
    virtual nlohmann::json calibrateColor(const std::string& cameraId, const std::string& chart, const ChartQuad& q) = 0;
    virtual void forgetColor(const std::string& cameraId) = 0;
    // Why an effect cannot be added on this PC (empty = it can). Not pure: a test fake or an
    // engine that never had the seam keeps compiling, and the API path stays the same.
    virtual std::string stageUnavailable(const std::string& kind) {
      (void)kind;
      return "";
    }
    virtual void shutdown() = 0;
  };

  CommandRouter(Show initial, Effects& fx) : show_(std::move(initial)), fx_(fx) {}
  nlohmann::json handle(const ClientId& client, std::string_view text);
  Show show() const;

 private:
  nlohmann::json dispatch(const ClientId& client, const std::string& cmd, const nlohmann::json& args);
  mutable std::mutex m_;
  Show show_;
  Effects& fx_;
};
}  // namespace bc
