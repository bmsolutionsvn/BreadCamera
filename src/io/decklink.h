#pragma once
#include <cstdint>
#include <optional>
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
uint32_t deckLinkModeFor(Rate r);  // 1080p at the supported rates
std::optional<Rate> deckLinkRateFor(uint32_t displayMode);  // nullopt for a non-1080p mode
IDeckLink* openDeckLink(int index);  // AddRef'd; throws std::runtime_error when absent
}  // namespace bc