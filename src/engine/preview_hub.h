#pragma once
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bc {
enum class PreviewSize { Small, Large };
struct PreviewDims {
  int w, h;
};
inline PreviewDims dims(PreviewSize s) { return s == PreviewSize::Large ? PreviewDims{1280, 720} : PreviewDims{512, 288}; }
std::optional<PreviewSize> parsePreviewSize(std::string_view s);

// Which UI client watches which camera preview (spec §6.3: only visible tiles stream).
class PreviewHub {
 public:
  using Key = std::pair<std::string, PreviewSize>;
  std::vector<Key> subscribe(const std::string& client, std::vector<std::string> cameraIds, PreviewSize size);
  void dropClient(const std::string& client);
  std::set<Key> wanted() const;
  std::vector<std::string> clientsFor(const std::string& cameraId, PreviewSize size) const;

 private:
  mutable std::mutex m_;
  std::map<std::string, std::set<Key>> byClient_;
};
}  // namespace bc
