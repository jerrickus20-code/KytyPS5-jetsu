#include "game-profile.h"

#include "loader/systemContent.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace GameProfile {
namespace {

// Demon's Souls 01.007.000 and 01.005.000 (PPSA01340, PPSA01341, PPSA01342): the same shaders recompiled; the camera in the
// G-buffer vertex shaders' constants, the depth and motion vectors in the TAA dispatch's images (docs/RE-DEMONS-SOULS.md).
constexpr std::string_view kDemonsSouls = R"(
title = PPSA01341
version = 01.007.000, 01.005.000
taa_shaders = 0xa44e580f4f0cfb2b, 0x934a6e1197b99148
taa_color = 0
taa_depth = 1
taa_motion = 3
taa_output = 6
camera_shaders = 0xf4d2e78cd1769fc7, 0x08e16923875aa8e9, 0x0ffcbf08be26e74e, 0x1d69b5d27198446a, 0x5931e753b757f3bc, 0xa97f610824942162
srt_view_proj = 29
srt_prev_view_proj = 45
srt_jitter = 57
composite_shader = 0x1ad8b5870fe4d3ec
composite_scene_slot = 1
)";

std::string_view Trim(std::string_view text) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
	return text;
}

bool Number(std::string_view text, uint64_t* value) {
	text = Trim(text);
	if (text.empty()) return false;
	const std::string copy(text);
	char*             end = nullptr;
	*value                = std::strtoull(copy.c_str(), &end, 0);
	return end != nullptr && *end == 0;
}

std::vector<std::string_view> Split(std::string_view text, std::string_view separators) {
	std::vector<std::string_view> parts;
	while (!text.empty()) {
		const auto at = text.find_first_of(separators);
		const auto part = Trim(text.substr(0, at));
		if (!part.empty()) parts.push_back(part);
		if (at == std::string_view::npos) break;
		text.remove_prefix(at + 1);
	}
	return parts;
}

bool Hashes(std::string_view value, std::vector<uint64_t>* out) {
	for (const auto part: Split(value, ", \t")) {
		uint64_t hash = 0;
		if (!Number(part, &hash) || hash == 0) return false;
		out->push_back(hash);
	}
	return true;
}

} // namespace

bool Profile::IsTaa(uint64_t hash) const {
	return std::find(taa_shaders.begin(), taa_shaders.end(), hash) != taa_shaders.end();
}

bool Profile::IsCameraShader(uint64_t hash) const {
	return std::find(camera_shaders.begin(), camera_shaders.end(), hash) != camera_shaders.end();
}

