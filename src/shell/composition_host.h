#pragma once

#include <windows.h>

#include <combaseapi.h>  // the `interface` macro WebView2's MIDL header needs

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <WebView2.h>
#include <d2d1.h>
#include <dcomp.h>
#include <nlohmann/json.hpp>
#include <wil/com.h>

namespace bc {

struct VideoLayer {
  std::string id;
  uint64_t handle = 0;
  int x = 0, y = 0, w = 0, h = 0;
};

// nullopt when the message is not for this engine (pid mismatch or malformed root). Drops bad layers
// (missing or non-numeric fields, w/h outside 1..16384, |x|,|y| > 32768) and keeps at most 64.
std::optional<std::vector<VideoLayer>> parseVideoLayers(const nlohmann::json& msg, DWORD enginePid);

// DirectComposition tree and WebView2 composition controller for one HWND (spec §5.1).
class CompositionHost {
 public:
  // Creates DComp + the composition controller; calls done(controller) or done(nullptr) on failure.
  static void create(HWND hwnd, ICoreWebView2Environment* env,
                     std::function<void(std::unique_ptr<CompositionHost>, ICoreWebView2Controller*)> done);
  ~CompositionHost();
  CompositionHost(const CompositionHost&) = delete;
  CompositionHost& operator=(const CompositionHost&) = delete;

  // WndProc hook: returns true (and sets result) when it handled the message.
  bool handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
  void setLayers(const std::vector<VideoLayer>& layers, HANDLE engineProcess);  // posts failures via onLayerFailed
  void setVisible(bool visible);
  void setDpi(UINT dpi);
  std::function<void(const std::string& id)> onLayerFailed;

 private:
  CompositionHost(HWND hwnd, wil::com_ptr<IDCompositionDesktopDevice> dcomp,
                  wil::com_ptr<IDCompositionTarget> target, wil::com_ptr<IDCompositionVisual2> root,
                  wil::com_ptr<IDCompositionVisual2> webviewVisual,
                  wil::com_ptr<ICoreWebView2CompositionController> composition,
                  wil::com_ptr<ICoreWebView2Controller> controller);

  bool makeLayerVisual(uint64_t handle, HANDLE engineProcess, wil::com_ptr<IDCompositionVisual2>& out);
  void forwardMouse(UINT msg, WPARAM wp, LPARAM lp);

  struct Layer {
    uint64_t handle = 0;
    wil::com_ptr<IDCompositionVisual2> visual;
  };

  HWND hwnd_ = nullptr;
  // Held for the host's lifetime: destroying the target detaches the window's composition layer.
  wil::com_ptr<IDCompositionTarget> target_;
  wil::com_ptr<IDCompositionDesktopDevice> dcomp_;
  wil::com_ptr<IDCompositionVisual2> root_;
  wil::com_ptr<IDCompositionVisual2> webview_;
  wil::com_ptr<ICoreWebView2CompositionController> composition_;
  wil::com_ptr<ICoreWebView2Controller> controller_;
  std::map<std::string, Layer> layers_;
  bool trackingLeave_ = false;
  POINT lastPoint_{};
};

}  // namespace bc