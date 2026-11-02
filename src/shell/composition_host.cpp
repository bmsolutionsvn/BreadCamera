#include "shell/composition_host.h"

#include <uiautomationcore.h>
#include <uiautomationcoreapi.h>
#include <windowsx.h>

#include <atomic>
#include <charconv>
#include <set>
#include <utility>

#include <wrl.h>

using Microsoft::WRL::Callback;

namespace bc {
namespace {

bool intField(const nlohmann::json& o, const char* key, int lo, int hi, int& out) {
  auto it = o.find(key);
  if (it == o.end() || !(it->is_number_integer() || it->is_number_unsigned())) return false;
  const double d = it->get<double>();
  if (d < lo || d > hi) return false;
  out = static_cast<int>(d);
  return true;
}

bool decimalHandle(const nlohmann::json& o, uint64_t& out) {
  auto it = o.find("handle");
  if (it == o.end() || !it->is_string()) return false;
  const std::string& s = it->get_ref<const std::string&>();
  if (s.empty() || s[0] < '0' || s[0] > '9') return false;
  uint64_t v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size()) return false;
  out = v;
  return true;
}

COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS virtualKeys() {
  auto keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
  if (GetKeyState(VK_LBUTTON) < 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
  if (GetKeyState(VK_RBUTTON) < 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
  if (GetKeyState(VK_MBUTTON) < 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
  if (GetKeyState(VK_SHIFT) < 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_SHIFT;
  if (GetKeyState(VK_CONTROL) < 0) keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_CONTROL;
  return keys;
}

bool anyButtonDown() {
  return GetKeyState(VK_LBUTTON) < 0 || GetKeyState(VK_RBUTTON) < 0 || GetKeyState(VK_MBUTTON) < 0 ||
         GetKeyState(VK_XBUTTON1) < 0 || GetKeyState(VK_XBUTTON2) < 0;
}

bool isButtonDown(UINT msg) {
  switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
      return true;
    default:
      return false;
  }
}

bool isButtonUp(UINT msg) {
  switch (msg) {
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
      return true;
    default:
      return false;
  }
}

bool isMouseMessage(UINT msg) {
  switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      return true;
    default:
      return false;
  }
}

}  // namespace

std::optional<std::vector<VideoLayer>> parseVideoLayers(const nlohmann::json& msg, DWORD enginePid) {
  if (!msg.is_object()) return std::nullopt;
  const auto pid = msg.find("pid");
  if (pid == msg.end() || !(pid->is_number_integer() || pid->is_number_unsigned())) return std::nullopt;
  uint64_t sender = 0;
  if (pid->is_number_unsigned()) {
    sender = pid->get<uint64_t>();
  } else {
    const int64_t signedPid = pid->get<int64_t>();
    if (signedPid < 0) return std::nullopt;
    sender = static_cast<uint64_t>(signedPid);
  }
  if (sender != enginePid) return std::nullopt;

  const auto layers = msg.find("layers");
  if (layers == msg.end() || !layers->is_array()) return std::nullopt;

  std::vector<VideoLayer> out;
  for (const auto& l : *layers) {
    if (out.size() >= 64) break;
    if (!l.is_object()) continue;
    const auto id = l.find("id");
    if (id == l.end() || !id->is_string()) continue;
    VideoLayer layer;
    layer.id = id->get<std::string>();
    if (!decimalHandle(l, layer.handle)) continue;
    if (!intField(l, "x", -32768, 32768, layer.x)) continue;
    if (!intField(l, "y", -32768, 32768, layer.y)) continue;
    if (!intField(l, "w", 1, 16384, layer.w)) continue;
    if (!intField(l, "h", 1, 16384, layer.h)) continue;
    out.push_back(std::move(layer));
  }
  return out;
}

void CompositionHost::create(HWND hwnd, ICoreWebView2Environment* env,
                             std::function<void(std::unique_ptr<CompositionHost>, ICoreWebView2Controller*)> done) {
  wil::com_ptr<ICoreWebView2Environment3> env3;
  if (!env || FAILED(env->QueryInterface(IID_PPV_ARGS(&env3)))) {
    done(nullptr, nullptr);
    return;
  }
  wil::com_ptr<IDCompositionDesktopDevice> dcomp;
  wil::com_ptr<IDCompositionTarget> target;
  wil::com_ptr<IDCompositionVisual2> root;
  wil::com_ptr<IDCompositionVisual2> webview;
  if (FAILED(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&dcomp))) ||
      FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) || FAILED(dcomp->CreateVisual(&root)) ||
      FAILED(dcomp->CreateVisual(&webview)) || FAILED(root->AddVisual(webview.get(), TRUE, nullptr)) ||
      FAILED(target->SetRoot(root.get())) || FAILED(dcomp->Commit())) {
    done(nullptr, nullptr);
    return;
  }

  // The completion handler and the synchronous-failure check below both report the outcome; the
  // once guard makes the async handler safe to invoke and guarantees exactly one done(...) call.
  auto finished = std::make_shared<std::atomic<bool>>(false);
  auto finish = [done = std::move(done), finished](std::unique_ptr<CompositionHost> host,
                                                   ICoreWebView2Controller* controller) {
    if (!finished->exchange(true)) done(std::move(host), controller);
  };

  const HRESULT created = env3->CreateCoreWebView2CompositionController(
      hwnd,
      Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
          [hwnd, dcomp, target, root, webview, finish](HRESULT hr,
                                                              ICoreWebView2CompositionController* c) mutable -> HRESULT {
            if (FAILED(hr) || !c) {
              finish(nullptr, nullptr);
              return S_OK;
            }
            wil::com_ptr<ICoreWebView2CompositionController> composition(c);
            wil::com_ptr<ICoreWebView2Controller> controller;
            if (FAILED(c->QueryInterface(IID_PPV_ARGS(&controller)))) {
              finish(nullptr, nullptr);
              return S_OK;
            }
            if (auto c2 = controller.try_query<ICoreWebView2Controller2>()) {
              c2->put_DefaultBackgroundColor({0, 0, 0, 0});
            }
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            controller->put_Bounds(bounds);
            controller->put_IsVisible(TRUE);
            if (auto c3 = controller.try_query<ICoreWebView2Controller3>()) {
              c3->put_RasterizationScale(GetDpiForWindow(hwnd) / 96.0);
            }
            if (FAILED(c->put_RootVisualTarget(webview.get()))) {
              finish(nullptr, nullptr);
              return S_OK;
            }
            dcomp->Commit();  // publish the visual tree WebView2 just attached to
            {  // follow the page's CSS cursor while the pointer is over the WebView
              EventRegistrationToken token;
              composition->add_CursorChanged(
                  Callback<ICoreWebView2CursorChangedEventHandler>(
                      [](ICoreWebView2CompositionController* sender, IUnknown*) -> HRESULT {
                        HCURSOR cursor = nullptr;
                        if (SUCCEEDED(sender->get_Cursor(&cursor)) && cursor) SetCursor(cursor);
                        return S_OK;
                      })
                      .Get(),
                  &token);
            }
            auto host = std::unique_ptr<CompositionHost>(
                new CompositionHost(hwnd, dcomp, target, root, webview, composition, controller));
            ICoreWebView2Controller* raw = controller.get();
            finish(std::move(host), raw);
            return S_OK;
          })
          .Get());
  if (FAILED(created)) {
    // A synchronous failure may never invoke the handler; finish() is safe either way and runs
    // the caller's done(...) exactly once.
    finish(nullptr, nullptr);
  }
}

