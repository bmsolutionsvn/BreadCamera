#include "engine/adapter_factory.h"

#include <stdexcept>

#include "core/utf8.h"
#include "io/ffmpeg_input.h"
#include "io/fullscreen_output.h"
#include "io/mf_capture_input.h"
#include "io/srt_output.h"
#include "io/srt_params.h"
#if BC_WITH_NDI
#include "io/ndi_input.h"
#include "io/ndi_output.h"
#endif

namespace bc {
std::unique_ptr<Input> DefaultAdapterFactory::makeInput(const InputConfig& cfg) {
  if (cfg.kind == "srt") return std::make_unique<FfmpegInput>(gpu_, pool_, srtOptionsFromParams(cfg.params));
  if (cfg.kind == "ndi") {
#if BC_WITH_NDI
    return std::make_unique<NdiInput>(gpu_, pool_, cfg.params.at("source").get<std::string>());
#else
    throw std::invalid_argument("NDI support is not installed in this build");
#endif
  }
  if (cfg.kind == "uvc") {
    return std::make_unique<MfCaptureInput>(gpu_, pool_, cfg.params.at("device").get<std::string>(), cfg.params.value("width", 0),
                                            cfg.params.value("height", 0), cfg.params.value("fps", 0));
  }
  throw std::invalid_argument("unknown input kind '" + cfg.kind + "'");
}

std::unique_ptr<Output> DefaultAdapterFactory::makeOutput(const OutputConfig& cfg, const CameraConfig& cam) {
  if (cfg.kind == "ndi") {
    // Same rule as CommandRouter::validateOutput: a hand-edited show file with a
    // non-string name is rejected here too instead of silently using the default.
    if (cfg.params.contains("name") && !cfg.params["name"].is_string())
      throw std::invalid_argument("NDI name must be text");
#if BC_WITH_NDI
    std::string name = cfg.params.value("name", std::string("BreadCamera - ") + cam.name);
    if (name.empty() || utf8Length(name) > 200) throw std::invalid_argument("NDI name must be 1 to 200 characters");
    return std::make_unique<NdiOutput>(name);
#else
    (void)cam;
    throw std::invalid_argument("NDI support is not installed in this build");
#endif
  }
  if (cfg.kind == "fullscreen") {
    if (cfg.params.contains("monitor") && !cfg.params["monitor"].is_number_integer()) {
      throw std::invalid_argument("monitor must be a whole number");
    }
    return std::make_unique<FullscreenOutput>(gpu_, cfg.params.value("monitor", 0));
  }
  if (cfg.kind == "srt") return std::make_unique<SrtOutput>(srtOutputOptionsFromParams(cfg.params));
  throw std::invalid_argument("unknown output kind '" + cfg.kind + "'");
}
}  // namespace bc
