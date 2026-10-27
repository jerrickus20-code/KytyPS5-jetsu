#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Loader {
struct Program;
// (Other games: their profile's render size patches, local/game-profile.h.)
// KYTY_RENDER_HEIGHT=<360..1440> (tools/dlss/run-dlss.ps1 -RenderHeight): Demon's Souls' performance mode - the PS5's
// default, its 3D rendered at 2560x1440 and upscaled by the game to a 3840x2160 output - renders its 3D at that height
// (16:9) instead. The output, the UI and what the game draws at output size stay 4K. 720 is the render size of the late
// DLSS (KYTY_LATE_DLSS: DLSS super resolution of the finished 1280x720 picture, local/frame-gen.h).
// Unset, "auto" or 1440: the game as it is. Only Demon's Souls 01.007.000 and 01.005.000's video settings are known;
// where they are not found the game's 2560x1440 is kept (a line in the log says so).
namespace DemonsSoulsRenderSize {
// The video settings' mode 3 case (the setter's and the constructor's): the output size, then the render size, as
// RIP-relative pairs of 32-bit width and height (vmovsd), then a short jump (01.007.000); 01.005.000 loads the same
// pairs with vmovddup.
inline constexpr std::string_view ModeCase           = "c5 fb 10 0d ?? ?? ?? ?? c5 fb 10 05 ?? ?? ?? ?? eb";
inline constexpr std::string_view ModeCaseDuplicated = "c5 fb 12 0d ?? ?? ?? ?? c5 fb 12 05 ?? ?? ?? ?? eb";
inline constexpr uint32_t         OutputWidth = 3840, OutputHeight = 2160, RenderWidth = 2560, RenderHeight = 1440;
// The render size query's per-mode tables (modes 1-5): widths, heights.
inline constexpr uint32_t ModeWidths[5] {1600, 1920, 2560, 3840, 2688};
inline constexpr uint32_t ModeHeights[5] {900, 1080, 1440, 2160, 1512};
inline constexpr int      PerformanceMode = 2; // (the index in the tables)
// The least render height: 360, a third of the 1080p output.
inline constexpr uint32_t LeastRenderHeight = 360;

// The one place `bytes` is in the program's loaded segments (a table of the video settings).
[[nodiscard]] std::optional<uint64_t> FindUniqueData(const Program& program, std::span<const uint8_t> bytes);
// Main program, before it runs (GamePatch::ApplyBuiltin's place).
void Install(Program* program);
// The render height KYTY_RENDER_HEIGHT asked for and Install applied, or 0.
[[nodiscard]] uint32_t AppliedHeight();
} // namespace DemonsSoulsRenderSize
} // namespace Loader
