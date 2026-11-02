// Cross-process DirectComposition surface probe (native-video Task 1, the plan's gate).
//
// Default mode re-launches itself with "serve". The child creates a composition surface handle,
// binds a 256x256 magenta flip-sequential swap chain to it and prints "<pid> <handle>". This
// process duplicates that handle into itself, binds it to its own composition device and a
// visible window, then reads the centre pixel off the screen. PASS = a surface handle survives
// the process boundary and presents.
//
// Exit: 0 PASS, 1 FAIL (<step> + HRESULT, or the pixel), 2 UNJUDGED (no interactive desktop).
#include <windows.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

#include "gpu/device.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kSize = 256;
constexpr int kExitFail = 1;
constexpr int kExitUnjudged = 2;

[[noreturn]] void fail(const std::string& step, HRESULT hr) {
  std::printf("FAIL: %s (hr=0x%08lX)\n", step.c_str(), static_cast<unsigned long>(hr));
  std::fflush(stdout);
  std::exit(kExitFail);
}

// ---- child: own the swap chain, publish the surface handle ----
int serve() {
  std::unique_ptr<bc::GpuDevice> gpu;
  try {
    gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  } catch (const std::exception& e) {
    std::printf("FAIL: GpuDevice::create: %s\n", e.what());
    std::fflush(stdout);
    return kExitFail;
  }

  HANDLE surface = nullptr;
  HRESULT hr = DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &surface);
  if (FAILED(hr)) fail("DCompositionCreateSurfaceHandle", hr);

  ComPtr<IDXGIFactoryMedia> factory;
  hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) fail("CreateDXGIFactory1(IDXGIFactoryMedia)", hr);

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = kSize;
  desc.Height = kSize;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

  ComPtr<IDXGISwapChain1> swap;
  hr = factory->CreateSwapChainForCompositionSurfaceHandle(gpu->device(), surface, &desc, nullptr, &swap);
  if (FAILED(hr)) fail("CreateSwapChainForCompositionSurfaceHandle", hr);

  ComPtr<ID3D11Texture2D> buffer;
  hr = swap->GetBuffer(0, IID_PPV_ARGS(&buffer));
  if (FAILED(hr)) fail("IDXGISwapChain1::GetBuffer", hr);

  ComPtr<ID3D11RenderTargetView> rtv;
  hr = gpu->device()->CreateRenderTargetView(buffer.Get(), nullptr, &rtv);
  if (FAILED(hr)) fail("CreateRenderTargetView", hr);

  std::printf("%lu %llu\n", static_cast<unsigned long>(GetCurrentProcessId()),
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(surface)));
  std::fflush(stdout);

  const float magenta[4] = {1.f, 0.f, 1.f, 1.f};
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  for (;;) {
    gpu->context()->ClearRenderTargetView(rtv.Get(), magenta);
    hr = swap->Present(0, 0);
    if (FAILED(hr)) fail("IDXGISwapChain1::Present", hr);
    Sleep(16);
    // The parent holds the write end open and closes it to say "stop". If stdin is a console
    // (someone ran `serve` by hand) PeekNamedPipe fails with another error and we keep going.
    DWORD available = 0;
    if (!PeekNamedPipe(in, nullptr, 0, nullptr, &available, nullptr) &&
        GetLastError() == ERROR_BROKEN_PIPE) {
      break;
    }
  }
  CloseHandle(surface);
  return 0;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Splits "<pid> <handle>"; returns false when the child did not print one.
bool parseHandleLine(const std::string& line, DWORD& pid, uintptr_t& handle) {
  const size_t space = line.find(' ');
  if (space == std::string::npos) return false;
  try {
    pid = static_cast<DWORD>(std::stoul(line.substr(0, space)));
    handle = static_cast<uintptr_t>(std::stoull(line.substr(space + 1)));
  } catch (const std::exception&) {
    return false;
  }
  return pid != 0 && handle != 0;
}

