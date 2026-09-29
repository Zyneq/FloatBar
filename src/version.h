#pragma once

// FB_VERSION is set by CMake (project version, or the release tag in CI).
#ifndef FB_VERSION
#define FB_VERSION "dev"
#endif

namespace fb {

inline constexpr wchar_t kVersion[] = L"" FB_VERSION;
inline constexpr wchar_t kIssuesUrl[] = L"https://github.com/Zyneq/FloatBar/issues/new";

}  // namespace fb
