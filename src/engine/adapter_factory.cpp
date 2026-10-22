#include "engine/adapter_factory.h"

#include <stdexcept>

#include "core/utf8.h"
#include "io/dshow_capture_input.h"
#include "io/ffmpeg_input.h"
#include "io/fullscreen_output.h"
#include "io/mf_capture_input.h"
#include "io/srt_output.h"
#include "io/srt_params.h"
#if BC_WITH_NDI
#include "io/ndi_input.h"
#include "io/ndi_output.h"
#endif
#if BC_WITH_OMT
#include "io/omt_input.h"
#include "io/omt_output.h"
#include "omthx/omthx.h"
#endif
#if BC_WITH_DECKLINK
#include "io/decklink_input.h"
#include "io/decklink_output.h"
#endif
#if BC_WITH_AJA
#include "io/aja_input.h"
#include "io/aja_output.h"
#endif
#if BC_WITH_VCAM
#include "io/vcam_output.h"
#endif
#if BC_WITH_LIVECALL
#include "io/breadcall_input.h"
#endif

namespace bc {
std::unique_ptr<Input> DefaultAdapterFactory::makeInput(const InputConfig& cfg) {
  if (cfg.kind == "srt") return std::make_unique<FfmpegInput>(gpu_, pool_, srtOptionsFromParams(cfg.params));
  if (cfg.kind == "ndi") {
#if BC_WITH_NDI
    // showFromJson does not run validateInput, so a hand-edited show file must be guarded
    // here too (same rule as the NDI output below).
    if (!cfg.params.contains("source") || !cfg.params["source"].is_string())
      throw std::invalid_argument("NDI source is required");
    if (cfg.params.contains("hx") && !cfg.params["hx"].is_boolean()) throw std::invalid_argument("NDI HX must be true or false");
    return std::make_unique<NdiInput>(gpu_, pool_, cfg.params["source"].get<std::string>(),
                                      cfg.params.value("hx", false));
#else
    throw std::invalid_argument("NDI support is not installed in this build");
#endif
  }
  if (cfg.kind == "omt") {
#if BC_WITH_OMT
    if (!cfg.params.contains("source") || !cfg.params["source"].is_string())
      throw std::invalid_argument("OMT source is required");
    return std::make_unique<OmtInput>(gpu_, pool_, cfg.params["source"].get<std::string>());
#else
    throw std::invalid_argument("OMT support is not installed in this build");
#endif
  }
  if (cfg.kind == "uvc") {
    if (!cfg.params.contains("device") || !cfg.params["device"].is_string())
      throw std::invalid_argument("device is required");
    const std::string device = cfg.params["device"].get<std::string>();
    // A DirectShow-only camera (OBS Virtual Camera) is listed as "dshow:<display name>";
    // everything else stays on the Media Foundation path.
    if (device.rfind("dshow:", 0) == 0) return std::make_unique<DShowCaptureInput>(gpu_, pool_, device.substr(6));
    return std::make_unique<MfCaptureInput>(gpu_, pool_, device, cfg.params.value("width", 0), cfg.params.value("height", 0),
                                            cfg.params.value("fps", 0));
  }
  if (cfg.kind == "decklink") {
#if BC_WITH_DECKLINK
    return std::make_unique<DeckLinkInput>(gpu_, pool_, cfg.params.at("device").get<int>());
#else
    throw std::invalid_argument("DeckLink support is not installed in this build");
#endif
  }
  if (cfg.kind == "aja") {
#if BC_WITH_AJA
    return std::make_unique<AjaInput>(gpu_, pool_, cfg.params.at("device").get<int>(), cfg.params.at("channel").get<int>());
#else
    throw std::invalid_argument("AJA support is not installed in this build");
#endif
  }
  if (cfg.kind == "breadcall") {
    // showFromJson does not run validateInput, so a hand-edited show file must be guarded
    // here too (same rule as NDI above): non-string url or token is refused.
    if (!cfg.params.contains("url") || !cfg.params["url"].is_string())
      throw std::invalid_argument("BreadCall room URL is required");
    if (!cfg.params.contains("token") || !cfg.params["token"].is_string())
      throw std::invalid_argument("BreadCall token is required");
    if (!cfg.params.contains("slot") || !cfg.params["slot"].is_number_integer())
      throw std::invalid_argument("BreadCall slot is required (a whole number)");
#if BC_WITH_LIVECALL
    return std::make_unique<BreadCallInput>(gpu_, pool_, cfg.params["url"].get<std::string>(),
                                            cfg.params["token"].get<std::string>(), cfg.params.at("slot").get<int>());
#else
    throw std::invalid_argument("BreadCall support is not built in this build");
#endif
  }
  throw std::invalid_argument("unknown input kind '" + cfg.kind + "'");
}

std::unique_ptr<Output> DefaultAdapterFactory::makeOutput(const OutputConfig& cfg, const CameraConfig& cam, Rate videoRate) {
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
  if (cfg.kind == "omt") {
    // Same rule as CommandRouter::validateOutput: a hand-edited show file with a
    // non-string mode or name is rejected here too instead of silently using the default.
    if (cfg.params.contains("mode") && !cfg.params["mode"].is_string())
      throw std::invalid_argument("OMT mode must be text");
    if (cfg.params.contains("name") && !cfg.params["name"].is_string())
      throw std::invalid_argument("OMT name must be text");
    // Same rule as CommandRouter::validateOutput: a hand-edited show file with a
    // non-string codec is rejected here too; the value is ignored for plain OMT (full).
    if (cfg.params.contains("codec") && !cfg.params["codec"].is_string())
      throw std::invalid_argument("OMT codec must be text");
#if BC_WITH_OMT
    const std::string mode = cfg.params.value("mode", std::string("hx"));
    std::string name = cfg.params.value("name", std::string("BreadCamera - ") + cam.name);
    if (name.empty() || utf8Length(name) > 200) throw std::invalid_argument("OMT name must be 1 to 200 characters");
    const auto codec = cfg.params.value("codec", std::string("hevc")) == "h264" ? omthx::Codec::H264 : omthx::Codec::Hevc;
    return std::make_unique<OmthOutput>(name, mode == "full" ? omthx::Mode::Full : omthx::Mode::Hx, codec);
#else
    (void)cam;
    throw std::invalid_argument("OMT support is not built (BC_WITH_OMT=OFF)");
#endif
  }
  if (cfg.kind == "decklink") {
#if BC_WITH_DECKLINK
    return std::make_unique<DeckLinkOutput>(cfg.params.at("device").get<int>(), videoRate, cfg.keyed);
#else
    throw std::invalid_argument("DeckLink support is not installed in this build");
#endif
  }
  if (cfg.kind == "aja") {
#if BC_WITH_AJA
    return std::make_unique<AjaOutput>(cfg.params.at("device").get<int>(), cfg.params.at("channel").get<int>(), videoRate);
#else
    throw std::invalid_argument("AJA support is not installed in this build");
#endif
  }
  if (cfg.kind == "vcam") {
#if BC_WITH_VCAM
    return std::make_unique<VirtualCameraOutput>(cfg.params.at("index").get<int>(), cfg.params.value("name", "BreadCamera - " + cam.name));
#else
    (void)cam;
    throw std::invalid_argument("the virtual camera is not available in this build");
#endif
  }
  throw std::invalid_argument("unknown output kind '" + cfg.kind + "'");
}
}  // namespace bc