CompositionHost::CompositionHost(HWND hwnd, wil::com_ptr<IDCompositionDesktopDevice> dcomp,
                                 wil::com_ptr<IDCompositionTarget> target, wil::com_ptr<IDCompositionVisual2> root,
                                 wil::com_ptr<IDCompositionVisual2> webviewVisual,
                                 wil::com_ptr<ICoreWebView2CompositionController> composition,
                                 wil::com_ptr<ICoreWebView2Controller> controller)
    : hwnd_(hwnd),
      target_(std::move(target)),
      dcomp_(std::move(dcomp)),
      root_(std::move(root)),
      webview_(std::move(webviewVisual)),
      composition_(std::move(composition)),
      controller_(std::move(controller)) {}

CompositionHost::~CompositionHost() = default;

void CompositionHost::forwardMouse(UINT msg, WPARAM wp, LPARAM lp) {
  COREWEBVIEW2_MOUSE_EVENT_KIND kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
  UINT32 data = 0;
  POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  switch (msg) {
    case WM_MOUSEMOVE:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
      break;
    case WM_LBUTTONDOWN:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
      break;
    case WM_LBUTTONUP:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
      break;
    case WM_LBUTTONDBLCLK:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK;
      break;
    case WM_RBUTTONDOWN:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
      break;
    case WM_RBUTTONUP:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
      break;
    case WM_RBUTTONDBLCLK:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOUBLE_CLICK;
      break;
    case WM_MBUTTONDOWN:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
      break;
    case WM_MBUTTONUP:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
      break;
    case WM_MBUTTONDBLCLK:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOUBLE_CLICK;
      break;
    case WM_XBUTTONDOWN:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN;
      data = GET_XBUTTON_WPARAM(wp);
      break;
    case WM_XBUTTONUP:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP;
      data = GET_XBUTTON_WPARAM(wp);
      break;
    case WM_XBUTTONDBLCLK:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOUBLE_CLICK;
      data = GET_XBUTTON_WPARAM(wp);
      break;
    case WM_MOUSEWHEEL:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
      data = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(wp));
      ScreenToClient(hwnd_, &pt);  // wheel positions are screen coordinates
      break;
    case WM_MOUSEHWHEEL:
      kind = COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL;
      data = static_cast<UINT32>(GET_WHEEL_DELTA_WPARAM(wp));
      ScreenToClient(hwnd_, &pt);
      break;
    default:
      return;
  }
  lastPoint_ = pt;
  composition_->SendMouseInput(kind, virtualKeys(), data, pt);
}

