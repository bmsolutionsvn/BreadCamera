#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/rate.h"

struct IDeckLink;

namespace bc {
struct DeckLinkInfo {
  int index = 0;
  std::string name;
  bool hasInput = false, hasOutput = false, externalKeying = false;
};
std::vector<DeckLinkInfo> listDeckLinkDevices();
uint32_t deckLinkModeFor(Rate r);  // 1080p50 / 1080p59.94 / 1080p60
IDeckLink* openDeckLink(int index);  // AddRef'd; throws std::runtime_error when absent
}  // namespace bc