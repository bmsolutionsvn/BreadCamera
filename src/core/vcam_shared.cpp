#include "core/vcam_shared.h"

#include <sddl.h>

#include <cstring>

namespace bc::vcam {
namespace {
int64_t qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
int64_t qpcFreq() {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return f.QuadPart;
}
}  // namespace

bool Writer::open(int index, bool global) {
  close();
  map_ = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, mappingName(index, global).c_str());
  if (!map_) return false;
  view_ = static_cast<uint8_t*>(MapViewOfFile(map_, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, kMappingBytes));
  if (!view_) {
    close();
    return false;
  }
  return true;
}

bool Writer::write(const uint8_t* bgra) {
  if (!view_) return false;
  auto* h = reinterpret_cast<Header*>(view_);
  InterlockedIncrement64(&h->seq);  // odd: writing
  MemoryBarrier();
  std::memcpy(view_ + kHeaderBytes, bgra, kFrameBytes);
  h->width = kWidth;
  h->height = kHeight;
  h->writtenAtQpc = qpc();
  MemoryBarrier();
  InterlockedIncrement64(&h->seq);  // even: done
  return true;
}

void Writer::close() {
  if (view_) UnmapViewOfFile(view_);
  if (map_) CloseHandle(map_);
  view_ = nullptr;
  map_ = nullptr;
}

bool Reader::create(int index, bool global) {
  close();
  PSECURITY_DESCRIPTOR sd = nullptr;
  // SYSTEM full control, Authenticated Users read/write (the engine runs as the signed-in user).
  ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GRGW;;;AU)", SDDL_REVISION_1, &sd, nullptr);
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
  map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(kMappingBytes),
                            mappingName(index, global).c_str());
  if (sd) LocalFree(sd);
  if (!map_) return false;
  view_ = static_cast<uint8_t*>(MapViewOfFile(map_, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, kMappingBytes));
  if (!view_) {
    close();
    return false;
  }
  auto* h = reinterpret_cast<Header*>(view_);
  if (h->magic != kMagic) {
    h->magic = kMagic;
    h->version = 1;
    h->seq = 0;
    h->writtenAtQpc = 0;
  }
  return true;
}

bool Reader::readLatest(uint8_t* dst, std::chrono::milliseconds maxAge) {
  if (!view_) return false;
  auto* h = reinterpret_cast<Header*>(view_);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const LONG64 before = h->seq;
    MemoryBarrier();
    if ((before & 1) || before == 0) continue;
    const int64_t at = h->writtenAtQpc;
    std::memcpy(dst, view_ + kHeaderBytes, kFrameBytes);
    MemoryBarrier();
    if (h->seq != before) continue;  // torn: try once more, else keep the previous frame
    const int64_t ageMs = (qpc() - at) * 1000 / qpcFreq();
    return ageMs <= maxAge.count();
  }
  return false;
}

void Reader::close() {
  if (view_) UnmapViewOfFile(view_);
  if (map_) CloseHandle(map_);
  view_ = nullptr;
  map_ = nullptr;
}
}  // namespace bc::vcam