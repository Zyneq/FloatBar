#pragma once

#include <windows.h>

#include <string>

namespace ib {

// Asks for consent, then writes a redacted diagnostic report into the config
// folder, reveals it in Explorer and offers to open the GitHub issue page.
// Nothing is uploaded; the user reviews and attaches the file themselves.
void CreateDebugReport(HWND owner, const std::wstring& status);

}  // namespace ib
