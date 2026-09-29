#pragma once

// IB_VERSION is set by CMake (project version, or the release tag in CI).
#ifndef IB_VERSION
#define IB_VERSION "dev"
#endif

namespace ib {

inline constexpr wchar_t kVersion[] = L"" IB_VERSION;
inline constexpr wchar_t kIssuesUrl[] = L"https://github.com/Zyneq/FloatBar/issues/new";

}  // namespace ib
