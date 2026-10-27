#include "loader/demonsSoulsRenderSize.h"

#include "local/game-profile.h"

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

// Another game's render size from its profile (local/game-profile.h): every patch's place is found first, and none is
// written unless all are (a half-changed resolution would break the game's targets).
void InstallProfile(Program& program, const char* setting) {
	const auto& profile = GameProfile::Current();
	if (!profile.loaded || profile.patches.empty()) {
		std::printf("Demon's Souls render size: KYTY_RENDER_HEIGHT=%s ignored: no game profile with render size patches "
		            "for this title (docs/DLSS.md, Game profiles)\n",
		            setting);
		return;
	}
	const auto height = static_cast<uint32_t>(std::strtoul(setting, nullptr, 10));
	if (height < LeastRenderHeight || height > OutputHeight) {
		std::printf("Game profile render size: KYTY_RENDER_HEIGHT=%s is not %u-%u; the game's own size kept\n", setting,
		            LeastRenderHeight, OutputHeight);
		return;
	}
	uint32_t width = 0;
	GameProfile::RenderSizeFor(profile, height, &width);
	struct Place {
		uint64_t address;
		uint32_t value, value2;
		bool     pair;
	};
	std::vector<Place> places;
	for (const auto& patch: profile.patches) {
		if (patch.kind == GameProfile::RenderPatch::Kind::U32) {
			const auto site = GuestCode::FindUnique(program, GuestCode::Pattern(patch.pattern));
			if (!site || !Readable(*site + patch.offset, sizeof(uint32_t))) {
				std::printf("Game profile render size: a patch's code (%s) of %s was not found, or not once; the game's own "
				            "size kept\n",
				            patch.pattern.c_str(), profile.title.c_str());
				return;
			}
			places.push_back({*site + patch.offset,
			                  patch.field == GameProfile::RenderPatch::Field::Width ? width : height, 0, false});
		} else {
			const uint32_t old[2] {patch.old_width, patch.old_height};
			const auto     site = FindUniqueData(program, std::span(reinterpret_cast<const uint8_t*>(old), sizeof(old)));
			if (!site) {
				std::printf("Game profile render size: the resolution table %ux%u of %s was not found, or not once; the "
				            "game's own size kept\n",
				            patch.old_width, patch.old_height, profile.title.c_str());
				return;
			}
			places.push_back({*site, width, height, true});
		}
	}
	for (const auto& place: places) {
		std::memcpy(reinterpret_cast<void*>(place.address), &place.value, sizeof(uint32_t));
		if (place.pair) std::memcpy(reinterpret_cast<void*>(place.address + sizeof(uint32_t)), &place.value2, sizeof(uint32_t));
	}
	g_applied = height;
	std::printf("Game profile render size: %s renders %ux%u (%zu patch(es))\n", profile.title.c_str(), width, height,
	            places.size());
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
	    program->elf == nullptr || program->elf->IsShared())
		return;
	if (!IsDemonsSouls()) {
		InstallProfile(*program, setting);
		return;
	}
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