bool Parse(std::string_view text, Profile* profile, std::string* error) {
	size_t line_number = 0;
	while (!text.empty()) {
		const auto newline = text.find('\n');
		auto       line    = text.substr(0, newline);
		text               = newline == std::string_view::npos ? std::string_view {} : text.substr(newline + 1);
		line_number++;
		if (const auto comment = line.find('#'); comment != std::string_view::npos) line = line.substr(0, comment);
		line = Trim(line);
		if (line.empty()) continue;
		const auto fail = [&](const char* what) {
			*error = "line " + std::to_string(line_number) + ": " + what;
			return false;
		};
		const auto equals = line.find('=');
		if (equals == std::string_view::npos) return fail("expected key = value");
		const auto key   = Trim(line.substr(0, equals));
		const auto value = Trim(line.substr(equals + 1));
		uint64_t   number = 0;
		const auto u32 = [&](uint32_t* out) {
			if (!Number(value, &number) || number > 0xffffffffull) return false;
			*out = static_cast<uint32_t>(number);
			return true;
		};
		if (key == "title") {
			profile->title = std::string(value);
		} else if (key == "version") {
			profile->version = std::string(value);
		} else if (key == "taa_shaders") {
			if (!Hashes(value, &profile->taa_shaders)) return fail("taa_shaders: hex hashes expected");
		} else if (key == "camera_shaders") {
			if (!Hashes(value, &profile->camera_shaders)) return fail("camera_shaders: hex hashes expected");
		} else if (key == "taa_color") {
			if (!u32(&profile->taa_color)) return fail("taa_color: number expected");
		} else if (key == "taa_depth") {
			if (!u32(&profile->taa_depth)) return fail("taa_depth: number expected");
		} else if (key == "taa_motion") {
			if (!u32(&profile->taa_motion)) return fail("taa_motion: number expected");
		} else if (key == "taa_output") {
			if (!u32(&profile->taa_output)) return fail("taa_output: number expected");
		} else if (key == "srt_view_proj") {
			if (!u32(&profile->srt_view_proj)) return fail("srt_view_proj: number expected");
		} else if (key == "srt_prev_view_proj") {
			if (!u32(&profile->srt_prev_view_proj)) return fail("srt_prev_view_proj: number expected");
		} else if (key == "srt_jitter") {
			if (!u32(&profile->srt_jitter)) return fail("srt_jitter: number expected");
		} else if (key == "composite_shader") {
			if (!Number(value, &profile->composite_shader)) return fail("composite_shader: hex hash expected");
		} else if (key == "late_input_shader") {
			if (!Number(value, &profile->late_input_shader)) return fail("late_input_shader: hex hash expected");
		} else if (key == "late_input_slot") {
			if (!u32(&profile->late_input_slot)) return fail("late_input_slot: number expected");
		} else if (key == "composite_scene_slot") {
			if (!u32(&profile->composite_scene_slot)) return fail("composite_scene_slot: number expected");
		} else if (key == "direct_motion_target") {
			uint32_t target = 0;
			if (!u32(&target) || target > 7) return fail("direct_motion_target: 0..7 expected");
			profile->direct_motion_target = static_cast<int>(target);
		} else if (key == "direct_jitter_buffer" || key == "direct_jitter_offset") {
			uint32_t index = 0;
			if (!u32(&index) || index > 4095) return fail("direct_jitter_buffer / direct_jitter_offset: number expected");
			(key == "direct_jitter_buffer" ? profile->direct_jitter_buffer : profile->direct_jitter_offset) = static_cast<int>(index);
		} else if (key == "no_frame_cap") {
			uint32_t on = 0;
			if (!u32(&on)) return fail("no_frame_cap: 0 or 1 expected");
			profile->no_frame_cap = on != 0;
		} else if (key == "render_aspect") {
			const auto colon = value.find(':');
			uint64_t   w = 0, h = 0;
			if (colon == std::string_view::npos || !Number(value.substr(0, colon), &w) ||
			    !Number(value.substr(colon + 1), &h) || w == 0 || h == 0 || w > 64 || h > 64)
				return fail("render_aspect: <w>:<h> expected");
			profile->aspect_w = static_cast<uint32_t>(w);
			profile->aspect_h = static_cast<uint32_t>(h);
		} else if (key == "patch_u32") {
			const auto parts = Split(value, "|");
			RenderPatch patch;
			uint64_t    offset = 0;
			if (parts.size() != 3 || !Number(parts[1], &offset) || offset > 4096 ||
			    (parts[2] != "width" && parts[2] != "height"))
				return fail("patch_u32: <pattern> | <offset> | width or height expected");
			patch.kind    = RenderPatch::Kind::U32;
			patch.pattern = std::string(parts[0]);
			patch.offset  = static_cast<uint32_t>(offset);
			patch.field   = parts[2] == "width" ? RenderPatch::Field::Width : RenderPatch::Field::Height;
			profile->patches.push_back(std::move(patch));
		} else if (key == "patch_pair") {
			const auto parts = Split(value, ", \t");
			RenderPatch patch;
			uint64_t    w = 0, h = 0;
			if (parts.size() != 2 || !Number(parts[0], &w) || !Number(parts[1], &h) || w == 0 || h == 0)
				return fail("patch_pair: <old width> <old height> expected");
			patch.kind       = RenderPatch::Kind::Pair;
			patch.old_width  = static_cast<uint32_t>(w);
			patch.old_height = static_cast<uint32_t>(h);
			profile->patches.push_back(std::move(patch));
		} else {
			return fail("unknown key");
		}
	}
	return true;
}

void RenderSizeFor(const Profile& profile, uint32_t height, uint32_t* width_out) {
	*width_out = static_cast<uint32_t>(std::lround(static_cast<double>(height) * profile.aspect_w / profile.aspect_h / 8.0)) * 8u;
}

const Profile& Current() {
	// Loaded once the game's metadata is there (no title: an empty profile, asked for again next time).
	static std::atomic<const Profile*> published {nullptr};
	static std::mutex                  mutex;
	static const Profile               none;
	if (const auto* ready = published.load(std::memory_order_acquire)) return *ready;
	std::scoped_lock lock {mutex};
	if (const auto* ready = published.load(std::memory_order_acquire)) return *ready;
	std::string title;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title) || title.empty()) return none;
	static Profile result;
	std::string    error;
	if (title == "PPSA01340" || title == "PPSA01341" || title == "PPSA01342") {
		if (Parse(kDemonsSouls, &result, &error)) result.loaded = true;
		published.store(&result, std::memory_order_release);
		return result;
	}
	std::vector<std::filesystem::path> folders;
	if (const char* dir = std::getenv("KYTY_PROFILE_DIR"); dir != nullptr && *dir != 0) folders.emplace_back(dir);
	folders.emplace_back("profiles");
	for (const auto& folder: folders) {
		const auto    path = folder / (title + ".txt");
		std::ifstream file(path);
		if (!file) continue;
		std::stringstream text;
		text << file.rdbuf();
		Profile parsed;
		if (!Parse(text.str(), &parsed, &error)) {
			std::printf("Game profile: %s: %s; no profile used\n", path.string().c_str(), error.c_str());
			break;
		}
		result        = std::move(parsed);
		result.loaded = true;
		if (result.title.empty()) result.title = title;
		std::printf("Game profile: %s: TAA %zu shader(s), camera %zu shader(s), %zu render size patch(es)\n",
		            path.string().c_str(), result.taa_shaders.size(), result.camera_shaders.size(), result.patches.size());
		published.store(&result, std::memory_order_release);
		return result;
	}
	if (error.empty())
		std::printf("Game profile: none for %s (profiles/%s.txt); DLSS frame generation, late DLSS and the render size "
		            "need one (docs/DLSS.md)\n",
		            title.c_str(), title.c_str());
	result.title = title;
	published.store(&result, std::memory_order_release);
	return result;
}

} // namespace GameProfile
