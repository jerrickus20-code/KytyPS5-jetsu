#include "loader/demonsSoulsRenderSize.h"

#include "graphics/host_gpu/hostMemory.h"
#include "loader/elf.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Loader::DemonsSoulsRenderSize {
namespace {

uint32_t g_applied = 0;

bool Readable(uint64_t address, uint64_t size) {
	return Libs::Graphics::HostMemoryRangeIsReadable(address, size);
}

bool HoldsPair(uint64_t address, uint32_t width, uint32_t height) {
	uint32_t pair[2] {};
	if (!Readable(address, sizeof(pair))) return false;
	std::memcpy(pair, reinterpret_cast<const void*>(address), sizeof(pair));
	return pair[0] == width && pair[1] == height;
}

// The one address of the performance mode's render size the mode cases (`pattern`) load (both cases load the same
// constant), after their 3840x2160 output.
std::optional<uint64_t> FindRenderConstant(const Program& program, std::string_view pattern) {
	std::vector<uint64_t> constants;
	for (const auto site: GuestCode::Find(program, GuestCode::Pattern(pattern), 64)) {
		int32_t output_displacement = 0, render_displacement = 0;
		std::memcpy(&output_displacement, reinterpret_cast<const void*>(site + 4), sizeof(int32_t));
		std::memcpy(&render_displacement, reinterpret_cast<const void*>(site + 12), sizeof(int32_t));
		const uint64_t output = site + 8 + static_cast<int64_t>(output_displacement);
		const uint64_t render = site + 16 + static_cast<int64_t>(render_displacement);
		if (HoldsPair(output, OutputWidth, OutputHeight) && HoldsPair(render, RenderWidth, RenderHeight) &&
		    std::find(constants.begin(), constants.end(), render) == constants.end())
			constants.push_back(render);
	}
	return constants.size() == 1 ? std::optional(constants[0]) : std::nullopt;
}

bool IsDemonsSouls() {
	std::string title;
	return SystemContentParamSfoGetString("TITLE_ID", &title) &&
	       (title == "PPSA01340" || title == "PPSA01341" || title == "PPSA01342");
}

} // namespace

std::optional<uint64_t> FindUniqueData(const Program& program, std::span<const uint8_t> bytes) {
	const auto* elf = program.elf.get();
	if (elf == nullptr || elf->GetEhdr() == nullptr || elf->GetPhdr() == nullptr) return std::nullopt;
	std::vector<uint64_t> found;
	const auto*           phdr = elf->GetPhdr();
	for (size_t i = 0; i < elf->GetEhdr()->e_phnum && found.size() < 2; i++) {
		if (phdr[i].p_type != PT_LOAD || phdr[i].p_filesz < bytes.size()) continue;
		const auto address = program.base_vaddr + phdr[i].p_vaddr;
		if (!Readable(address, phdr[i].p_filesz)) continue;
		const auto* begin = reinterpret_cast<const uint8_t*>(address);
		const auto* end   = begin + phdr[i].p_filesz;
		for (const auto* at = std::search(begin, end, bytes.begin(), bytes.end()); at != end && found.size() < 2;
		     at             = std::search(at + 1, end, bytes.begin(), bytes.end()))
			found.push_back(reinterpret_cast<uint64_t>(at));
	}
	return found.size() == 1 ? std::optional(found[0]) : std::nullopt;
}

uint32_t AppliedHeight() {
	return g_applied;
}

void Install(Program* program) {
	const char* setting = std::getenv("KYTY_RENDER_HEIGHT");
	if (setting == nullptr || *setting == 0 || std::strcmp(setting, "auto") == 0 || program == nullptr ||
	    program->elf == nullptr || program->elf->IsShared() || !IsDemonsSouls())
		return;
	const auto requested = static_cast<uint32_t>(std::strtoul(setting, nullptr, 10));
	if (requested == RenderHeight) return;
	if (requested < LeastRenderHeight || requested > RenderHeight) {
		std::printf("Demon's Souls render size: KYTY_RENDER_HEIGHT=%s is not %u-%u; the game's %ux%u kept\n", setting,
		            LeastRenderHeight, RenderHeight, RenderWidth, RenderHeight);
		return;
	}
	// The width a multiple of 8, 16:9 (1280x720 for 720).
	const uint32_t width  = static_cast<uint32_t>(std::lround(requested * 16.0 / 9.0 / 8.0)) * 8u;
	const uint32_t height = requested;
	auto           constant = FindRenderConstant(*program, ModeCase);
	if (!constant) constant = FindRenderConstant(*program, ModeCaseDuplicated);
	const auto widths  = FindUniqueData(*program, std::span(reinterpret_cast<const uint8_t*>(ModeWidths), sizeof(ModeWidths)));
	const auto heights = FindUniqueData(*program, std::span(reinterpret_cast<const uint8_t*>(ModeHeights), sizeof(ModeHeights)));
	if (!constant || !widths || !heights) {
		std::printf("Demon's Souls render size: the video settings of a known build not found; the game's %ux%u kept\n",
		            RenderWidth, RenderHeight);
		return;
	}
	// Installation occurs before module initializers or guest threads run; program memory is writable to the host.
	const uint32_t pair[2] {width, height};
	std::memcpy(reinterpret_cast<void*>(*constant), pair, sizeof(pair));
	std::memcpy(reinterpret_cast<void*>(*widths + PerformanceMode * sizeof(uint32_t)), &width, sizeof(uint32_t));
	std::memcpy(reinterpret_cast<void*>(*heights + PerformanceMode * sizeof(uint32_t)), &height, sizeof(uint32_t));
	g_applied = height;
	std::printf("Demon's Souls render size: performance mode renders %ux%u instead of %ux%u (output %ux%u)\n", width,
	            height, RenderWidth, RenderHeight, OutputWidth, OutputHeight);
}

} // namespace Loader::DemonsSoulsRenderSize
