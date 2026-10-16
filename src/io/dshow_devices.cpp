#include "io/dshow_devices.h"

#include <windows.h>

#include <initguid.h>  // defines the DirectShow GUIDs in this one TU
#include <dshow.h>

#include <algorithm>
#include <cctype>

#include "core/utf8.h"

namespace bc {
namespace {
struct ComInit {
  ComInit() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComInit() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  HRESULT hr_;
};

bool ieq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

std::string bagString(IPropertyBag* bag, const wchar_t* key) {
  VARIANT v;
  VariantInit(&v);
  std::string out;
  if (SUCCEEDED(bag->Read(key, &v, nullptr)) && v.vt == VT_BSTR && v.bstrVal) out = toUtf8(v.bstrVal);
  VariantClear(&v);
  return out;
}
}  // namespace

std::vector<DShowDevice> listDShowDevices() {
  ComInit com;
  std::vector<DShowDevice> out;
  ICreateDevEnum* devEnum = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum)))) return out;
  IEnumMoniker* monikers = nullptr;
  if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) != S_OK || !monikers) {
    devEnum->Release();
    return out;  // S_FALSE means an empty category
  }
  IMoniker* mon = nullptr;
  while (monikers->Next(1, &mon, nullptr) == S_OK) {
    DShowDevice d;
    LPOLESTR w = nullptr;
    if (SUCCEEDED(mon->GetDisplayName(nullptr, nullptr, &w)) && w) {
      d.displayName = toUtf8(w);
      CoTaskMemFree(w);
    }
    IPropertyBag* bag = nullptr;
    if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
      d.name = bagString(bag, L"FriendlyName");
      d.devicePath = bagString(bag, L"DevicePath");
      bag->Release();
    }
    mon->Release();
    if (!d.name.empty()) out.push_back(std::move(d));
  }
  monikers->Release();
  devEnum->Release();
  return out;
}

std::vector<CaptureDevice> mergeCaptureDevices(const std::vector<CaptureDevice>& mf, const std::vector<DShowDevice>& dshow) {
  std::vector<CaptureDevice> out = mf;
  for (const auto& d : dshow) {
    // DirectShow wraps physical cameras too, so a device both stacks see must not double.
    const bool dupPath = !d.devicePath.empty() &&
                         std::any_of(mf.begin(), mf.end(), [&](const CaptureDevice& m) { return ieq(m.id, d.devicePath); });
    const bool dupName = std::any_of(mf.begin(), mf.end(), [&](const CaptureDevice& m) { return ieq(m.name, d.name); });
    const std::string id = "dshow:" + d.displayName;
    const bool already = std::any_of(out.begin(), out.end(), [&](const CaptureDevice& m) { return m.id == id; });
    if (dupPath || dupName || already) continue;
    out.push_back({d.name, id});
  }
  return out;
}
}  // namespace bc
