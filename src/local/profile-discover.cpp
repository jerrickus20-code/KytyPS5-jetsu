#include "profile-discover.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace Libs::Graphics::ProfileDiscover {
namespace {
std::mutex                  g_mutex;
std::unordered_set<uint64_t> g_seen;
std::atomic<uint32_t>       g_lines {0};
constexpr uint32_t          kMaxLines = 6000;

bool Seen(uint64_t key) {
	std::scoped_lock lock {g_mutex};
	return !g_seen.insert(key).second;
}

bool Room() {
	return g_lines.fetch_add(1, std::memory_order_relaxed) < kMaxLines;
}

std::string Images(std::span<const ImageInfo> images) {
	std::string text;
	char        item[160];
	for (const auto& image: images) {
		std::snprintf(item, sizeof(item), " [%u %s%s fmt %u %ux%u%s]", image.slot, image.written ? "W" : "R",
		              image.depth ? " depth" : "", image.format, image.width, image.height,
		              image.layers > 1 ? (" x" + std::to_string(image.layers)).c_str() : "");
		text += item;
	}
	return text;
}
std::string g_last_line;
uint32_t    g_last_count = 0;
uint32_t    g_timeline_lines = 0;
constexpr uint32_t kMaxTimelineLines = 1500;

void FlushTimeline() { // under g_mutex
	if (g_last_count == 0) return;
	std::printf("TL x%u %s\n", g_last_count, g_last_line.c_str());
	g_last_count = 0;
	if (++g_timeline_lines == kMaxTimelineLines) std::printf("TL (end: %u lines)\n", kMaxTimelineLines);
	std::fflush(stdout);
}
} // namespace

bool TimelineActive() {
	// KYTY_PROFILE_TIMELINE=<file>: the timeline starts when that file exists (checked twice a second), so it can be
	// begun when the game is in a scene.
	static const std::string file = [] {
		const char* value = std::getenv("KYTY_PROFILE_TIMELINE");
		return std::string {value != nullptr ? value : ""};
	}();
	if (file.empty()) return false;
	static std::atomic<bool> started {false};
	static std::atomic<int64_t> last_check {0};
	if (g_timeline_lines >= kMaxTimelineLines) return false;
	if (started.load(std::memory_order_relaxed)) return true;
	const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (now - last_check.load(std::memory_order_relaxed) < 500) return false;
	last_check.store(now, std::memory_order_relaxed);
	std::error_code ec;
	if (!std::filesystem::exists(file, ec)) return false;
	started.store(true, std::memory_order_relaxed);
	std::printf("TL start (%s)\n", file.c_str());
	return true;
}

void OnTimeline(const TimelineInfo& info, std::span<const ImageInfo> images) {
	char head[200];
	if (info.compute)
		std::snprintf(head, sizeof(head), "C 0x%016" PRIx64 " groups %ux%ux%u:", info.ps, info.groups_x, info.groups_y, info.groups_z);
	else
		std::snprintf(head, sizeof(head), "D vs 0x%016" PRIx64 " ps 0x%016" PRIx64 " -> %ux%u fmt %u colors %u depth %d:", info.vs,
		              info.ps, info.target_width, info.target_height, info.target_format, info.colors, info.depth);
	std::string line = head + Images(images);
	std::scoped_lock lock {g_mutex};
	if (line == g_last_line) { ++g_last_count; return; }
	FlushTimeline();
	g_last_line = std::move(line);
	g_last_count = 1;
}

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_PROFILE_DISCOVER");
		const char* timeline = std::getenv("KYTY_PROFILE_TIMELINE");
		const bool  on    = (value != nullptr && *value != 0 && std::strcmp(value, "0") != 0) || (timeline != nullptr && *timeline != 0);
		if (on) std::printf("PROFILE discovery on (KYTY_PROFILE_DISCOVER): shaders are logged once each\n");
		return on;
	}();
	return enabled;
}

void OnCompute(uint64_t shader_hash, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z, std::span<const ImageInfo> images) {
	// (A shader's first dispatch with images only; by size: the same shader may run at several.)
	if (images.empty() || Seen(shader_hash ^ 0x1ull)) return;
	if (!Room()) return;
	std::printf("PROFILE compute 0x%016" PRIx64 " groups %ux%ux%u:%s\n", shader_hash, groups_x, groups_y, groups_z,
	            Images(images).c_str());
	std::fflush(stdout);
}

void OnVertex(uint64_t shader_hash, std::span<const uint32_t> srt) {
	if (srt.size() < 16 || Seen(shader_hash ^ 0x2ull)) return;
	if (!Room()) return;
	std::string text;
	char        word[48];
	const size_t count = srt.size() < 96 ? srt.size() : 96;
	for (size_t i = 0; i < count; ++i) {
		float value;
		std::memcpy(&value, &srt[i], sizeof(value));
		if (std::isfinite(value) && std::fabs(value) > 1e-6f && std::fabs(value) < 1e7f)
			std::snprintf(word, sizeof(word), " %zu:%g", i, static_cast<double>(value));
		else
			std::snprintf(word, sizeof(word), " %zu:0x%x", i, srt[i]);
		text += word;
	}
	std::printf("PROFILE vertex 0x%016" PRIx64 " srt %zu words:%s\n", shader_hash, srt.size(), text.c_str());
	std::fflush(stdout);
}

void OnPixel(uint64_t shader_hash, uint32_t target_width, uint32_t target_height, std::span<const ImageInfo> images) {
	for (const auto& image: images) {
		if (image.depth || image.width == 0 || image.height == 0) continue;
		if (target_width < image.width * 3 / 2 || target_height < image.height * 3 / 2) continue;
		if (Seen(shader_hash ^ (uint64_t {image.slot} << 56u) ^ 0x3ull)) continue;
		if (!Room()) return;
		std::printf("PROFILE pixel 0x%016" PRIx64 " draws into %ux%u, slot %u samples %ux%u (fmt %u)\n", shader_hash,
		            target_width, target_height, image.slot, image.width, image.height, image.format);
		std::fflush(stdout);
	}
}

} // namespace Libs::Graphics::ProfileDiscover
