#include "io/decklink.h"

#include <windows.h>

// WIN32_LEAN_AND_MEAN (set project-wide) keeps OLE out of windows.h, so the
// MIDL-generated header's `typedef interface X X;` forward declarations and
// CoCreateInstance/SysFreeString need these included explicitly.
#include <objbase.h>
#include <oleauto.h>

#include <stdexcept>

#include "DeckLinkAPI_h.h"
#include "core/utf8.h"

namespace bc {
namespace {
struct Com {
  Com() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
  ~Com() { CoUninitialize(); }
};
}  // namespace

IDeckLink* openDeckLink(int index) {
  IDeckLinkIterator* it = nullptr;
  if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, reinterpret_cast<void**>(&it)))) {
    throw std::runtime_error("the Blackmagic Desktop Video driver is not installed");
  }
  IDeckLink* dl = nullptr;
  for (int i = 0; it->Next(&dl) == S_OK; ++i) {
    if (i == index) break;
    dl->Release();
    dl = nullptr;
  }
  it->Release();
  if (!dl) throw std::runtime_error("DeckLink device " + std::to_string(index) + " not found");
  return dl;
}

std::vector<DeckLinkInfo> listDeckLinkDevices() {
  Com com;
  std::vector<DeckLinkInfo> out;
  IDeckLinkIterator* it = nullptr;
  if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, reinterpret_cast<void**>(&it)))) return out;
  IDeckLink* dl = nullptr;
  for (int i = 0; it->Next(&dl) == S_OK; ++i) {
    DeckLinkInfo info;
    info.index = i;
    BSTR name = nullptr;
    if (dl->GetDisplayName(&name) == S_OK) {
      info.name = toUtf8(name);
      SysFreeString(name);
    }
    IDeckLinkInput* in = nullptr;
    IDeckLinkOutput* o = nullptr;
    info.hasInput = dl->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void**>(&in)) == S_OK;
    info.hasOutput = dl->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void**>(&o)) == S_OK;
    if (in) in->Release();
    if (o) o->Release();
    IDeckLinkProfileAttributes* attrs = nullptr;
    if (dl->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&attrs)) == S_OK) {
      BOOL ext = FALSE;
      attrs->GetFlag(BMDDeckLinkSupportsExternalKeying, &ext);
      info.externalKeying = ext != FALSE;
      attrs->Release();
    }
    out.push_back(info);
    dl->Release();
  }
  it->Release();
  return out;
}

uint32_t deckLinkModeFor(Rate r) {
  if (r == Rate{50, 1}) return bmdModeHD1080p50;
  if (r == Rate{60000, 1001}) return bmdModeHD1080p5994;
  if (r == Rate{60, 1}) return bmdModeHD1080p6000;
  throw std::invalid_argument("no DeckLink mode for rate " + r.str());
}
}  // namespace bc