// ---- parent: adopt the handle, compose it, read the pixel ----
int run() {
  // Self-launch with inherited pipes.
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE childIn = nullptr;
  HANDLE childOut = nullptr;
  HANDLE parentIn = nullptr;
  HANDLE parentOut = nullptr;
  if (!CreatePipe(&childIn, &parentOut, &inherit, 0)) fail("CreatePipe(stdout)", HRESULT_FROM_WIN32(GetLastError()));
  if (!CreatePipe(&parentIn, &childOut, &inherit, 0)) fail("CreatePipe(stdin)", HRESULT_FROM_WIN32(GetLastError()));
  SetHandleInformation(parentOut, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(parentIn, HANDLE_FLAG_INHERIT, 0);

  wchar_t exe[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) fail("GetModuleFileNameW", HRESULT_FROM_WIN32(GetLastError()));
  std::wstring cmd = std::wstring(L"\"") + exe + L"\" serve";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = childIn;
  si.hStdOutput = childOut;
  // stderr is not the protocol: bc_gpu warns there (BC_GPU_ADAPTER out of range), and with
  // STARTF_USESTDHANDLES the handle must be inheritable, so hand the child a copy of ours.
  HANDLE childErr = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(), &childErr, 0, TRUE,
                       DUPLICATE_SAME_ACCESS)) {
    childErr = nullptr;
  }
  si.hStdError = childErr;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
    fail("CreateProcessW", HRESULT_FROM_WIN32(GetLastError()));
  }
  CloseHandle(childIn);
  CloseHandle(childOut);
  if (childErr) CloseHandle(childErr);

  auto closeChild = [&] {
    CloseHandle(parentOut);  // child stdin EOF -> it stops presenting and exits
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  };

  std::string line;
  auto reader = std::async(std::launch::async, [&] {
    char c = 0;
    DWORD read = 0;
    while (ReadFile(parentIn, &c, 1, &read, nullptr) && read == 1) {
      if (c == '\n') break;
      line += c;
    }
    return line;
  });
  if (reader.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
    std::printf("FAIL: child did not report a surface handle within 10 s\n");
    std::fflush(stdout);
    TerminateProcess(pi.hProcess, 1);
    closeChild();
    return kExitFail;
  }
  line = reader.get();
  CloseHandle(parentIn);

  DWORD pid = 0;
  uintptr_t handle = 0;
  if (!parseHandleLine(line, pid, handle)) {
    std::printf("FAIL: child reported \"%s\"\n", line.c_str());
    std::fflush(stdout);
    closeChild();
    return kExitFail;
  }

  // The child's handle is valid in the child; copy it into this process.
  HANDLE childProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
  if (!childProc) fail("OpenProcess(PROCESS_DUP_HANDLE)", HRESULT_FROM_WIN32(GetLastError()));
  HANDLE adopted = nullptr;
  if (!DuplicateHandle(childProc, reinterpret_cast<HANDLE>(handle), GetCurrentProcess(), &adopted, 0, FALSE,
                       DUPLICATE_SAME_ACCESS)) {
    fail("DuplicateHandle", HRESULT_FROM_WIN32(GetLastError()));
  }
  CloseHandle(childProc);

  WNDCLASSW wc{};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"bc_native_video_probe";
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  if (!RegisterClassW(&wc)) fail("RegisterClassW", HRESULT_FROM_WIN32(GetLastError()));
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"bc native video probe", WS_POPUP, 100, 100,
                              kSize, kSize, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) fail("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
  SetWindowPos(hwnd, HWND_TOPMOST, 100, 100, kSize, kSize, SWP_SHOWWINDOW);
  SetForegroundWindow(hwnd);

  ComPtr<IDCompositionDesktopDevice> dcomp;
  HRESULT hr = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&dcomp));
  if (FAILED(hr)) fail("DCompositionCreateDevice2", hr);

  ComPtr<IDCompositionTarget> target;
  hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, &target);
  if (FAILED(hr)) fail("CreateTargetForHwnd", hr);

  ComPtr<IUnknown> surface;
  hr = dcomp->CreateSurfaceFromHandle(adopted, &surface);
  if (FAILED(hr)) fail("CreateSurfaceFromHandle", hr);

  ComPtr<IDCompositionVisual2> visual;
  hr = dcomp->CreateVisual(&visual);
  if (FAILED(hr)) fail("CreateVisual", hr);
  hr = visual->SetContent(surface.Get());
  if (FAILED(hr)) fail("IDCompositionVisual::SetContent", hr);
  hr = target->SetRoot(visual.Get());
  if (FAILED(hr)) fail("IDCompositionTarget::SetRoot", hr);
  hr = dcomp->Commit();
  if (FAILED(hr)) fail("IDCompositionDevice::Commit", hr);

  for (auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
       std::chrono::steady_clock::now() < deadline;) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(16);
  }

  POINT origin{0, 0};
  ClientToScreen(hwnd, &origin);
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, kSize, kSize);
  HGDIOBJ old = SelectObject(mem, bitmap);
  BitBlt(mem, 0, 0, kSize, kSize, screen, origin.x, origin.y, SRCCOPY);
  const COLORREF pixel = GetPixel(mem, kSize / 2, kSize / 2);
  SelectObject(mem, old);
  DeleteObject(bitmap);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);

  closeChild();  // stop the child's presents before judging

  const int r = GetRValue(pixel);
  const int g = GetGValue(pixel);
  const int b = GetBValue(pixel);
  if (r > 200 && g < 60 && b > 200) {
    std::printf("PASS\n");
    std::fflush(stdout);
    return 0;
  }
  if (r < 10 && g < 10 && b < 10) {
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
    if (!desk) {
      std::printf("UNJUDGED: no interactive desktop\n");
      std::fflush(stdout);
      return kExitUnjudged;
    }
    CloseDesktop(desk);
  }
  std::printf("FAIL: pixel R=%d G=%d B=%d\n", r, g, b);
  std::fflush(stdout);
  return kExitFail;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "serve") return serve();
  return run();
}