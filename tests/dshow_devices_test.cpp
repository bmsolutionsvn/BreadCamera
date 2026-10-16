#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include "io/dshow_devices.h"

using bc::CaptureDevice;
using bc::DShowDevice;

TEST_CASE("mergeCaptureDevices: a camera both stacks see is listed once") {
  std::vector<CaptureDevice> mf = {{"HD WebCam", "\\\\?\\usb#vid_0408"}};
  std::vector<DShowDevice> ds = {{"HD WebCam", "@device:pnp:x", "\\\\?\\usb#vid_0408"}};
  CHECK(bc::mergeCaptureDevices(mf, ds).size() == 1);
}

TEST_CASE("mergeCaptureDevices: a DirectShow-only device is appended with a dshow: id") {
  std::vector<CaptureDevice> mf = {{"HD WebCam", "\\\\?\\usb#vid_0408"}};
  std::vector<DShowDevice> ds = {{"OBS Virtual Camera", "@device:sw:obs", ""}};
  auto out = bc::mergeCaptureDevices(mf, ds);
  REQUIRE(out.size() == 2);
  CHECK(out[1].name == "OBS Virtual Camera");
  CHECK(out[1].id == "dshow:@device:sw:obs");
}

TEST_CASE("mergeCaptureDevices: a duplicate with no DevicePath falls back to the name") {
  std::vector<CaptureDevice> mf = {{"HD WebCam", "\\\\?\\usb#vid_0408"}};
  std::vector<DShowDevice> ds = {{"HD WebCam", "@device:sw:x", ""}};
  CHECK(bc::mergeCaptureDevices(mf, ds).size() == 1);
}

TEST_CASE("mergeCaptureDevices: the search is case-insensitive") {
  std::vector<CaptureDevice> mf = {{"HD WebCam", "\\\\?\\usb#vid_0408"}};
  std::vector<DShowDevice> ds = {{"hd webcam", "@device:pnp:x", "\\\\?\\USB#VID_0408"}};
  CHECK(bc::mergeCaptureDevices(mf, ds).size() == 1);
}

TEST_CASE("listDShowDevices: runs and returns names with display names") {
  for (auto& d : bc::listDShowDevices()) {
    CHECK_FALSE(d.name.empty());
    CHECK_FALSE(d.displayName.empty());
  }
}
