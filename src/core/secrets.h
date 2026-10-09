#pragma once
#include <string>
#include <string_view>

namespace bc {
// DPAPI (current user) + base64. Spec §6.2: secrets never sit on disk as plain text.
std::string protectSecret(std::string_view plain);
// Throws std::runtime_error when the input is not base64 or not decryptable by this user.
std::string unprotectSecret(std::string_view base64);
}  // namespace bc
