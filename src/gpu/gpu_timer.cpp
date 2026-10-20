#include "gpu/gpu_timer.h"

#include <stdexcept>

namespace bc {
namespace {
Microsoft::WRL::ComPtr<ID3D11Query> makeQuery(ID3D11Device* device, D3D11_QUERY type) {
  D3D11_QUERY_DESC desc{};
  desc.Query = type;
  Microsoft::WRL::ComPtr<ID3D11Query> query;
  if (FAILED(device->CreateQuery(&desc, &query))) {
    throw std::runtime_error("cannot create a D3D11 timestamp query");
  }
  return query;
}
}  // namespace

GpuTimer::GpuTimer(GpuDevice& gpu) : gpu_(gpu) {
  for (auto& s : sets_) {
    s.disjoint = makeQuery(gpu_.device(), D3D11_QUERY_TIMESTAMP_DISJOINT);
    s.begin = makeQuery(gpu_.device(), D3D11_QUERY_TIMESTAMP);
    s.end = makeQuery(gpu_.device(), D3D11_QUERY_TIMESTAMP);
  }
}

void GpuTimer::begin() {
  active_ = -1;
  if (pendingCount_ >= kSets) return;  // the ring is full: record nothing
  for (int s = 0; s < kSets; ++s) {
    bool inUse = false;
    for (int i = 0; i < pendingCount_; ++i) inUse = inUse || pending_[i] == s;
    if (!inUse) {
      active_ = s;
      break;
    }
  }
  if (active_ < 0) return;
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  ctx->Begin(sets_[active_].disjoint.Get());
  ctx->End(sets_[active_].begin.Get());
}

void GpuTimer::end() {
  if (active_ < 0) return;
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  ctx->End(sets_[active_].end.Get());
  ctx->End(sets_[active_].disjoint.Get());
  pending_[pendingCount_++] = active_;
  active_ = -1;
}

std::optional<std::chrono::nanoseconds> GpuTimer::poll() {
  if (pendingCount_ == 0) return std::nullopt;
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  std::optional<std::chrono::nanoseconds> newest;
  std::array<int, kSets> kept{};
  int keptCount = 0;
  for (int i = 0; i < pendingCount_; ++i) {
    const int s = pending_[i];
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    if (ctx->GetData(sets_[s].disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
      kept[keptCount++] = s;  // still running: leave it pending
      continue;
    }
    if (disjoint.Disjoint || disjoint.Frequency == 0) {
      continue;  // a broken measurement: discard the slot
    }
    // The disjoint query is done, but the driver can report the timestamp data a moment
    // later, so read it plainly and only then consume the slot (the read never blocks).
    UINT64 b = 0, e = 0;
    if (ctx->GetData(sets_[s].begin.Get(), &b, sizeof(b), 0) == S_OK &&
        ctx->GetData(sets_[s].end.Get(), &e, sizeof(e), 0) == S_OK && e >= b) {
      const double ns = static_cast<double>(e - b) * 1e9 / static_cast<double>(disjoint.Frequency);
      newest = std::chrono::nanoseconds(static_cast<int64_t>(ns + 0.5));  // the newest wins
    } else {
      kept[keptCount++] = s;  // timestamps not readable yet: try again on a later poll
    }
  }
  for (int i = 0; i < keptCount; ++i) pending_[i] = kept[i];
  pendingCount_ = keptCount;
  return newest;
}
}  // namespace bc