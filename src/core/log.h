#pragma once

#include <string>

namespace ib::log {

// Rolling log at <dir>\log.txt; rotated to log.old.txt at about 1 MB.
void Init(const std::wstring& dir);
void Write(const wchar_t* format, ...);

}  // namespace ib::log
