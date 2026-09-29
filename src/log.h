#pragma once

#include <string>

namespace fb::log {

// Rolling log at <dir>\log.txt; rotated to log.old.txt at about 1 MB.
void Init(const std::wstring& dir);
std::wstring Path();
void Write(const wchar_t* format, ...);

// Verbose lines are only written after the user opted in (Settings → Verbose debug logging).
void SetVerbose(bool verbose);
bool Verbose();
void Debug(const wchar_t* format, ...);

}  // namespace fb::log
