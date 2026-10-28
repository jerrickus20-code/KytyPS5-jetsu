#include "loader/astroFrameRate.h"

#include "common/logging/log.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace Loader::AstroFrameRate {

namespace {

// The engine's SetFrameRate(double fps) at 0x1b4b8c0 stores fps (double), 1000000 / fps (uint64, us) and 1 / fps (float,
// the frame time its game code reads through 0x1b4b950). Its bytes are checked before the three values are used.
constexpr uint64_t kSetFrameRate = 0x1b4b8c0;
constexpr uint8_t  kSetFrameRateCode[] = {
    0xc5, 0xfb, 0x10, 0x0d, 0x58, 0xbf, 0x48, 0x00, 0xc5, 0xfb, 0x10, 0x15, 0xa0, 0xc1, 0x48, 0x00, 0x48, 0xb9, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xc5, 0xfb, 0x11, 0x05, 0x96, 0x59, 0xbd, 0x00, 0xc5, 0xf3, 0x5e, 0xc8,
    0xc5, 0xf3, 0x5c, 0xda, 0xc4, 0xe1, 0xfb, 0x2c, 0xd1, 0xc4, 0xe1, 0xfb, 0x2c, 0xc3, 0x48, 0x31, 0xc1, 0xc5, 0xf9,
    0x2e, 0xca, 0xc5, 0xfb, 0x10, 0x15, 0xed, 0xbf, 0x48, 0x00, 0x48, 0x0f, 0x43, 0xd1, 0x48, 0x89, 0x15, 0x7a, 0x59,
    0xbd, 0x00, 0xc5, 0xeb, 0x5e, 0xc0, 0xc5, 0xfb, 0x5a, 0xc0, 0xc5, 0xfa, 0x11, 0x05, 0x62, 0x59, 0xbd, 0x00, 0xc3};
constexpr uint8_t kFrameTimeGetter[] = {0xc5, 0xfa, 0x10, 0x05, 0x28, 0x59, 0xbd, 0x00, 0xc3}; // 0x1b4b950
constexpr uint64_t kFps       = 0x2721278; // double
constexpr uint64_t kFrameTime = 0x2721280; // float, seconds
constexpr uint64_t kPeriodUs  = 0x2721288; // uint64, microseconds

std::atomic<uint64_t> g_base {0};

} // namespace

void Install(Program* main_program) {
	std::string title_id;
	std::string version;
	if (main_program == nullptr || main_program->base_vaddr == 0 ||
	    !SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id != "PPSA01325" ||
	    !SystemContentParamSfoGetString("APP_VER", &version)) {
		return;
	}
	const uint64_t base = main_program->base_vaddr;
	const bool     inside = kPeriodUs + 8 <= main_program->mapped_size;
	if (version != "01.905.000" || !inside ||
	    std::memcmp(reinterpret_cast<const void*>(base + kSetFrameRate), kSetFrameRateCode, sizeof(kSetFrameRateCode)) != 0 ||
	    std::memcmp(reinterpret_cast<const void*>(base + 0x1b4b950), kFrameTimeGetter, sizeof(kFrameTimeGetter)) != 0) {
		Log::WriteToConsoleAndLog(fmt::format("Astro frame rate: {} {} is not as expected; the 30 fps cap stays off "
		                                      "for this game\n", title_id, version));
		return;
	}
	g_base = base;
	Log::WriteToConsoleAndLog(std::string("Astro frame rate: ready (the F4 30 fps cap keeps the game at its normal speed)\n"));
}

bool Installed() {
	return g_base.load(std::memory_order_relaxed) != 0;
}

void Apply(int fps) {
	const uint64_t base = g_base.load(std::memory_order_relaxed);
	if (base == 0 || fps <= 0) return;
	auto* rate = reinterpret_cast<volatile double*>(base + kFps);
	if (*rate == static_cast<double>(fps)) return;
	// Only the engine's own two rates are replaced (not a rate a debug option set, 90 or 120).
	if (*rate != 30.0 && *rate != 60.0) {
		static std::atomic<bool> told {false};
		if (!told.exchange(true)) {
			Log::WriteToConsoleAndLog(fmt::format("Astro frame rate: the game's rate is {} fps; left as it is\n", *rate));
		}
		return;
	}
	*reinterpret_cast<volatile uint64_t*>(base + kPeriodUs) = 1000000u / static_cast<uint64_t>(fps);
	*reinterpret_cast<volatile float*>(base + kFrameTime)   = 1.0f / static_cast<float>(fps);
	*rate                                                    = static_cast<double>(fps);
	Log::WriteToConsoleAndLog(fmt::format("Astro frame rate: the game runs at {} fps\n", fps));
}

} // namespace Loader::AstroFrameRate
