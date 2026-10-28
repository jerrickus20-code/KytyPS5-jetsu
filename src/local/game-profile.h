#ifndef EMULATOR_SRC_LOCAL_GAME_PROFILE_H_
#define EMULATOR_SRC_LOCAL_GAME_PROFILE_H_
// Per-game profiles for DLSS frame generation, late DLSS and the render size (docs/DLSS.md, "Game profiles").
//
// What the emulator needs to know about a game, found once by looking at it (a frame capture, KYTY_PROFILE_DISCOVER's log,
// its code) and kept in a text file named after the title: profiles/<TITLE_ID>.txt next to the emulator (or in
// KYTY_PROFILE_DIR). Demon's Souls PPSA01340-2 are built in. Lines are `key = value`, `#` starts a comment; numbers
// are decimal or 0x hex, lists are separated by commas or spaces. A game without a profile gets none of these features
// (the cap and the overlay stay).
//
//   title                    = PPSA01341                       # informative
//   version                  = 01.007.000                      # informative; patches are checked by their patterns
//   taa_shaders              = 0xa44e580f4f0cfb2b, 0x934a...   # compute shader hashes of the TAA pass
//   taa_color / taa_depth / taa_motion / taa_output = 0 / 1 / 3 / 6   # that pass's texture slots
//   camera_shaders           = 0xf4d2..., ...                  # vertex shader hashes whose constants hold the camera
//   srt_view_proj            = 29   # 16 floats, row-major, clip = M * world, as the flattened constants' word index
//   srt_prev_view_proj       = 45   # 12 floats: the previous frame's rows x, y and w
//   srt_jitter               = 57   # 2 floats: the clip-space TAA offset (NDC units)
//   composite_shader         = 0x1ad8b5870fe4d3ec   # the pixel shader laying the interface over the render-size picture
//   composite_scene_slot     = 1                    # ... and the texture slot of that picture
//   direct_motion_target     = 4      # a game without a TAA pass: the G-buffer draw's color target (RG16F) holding the motion
//                                      # vectors; its depth target is the other input (late DLSS then needs no camera)
//   direct_jitter_buffer     = 1      # ... and the G-buffer vertex shader's constant buffer (V# index) holding its projection,
//   direct_jitter_offset     = 40     # whose jitter (2 floats, NDC: the projection's z-row x and y) is at this float index
//   no_frame_cap             = 1      # the game's speed follows its frame rate: the F4 30 fps cap is refused
//   render_aspect            = 16:9                 # the render size's aspect (width a multiple of 8)
//   patch_u32                = <pattern> | <offset> | width|height   # see RenderPatch
//   patch_pair               = <old width> <old height>              # see RenderPatch
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameProfile {

// One change that gives a game another render size, applied before the game runs when KYTY_RENDER_HEIGHT is set:
//  - U32: the one place in the game's code where `pattern` (hex bytes, "??" for bytes that vary between builds) starts;
//    the 32-bit little-endian value at `offset` bytes after it is set to the render width or height.
//  - Pair: the one place in the game's loaded segments where the 32-bit width `old_width` is followed by the 32-bit
//    height `old_height` (a table of resolutions); both are replaced by the new size.
struct RenderPatch {
	enum class Kind { U32, Pair } kind = Kind::U32;
	enum class Field { Width, Height } field = Field::Width;
	std::string pattern;
	uint32_t    offset = 0;
	uint32_t    old_width = 0, old_height = 0;
};

struct Profile {
	std::string title, version;
	std::vector<uint64_t> taa_shaders, camera_shaders;
	uint32_t taa_color = 0, taa_depth = 1, taa_motion = 3, taa_output = 6;
	uint32_t srt_view_proj = 0, srt_prev_view_proj = 0, srt_jitter = 0;
	uint64_t composite_shader     = 0;
	uint32_t composite_scene_slot = 1;
	// late_input_shader / late_input_slot: the picture late DLSS upscales is that pixel shader's texture (slot), e.g. a game's
	// own temporal upscaler's render-size input; DLSS's result still goes to the composite's scene slot. 0: the composite's own.
	uint64_t late_input_shader    = 0;
	uint32_t late_input_slot      = 0;
	uint32_t aspect_w = 16, aspect_h = 9;
	int      direct_motion_target = -1; // see the file format above; -1: none
	int      direct_jitter_buffer = -1, direct_jitter_offset = -1;
	bool     no_frame_cap         = false;
	std::vector<RenderPatch> patches;
	bool                     loaded = false; // a profile of this title was found

	[[nodiscard]] bool IsTaa(uint64_t hash) const;
	[[nodiscard]] bool IsCameraShader(uint64_t hash) const;
	// The camera can be read (camera shaders and their constants' places given).
	[[nodiscard]] bool HasCamera() const { return !camera_shaders.empty() && srt_view_proj != 0; }
	// The frame generation inputs can be found (the TAA pass given).
	[[nodiscard]] bool HasTaa() const { return !taa_shaders.empty(); }
};

// Parses a profile's text; on a syntax error returns false and says where in `error`.
bool Parse(std::string_view text, Profile* profile, std::string* error);

// The profile of the running game (its TITLE_ID from the game's metadata), loaded on first use: the built-in or
// profiles/<TITLE_ID>.txt. Always valid; `loaded` tells whether the title has one. Call after the game's metadata is loaded.
const Profile& Current();

// The render size for a height: the profile's aspect, the width a multiple of 8.
void RenderSizeFor(const Profile& profile, uint32_t height, uint32_t* width_out);

} // namespace GameProfile

#endif
