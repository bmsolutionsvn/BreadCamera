#pragma once
#include <string>
#include <vector>

#include "io/mf_capture_input.h"  // bc::CaptureDevice

namespace bc {
// A DirectShow video-input device (CLSID_VideoInputDeviceCategory). OBS Virtual Camera and
// other DirectShow-only virtual cameras live here, where Media Foundation cannot see them.
struct DShowDevice {
  std::string name;         // IPropertyBag "FriendlyName"
  std::string displayName;  // IMoniker::GetDisplayName — the stable id
  std::string devicePath;   // IPropertyBag "DevicePath"; empty for software filters
};

// Every DirectShow video capture source, in enumeration order. Runs on the calling thread and
// initialises COM itself; returns empty when the machine has none.
std::vector<DShowDevice> listDShowDevices();

// The MF devices first, then every DirectShow device the MF list does not already hold: a
// DirectShow device is skipped when its DevicePath (case-insensitive) matches an MF id, when
// its friendly name matches an MF name, or when the result already holds its id. A DirectShow
// device that survives is listed as {"name", "dshow:<display name>"}.
std::vector<CaptureDevice> mergeCaptureDevices(const std::vector<CaptureDevice>& mf,
                                               const std::vector<DShowDevice>& dshow);
}  // namespace bc
