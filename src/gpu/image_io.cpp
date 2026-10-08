#include "gpu/image_io.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace bc {
CpuImage loadImageBgra(const std::filesystem::path& path) {
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct Uninit {
    bool on;
    ~Uninit() { if (on) CoUninitialize(); }
  } uninit{SUCCEEDED(co)};
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
    throw std::runtime_error("WIC is not available");
  }
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
    throw std::runtime_error("cannot open image " + path.string());
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, &frame))) throw std::runtime_error("cannot read image " + path.string());
  ComPtr<IWICBitmapSource> bgra;
  if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &bgra))) {
    throw std::runtime_error("cannot convert image " + path.string());
  }
  CpuImage img;
  bgra->GetSize(&img.width, &img.height);
  img.bgra.resize(static_cast<size_t>(img.width) * img.height * 4);
  if (FAILED(bgra->CopyPixels(nullptr, img.width * 4, static_cast<UINT>(img.bgra.size()), img.bgra.data()))) {
    throw std::runtime_error("cannot decode image " + path.string());
  }
  return img;
}
}  // namespace bc
