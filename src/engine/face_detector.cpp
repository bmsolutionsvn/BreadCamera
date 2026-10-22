#include "engine/face_detector.h"

#if BC_WITH_FACE_DETECT

#include <windows.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "gpu/readback.h"

namespace bc {
namespace {
// A quarter-size picture is plenty for a face and keeps the readback and CPU
// cost small.
constexpr int kW = 320;
constexpr int kH = 180;
// Below this the box is not trusted; the framer then holds the last framing.
constexpr float kMinScore = 0.6f;
} // namespace

struct FaceDetector::Impl {
  Converter &conv;
  cv::Ptr<cv::FaceDetectorYN> detector;

  struct Cam {
    ReadbackSlot slot;
    bool hasPending = false;
    CpuImage pending;
    std::optional<FaceBox> latest;
  };

  mutable std::mutex m;
  std::condition_variable cv;
  std::map<std::string, Cam> cams;
  std::thread worker;
  bool stop = false;

  Impl(Converter &conv_, const std::filesystem::path &model) : conv(conv_) {
    if (!std::filesystem::exists(model))
      throw std::runtime_error("face model not found: " + model.string());
    detector = cv::FaceDetectorYN::create(model.string(), "", cv::Size(kW, kH),
                                          kMinScore, 0.3f, 50);
    if (detector.empty())
      throw std::runtime_error("face model could not load: " + model.string());
    worker = std::thread([this] { run(); });
  }

  ~Impl() {
    {
      std::lock_guard l(m);
      stop = true;
    }
    cv.notify_all();
    if (worker.joinable())
      worker.join();
  }

  void run() {
    std::string lastId;
    while (true) {
      std::string id;
      CpuImage img;
      {
        std::unique_lock l(m);
        cv.wait(l, [&] {
          return stop ||
                 std::any_of(cams.begin(), cams.end(), [](const auto &kv) {
                   return kv.second.hasPending;
                 });
        });
        if (stop)
          return;
        // Round-robin, so one busy camera cannot starve the others.
        auto it = cams.upper_bound(lastId);
        for (size_t n = 0; n < cams.size(); ++n, ++it) {
          if (it == cams.end())
            it = cams.begin();
          if (it->second.hasPending)
            break;
        }
        id = it->first;
        img = std::move(it->second.pending);
        it->second.hasPending = false;
      }
      lastId = id;

      std::optional<FaceBox> found;
      try {
        found = detect(img);
      } catch (const cv::Exception &) {
        // A bad frame is not fatal: the framer holds, and the next frame tries
        // again.
      }

      std::lock_guard l(m);
      auto it = cams.find(id);
      if (it != cams.end())
        it->second.latest = found;
    }
  }

  std::optional<FaceBox> detect(const CpuImage &img) {
    if (img.width != (uint32_t)kW || img.height != (uint32_t)kH)
      return std::nullopt;
    cv::Mat bgra(kH, kW, CV_8UC4, const_cast<uint8_t *>(img.bgra.data()));
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);

    cv::Mat faces;
    detector->detect(bgr, faces);
    if (faces.empty())
      return std::nullopt;

    // The biggest confident face is the one being framed; a face at the back of
    // the room does not steal the shot from the presenter.
    float bestArea = 0;
    FaceBox best;
    for (int i = 0; i < faces.rows; ++i) {
      const float *f = faces.ptr<float>(i);
      if (f[14] < kMinScore)
        continue;
      const float area = f[2] * f[3];
      if (area <= bestArea)
        continue;
      bestArea = area;
      best = FaceBox{f[0] / kW, f[1] / kH, f[2] / kW, f[3] / kH};
    }
    if (bestArea <= 0)
      return std::nullopt;
    return best;
  }
};

FaceDetector::FaceDetector(Converter &conv, std::filesystem::path modelPath)
    : impl_(std::make_unique<Impl>(conv, modelPath)) {}

FaceDetector::~FaceDetector() = default;

void FaceDetector::process(std::vector<FaceJob *> &jobs) {
  for (auto *job : jobs) {
    if (!job || !job->frame)
      continue;
    auto tex = impl_->conv.toBgra8(*job->frame, kW, kH);
    // Hold the map lock while the slot is touched: forget() may erase this
    // camera otherwise.
    std::lock_guard l(impl_->m);
    Impl::Cam &cam = impl_->cams[job->cameraId];
    CpuImage done;
    if (cam.slot.tryFinish(impl_->conv, done)) {
      cam.pending = std::move(done);
      cam.hasPending = true;
    }
    if (!cam.slot.pending())
      cam.slot.begin(impl_->conv, *tex);
  }
  impl_->cv.notify_one();
}

std::optional<FaceBox> FaceDetector::face(const std::string &cameraId) const {
  std::lock_guard l(impl_->m);
  auto it = impl_->cams.find(cameraId);
  return it == impl_->cams.end() ? std::nullopt : it->second.latest;
}

void FaceDetector::forget(const std::string &cameraId) {
  std::lock_guard l(impl_->m);
  impl_->cams.erase(cameraId);
}

std::filesystem::path defaultFaceModel() {
  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path() / "models" / "yunet.onnx";
}
} // namespace bc

#else // BC_WITH_FACE_DETECT

namespace bc {
struct FaceDetector::Impl {};

FaceDetector::FaceDetector(Converter &, std::filesystem::path) {
  throw std::runtime_error("face detection is not built in");
}
FaceDetector::~FaceDetector() = default;
void FaceDetector::process(std::vector<FaceJob *> &) {}
std::optional<FaceBox> FaceDetector::face(const std::string &) const {
  return std::nullopt;
}
void FaceDetector::forget(const std::string &) {}
std::filesystem::path defaultFaceModel() { return {}; }
} // namespace bc

#endif
