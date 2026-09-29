#pragma once

#include <string>

namespace fb {

// Text dump of the environment plus the raw UIA tree of every taskbar.
// With `includeNames` false, the Name property (window titles, tray tooltips
// such as Wi-Fi network names) is replaced by its length, for shareable reports.
std::wstring DumpTaskbarTrees(bool includeNames);

// Environment header only (OS build, DPI awareness, alignment, monitors).
std::wstring DescribeEnvironment();

}  // namespace fb
