#pragma once
// Host-OS helpers of the local frame generation code (frame-gen.cpp).

#include <string>

namespace LocalPlatform {

// Windows: the file at `path` carries a valid Authenticode signature of NVIDIA / AMD (the Streamline and FidelityFX
// runtimes are loaded only when they do). Elsewhere: false.
bool IsNvidiaSigned(const std::wstring& path);
bool IsAmdSigned(const std::wstring& path);

} // namespace LocalPlatform
