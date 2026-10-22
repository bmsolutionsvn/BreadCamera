#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/autoframe.h"
#include "gpu/converter.h"

namespace bc {
struct FaceJob {
  std::string cameraId;
  TextureRef frame; // linear RGBA16F, any size
};

// AI face detection for Auto frame: YuNet through OpenCV, fed by a small
// off-tick readback. One worker thread owns the detector; the tick only starts
// the copy and reads the last result.
class FaceDetector {
public:
  FaceDetector(
      Converter &conv,
      std::filesystem::path modelPath); // throws when the model will not load
  ~FaceDetector();
  FaceDetector(const FaceDetector &) = delete;
  FaceDetector &operator=(const FaceDetector &) = delete;

  // Tick thread: hands over this tick's frame (a 320x180 copy is issued; never
  // blocks).
  void process(std::vector<FaceJob *> &jobs);
  // The last face found for that camera, normalized 0..1; empty when none.
  std::optional<FaceBox> face(const std::string &cameraId) const;
  void forget(const std::string &cameraId);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// <exe folder>/models/yunet.onnx
std::filesystem::path defaultFaceModel();
} // namespace bc
