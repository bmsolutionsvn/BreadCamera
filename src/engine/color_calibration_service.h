#pragma once
#include <filesystem>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "core/chart.h"
#include "engine/engine.h"

namespace bc {
// One-time chart calibration (spec §5.3): capture, sample, solve, write the LUT.
class ColorCalibrationService {
 public:
  struct Result {
    std::string lutPath;
    double deltaEBefore = 0;
    double deltaE = 0;
    std::string rating;
    bool reference = false;
  };
  ColorCalibrationService(Engine& engine, std::filesystem::path lutDir);
  nlohmann::json find(const std::string& cameraId);
  Result calibrate(const std::string& cameraId, const std::string& chart, const ChartQuad& q);
  void forget(const std::string& cameraId);

 private:
  std::filesystem::path newLutPath(const std::string& cameraId) const;
  Engine& engine_;
  std::filesystem::path lutDir_;
  std::mutex m_;  // one calibration at a time; guards the tablet reference file
};
}  // namespace bc