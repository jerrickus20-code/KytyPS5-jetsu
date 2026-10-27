#ifndef EMULATOR_SRC_LOCAL_PROFILE_DISCOVER_H_
#define EMULATOR_SRC_LOCAL_PROFILE_DISCOVER_H_
// KYTY_PROFILE_DISCOVER=1: a log for writing a game's profile (local/game-profile.h). The renderer thread writes, once per
// shader, what the profile's fields are looked for in:
//   PROFILE compute 0x<hash>: its images (slot, sampled/written, format, size): the TAA pass has a color, a depth, a
//                             motion vector image (RG16 float) in and a color image of the same size out;
//   PROFILE vertex  0x<hash>: the first constants (flattened SRT words, hex and float): the camera's matrices;
//   PROFILE pixel   0x<hash>: draws into a target at least 1.5 times as large as an image they sample: the final composite.
// Lines go to the console and the log (send _dlss\run.log); at most a few thousand.
#include <cstdint>
#include <span>

namespace Libs::Graphics {
namespace ProfileDiscover {
[[nodiscard]] bool Enabled();
struct ImageInfo {
	uint32_t slot;
	bool     written, depth;
	uint32_t format, width, height, layers;
};
// A direct compute dispatch's shader and its images.
void OnCompute(uint64_t shader_hash, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z, std::span<const ImageInfo> images);
// A draw's first vertex shader and its constants.
void OnVertex(uint64_t shader_hash, std::span<const uint32_t> srt);
// KYTY_PROFILE_TIMELINE=<file>: from the moment that file exists, every dispatch and draw in order (about 1500 lines, equal neighbours
// counted): "TL xN C <hash> groups.. images" and "TL xN D vs ps -> WxH fmt, colors, depth, images", for finding a game's
// passes (a temporal anti-aliasing pass, the upscale to the output, the final composite).
struct TimelineInfo {
	bool     compute;
	uint64_t vs, ps;
	uint32_t groups_x, groups_y, groups_z;
	uint32_t target_width, target_height, target_format, colors;
	int      depth; // -1 none, 0 test only, 1 writes
};
[[nodiscard]] bool TimelineActive();
void OnTimeline(const TimelineInfo& info, std::span<const ImageInfo> images);
// A draw's pixel shader, the images it samples and its first target's size.
void OnPixel(uint64_t shader_hash, uint32_t target_width, uint32_t target_height, std::span<const ImageInfo> images);
} // namespace ProfileDiscover
} // namespace Libs::Graphics

#endif
