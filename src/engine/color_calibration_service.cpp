#include "engine/color_calibration_service.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include "core/calibration.h"
#include "core/lut.h"
#include "core/utf8.h"
#include "engine/chart_detector.h"

namespace bc {
namespace {
using nlohmann::json;

json colorsToJson(const ChartColors& c) {
  json out = json::array();
  for (auto& v : c) out.push_back({v.r, v.g, v.b});
  return out;
}
ChartColors colorsFromJson(const json& j) {
  if (!j.is_array() || j.size() != 24) throw std::runtime_error("bad tablet reference file");
  ChartColors out;
  for (size_t i = 0; i < 24; ++i) out[i] = {j[i][0].get<float>(), j[i][1].get<float>(), j[i][2].get<float>()};
  return out;
}
}  // namespace

ColorCalibrationService::ColorCalibrationService(Engine& engine, std::filesystem::path lutDir)
    : engine_(engine), lutDir_(std::move(lutDir)) {}

std::filesystem::path ColorCalibrationService::newLutPath(const std::string& cameraId) const {
  const auto t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
  auto p = lutDir_ / (cameraId + "-" + stamp + ".cube");
  for (int n = 2; std::filesystem::exists(p); ++n) p = lutDir_ / (cameraId + "-" + stamp + "-" + std::to_string(n) + ".cube");
  return p;
}

json ColorCalibrationService::find(const std::string& cameraId) {
  if (!chartDetectionAvailable()) return {{"available", false}, {"found", false}};
  auto frames = engine_.capture({cameraId}, 1, 1, 960, 540);
  auto q = findChart(frames.at(cameraId).front());
  if (!q) return {{"available", true}, {"found", false}};
  json corners = json::array();
  for (auto& c : q->c) corners.push_back({c[0], c[1]});
  return {{"available", true}, {"found", true}, {"corners", corners}};
}

ColorCalibrationService::Result ColorCalibrationService::calibrate(const std::string& cameraId, const std::string& chart,
                                                                   const ChartQuad& q) {
  std::lock_guard l(m_);
  if (!validQuad(q)) throw std::invalid_argument("place the four corners on the chart");
  auto frames = engine_.capture({cameraId}, 15, 2, 960, 540);
  const ChartColors measured = sampleChart(frames.at(cameraId), q);
  std::filesystem::create_directories(lutDir_);
  Result r;
  ChartColors target = colorCheckerReference();
  if (chart == "tablet") {
    const auto refFile = lutDir_ / "tablet-reference.json";
    if (!std::filesystem::exists(refFile)) {
      std::ofstream(refFile) << json{{"cameraId", cameraId}, {"colors", colorsToJson(measured)}}.dump(2);
      const auto path = newLutPath(cameraId);
      writeCube(path, Lut3D::identity(33), "BreadCamera tablet reference");
      return {toUtf8(path.wstring()), 0, 0, "good", true};
    }
    target = colorsFromJson(json::parse(std::ifstream(refFile))["colors"]);
  }
  const auto cal = solveCalibration(measured, target);
  const auto path = newLutPath(cameraId);
  writeCube(path, cal.lut, "BreadCamera chart calibration");
  r.lutPath = toUtf8(path.wstring());
  r.deltaEBefore = cal.deltaEBefore;
  r.deltaE = cal.deltaEAfter;
  r.rating = std::string(calibrationRating(cal.deltaEAfter));
  return r;
}

void ColorCalibrationService::forget(const std::string& cameraId) {
  std::lock_guard l(m_);
  const auto refFile = lutDir_ / "tablet-reference.json";
  std::error_code ec;
  if (!std::filesystem::exists(refFile, ec)) return;
  try {
    if (json::parse(std::ifstream(refFile)).value("cameraId", std::string()) == cameraId) std::filesystem::remove(refFile, ec);
  } catch (const json::exception&) {
    std::filesystem::remove(refFile, ec);  // unreadable: drop it rather than match to garbage
  }
}
}  // namespace bc