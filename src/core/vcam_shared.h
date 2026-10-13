#pragma once
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace bc::vcam {
constexpr uint32_t kWidth = 1920, kHeight = 1080;
constexpr size_t kFrameBytes = size_t{kWidth} * kHeight * 4;
constexpr size_t kHeaderBytes = 64;
constexpr size_t kMappingBytes = kHeaderBytes + kFrameBytes;
constexpr uint32_t kMagic = 0x4D414342;  // "BCAM"

// Fixed CLSIDs of the four BreadCamera virtual cameras (the DLL registers all four).
inline constexpr const wchar_t* kClsids[4] = {
    L"{7B1E3A40-5C2D-4E8F-9A61-0B3C4D5E6F01}", L"{7B1E3A40-5C2D-4E8F-9A61-0B3C4D5E6F02}",
    L"{7B1E3A40-5C2D-4E8F-9A61-0B3C4D5E6F03}", L"{7B1E3A40-5C2D-4E8F-9A61-0B3C4D5E6F04}"};

inline std::wstring mappingName(int index, bool global = true) {
  return std::wstring(global ? L"Global\\" : L"Local\\") + L"BreadCameraVcam" + std::to_wstring(index);
}

struct Header {
  uint32_t magic;
  uint32_t version;
  volatile LONG64 seq;
  int64_t writtenAtQpc;
  uint32_t width, height;
};

// Engine side: opens the section the DLL created; false when no app has the camera open.
class Writer {
 public:
  ~Writer() { close(); }
  bool open(int index, bool global = true);
  bool write(const uint8_t* bgra1080p);
  void close();
  bool isOpen() const { return view_ != nullptr; }

 private:
  HANDLE map_ = nullptr;
  uint8_t* view_ = nullptr;
};

// DLL side: creates the section (with a DACL the user's engine can open) and reads frames.
class Reader {
 public:
  ~Reader() { close(); }
  bool create(int index, bool global = true);
  bool readLatest(uint8_t* dstBgra1080p, std::chrono::milliseconds maxAge);
  void close();

 private:
  HANDLE map_ = nullptr;
  uint8_t* view_ = nullptr;
};
}  // namespace bc::vcam