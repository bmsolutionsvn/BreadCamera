#include "core/secrets.h"

#include <windows.h>
#include <wincrypt.h>

#include <stdexcept>
#include <vector>

namespace bc {
std::string protectSecret(std::string_view plain) {
  DATA_BLOB in{static_cast<DWORD>(plain.size()),
               reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  DATA_BLOB out{};
  if (!CryptProtectData(&in, L"BreadCamera", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    throw std::runtime_error("CryptProtectData failed: " + std::to_string(GetLastError()));
  }
  DWORD len = 0;
  CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       nullptr, &len);
  std::string b64(len, '\0');
  CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       b64.data(), &len);
  b64.resize(len);
  while (!b64.empty() && b64.back() == '\0') b64.pop_back();  // trim a NUL if the API counted it
  LocalFree(out.pbData);
  return b64;
}

std::string unprotectSecret(std::string_view base64) {
  DWORD len = 0;
  if (!CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()),
                            CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) {
    throw std::runtime_error("secret is not valid base64");
  }
  std::vector<BYTE> bin(len);
  CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64,
                       bin.data(), &len, nullptr, nullptr);
  DATA_BLOB in{len, bin.data()};
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &out)) {
    throw std::runtime_error("CryptUnprotectData failed: " + std::to_string(GetLastError()));
  }
  std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return plain;
}
}  // namespace bc
