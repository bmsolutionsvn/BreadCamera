#include "io/ndi_discovery.h"

#include <Processing.NDI.Lib.h>

#include <stdexcept>

namespace bc {
std::vector<std::string> listNdiSources(std::chrono::milliseconds wait) {
  static const bool ok = NDIlib_initialize();
  if (!ok) throw std::runtime_error("the NDI runtime could not start");
  NDIlib_find_create_t fc{};
  fc.show_local_sources = true;
  NDIlib_find_instance_t find = NDIlib_find_create_v2(&fc);
  if (!find) throw std::runtime_error("cannot start NDI discovery");
  NDIlib_find_wait_for_sources(find, static_cast<uint32_t>(wait.count()));
  uint32_t n = 0;
  const NDIlib_source_t* srcs = NDIlib_find_get_current_sources(find, &n);
  std::vector<std::string> out;
  for (uint32_t i = 0; i < n; ++i) out.emplace_back(srcs[i].p_ndi_name);
  NDIlib_find_destroy(find);
  return out;
}
}  // namespace bc