bool CompositionHost::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
  if (!composition_ || !controller_) return false;
  if (isMouseMessage(msg)) {
    if (msg == WM_MOUSEMOVE && !trackingLeave_) {
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
      if (TrackMouseEvent(&track)) trackingLeave_ = true;
    }
    forwardMouse(msg, wp, lp);
    if (isButtonDown(msg)) {
      SetCapture(hwnd);
    } else if (isButtonUp(msg) && !anyButtonDown()) {
      ReleaseCapture();
    }
    *result = 0;
    return true;
  }
  switch (msg) {
    case WM_MOUSELEAVE:
      trackingLeave_ = false;
      composition_->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, virtualKeys(), 0, lastPoint_);
      *result = 0;
      return true;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        HCURSOR cursor = nullptr;
        if (SUCCEEDED(composition_->get_Cursor(&cursor)) && cursor) SetCursor(cursor);
        *result = TRUE;
        return true;
      }
      return false;
    case WM_NCHITTEST: {
      POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &p);
      RECT rc;
      GetClientRect(hwnd, &rc);
      if (!PtInRect(&rc, p)) return false;  // the resize borders stay DefWindowProc's
      auto kind = COREWEBVIEW2_NON_CLIENT_REGION_KIND_NOWHERE;
      if (auto c4 = composition_.try_query<ICoreWebView2CompositionController4>()) {
        c4->GetNonClientRegionAtPoint(p, &kind);
      }
      *result = kind == COREWEBVIEW2_NON_CLIENT_REGION_KIND_CAPTION ? HTCAPTION : HTCLIENT;
      return true;
    }
    case WM_SETFOCUS:
      controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      return false;
    case WM_ACTIVATE:
      if (LOWORD(wp) != WA_INACTIVE) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      return false;
    case WM_GETOBJECT:
      if (static_cast<LONG>(lp) == UiaRootObjectId) {
        auto c2 = composition_.try_query<ICoreWebView2CompositionController2>();
        wil::com_ptr<IUnknown> provider;
        if (c2 && SUCCEEDED(c2->get_AutomationProvider(&provider)) && provider) {
          *result =
              UiaReturnRawElementProvider(hwnd_, wp, lp, static_cast<IRawElementProviderSimple*>(provider.get()));
          return true;
        }
      }
      return false;
    default:
      return false;
  }
}

bool CompositionHost::makeLayerVisual(uint64_t handle, HANDLE engineProcess,
                                      wil::com_ptr<IDCompositionVisual2>& out) {
  if (!engineProcess || handle == 0) return false;
  HANDLE duplicate = nullptr;
  if (!DuplicateHandle(engineProcess, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)), GetCurrentProcess(),
                       &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    return false;
  }
  wil::com_ptr<IUnknown> surface;
  const HRESULT hr = dcomp_->CreateSurfaceFromHandle(duplicate, &surface);
  CloseHandle(duplicate);  // the surface object keeps its own reference
  if (FAILED(hr)) return false;
  wil::com_ptr<IDCompositionVisual2> visual;
  if (FAILED(dcomp_->CreateVisual(&visual)) || FAILED(visual->SetContent(surface.get())) ||
      FAILED(root_->AddVisual(visual.get(), FALSE, webview_.get()))) {
    return false;
  }
  out = std::move(visual);
  return true;
}

void CompositionHost::setLayers(const std::vector<VideoLayer>& layers, HANDLE engineProcess) {
  std::set<std::string> listed;
  for (const auto& layer : layers) {
    listed.insert(layer.id);
    auto it = layers_.find(layer.id);
    if (it == layers_.end() || it->second.handle != layer.handle) {
      wil::com_ptr<IDCompositionVisual2> visual;
      if (!makeLayerVisual(layer.handle, engineProcess, visual)) {
        if (it != layers_.end()) {
          root_->RemoveVisual(it->second.visual.get());
          layers_.erase(it);
        }
        if (onLayerFailed) onLayerFailed(layer.id);
        continue;
      }
      if (it != layers_.end()) {
        root_->RemoveVisual(it->second.visual.get());
        layers_.erase(it);
      }
      layers_[layer.id] = Layer{layer.handle, std::move(visual)};
    }
    auto& visual = layers_[layer.id].visual;
    visual->SetOffsetX(static_cast<float>(layer.x));
    visual->SetOffsetY(static_cast<float>(layer.y));
    const D2D_RECT_F clip{0.f, 0.f, static_cast<float>(layer.w), static_cast<float>(layer.h)};
    visual->SetClip(clip);
  }
  for (auto it = layers_.begin(); it != layers_.end();) {
    if (!listed.contains(it->first)) {
      root_->RemoveVisual(it->second.visual.get());
      it = layers_.erase(it);
    } else {
      ++it;
    }
  }
  dcomp_->Commit();
}

void CompositionHost::setVisible(bool visible) {
  if (controller_) controller_->put_IsVisible(visible ? TRUE : FALSE);
}

void CompositionHost::setDpi(UINT dpi) {
  if (!controller_) return;
  if (auto c3 = controller_.try_query<ICoreWebView2Controller3>()) {
    c3->put_RasterizationScale(dpi / 96.0);
  }
  RECT rc;
  GetClientRect(hwnd_, &rc);
  controller_->put_Bounds(rc);
}

}  // namespace bc