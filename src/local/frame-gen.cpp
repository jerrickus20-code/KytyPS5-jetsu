#include "frame-gen.h"
#include "frame-gen-hudless.h"
#include "fsr-native.h"

#include "common/logging/log.h"
#include "local-platform.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "common/emulatorConfig.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#ifdef KYTY_FRAMEGEN_STREAMLINE
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
// NVIDIA Streamline SDK headers (_Build/deps/streamline/sdk/include, see CMakeLists.txt).
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_nis.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <cinttypes>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#ifdef KYTY_LOCAL_VULKAN_RECORDING
#include "vulkan-recording.h"
#endif
#endif

#ifdef KYTY_FRAMEGEN_FSR
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
// AMD FidelityFX SDK 1.1.4's API headers (_Build/deps/fidelityfx, see CMakeLists.txt; MIT); its runtime is
// amd_fidelityfx_vk.dll next to the emulator.
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include "gpu_blit_shaders/fsr_hud_mask_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"

#include <cfloat>
#include <chrono>
#include <cinttypes>
#include <utility>
#include <cstdio>
#include <filesystem>
#include <string>
#endif

#if defined(KYTY_FRAMEGEN_FSR) || defined(KYTY_FRAMEGEN_NGX)
// FSR's HUD-less image (RecordHudless): the Windows FSR backend's, and FSR 3 built in on Linux (fsr-native.h).
#define KYTY_FRAMEGEN_FSR_HUD 1
#include "gpu_blit_shaders/fsr_hud_mask_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"

#include <cinttypes>
#include <cstdio>
#endif

#ifdef KYTY_FRAMEGEN_NGX
// NVIDIA DLSS on Linux, where Streamline does not exist: NGX through the bridge libkyty_dlss.so (tools/local/dlss-bridge),
// loaded at run time, the only part that uses NVIDIA's SDK.
#include "kyty-dlss-bridge.h"

#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#endif

extern "C" {
// DLSS-G frames' Reflex sleep (slReflexSleep, every present): 0 skips it (KYTY_FRAMEGEN_SLEEP=0).
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_framegen_sleep {1};
}

namespace Libs::Graphics::FrameGen {

namespace {

struct Sources {
	ImageId  depth, motion;
	uint64_t depth_address = 0, motion_address = 0;
	bool     valid = false;
};

int      g_generate    = 0;     // frames to generate per rendered frame (0 = off)
bool     g_ready       = false; // Streamline loaded and the feature available
bool     g_fsr_native  = false; // Linux KYTY_FRAMEGEN_BACKEND=fsr: FSR 3 built in (fsr-native.h), shown by the presenter
bool     g_upscale     = false; // KYTY_UPSCALER=dlss: DLSS super resolution asked for (Streamline loaded for it)
bool     g_upscale_ready = false; // ... and available on the device
Camera   g_camera;              // Kyty.Gpu: the current guest frame
Sources  g_sources;             // Kyty.Gpu: the current guest frame
uint64_t g_guest_frame = 0;     // Kyty.Gpu
std::atomic<bool> g_scene {false}; // the current guest frame ran the TAA pass
std::atomic<bool> g_last_scene {false}; // ... the last one the guest flipped
std::atomic<uint64_t> g_flips_taken {0}; // TakeScene calls (guest flips)

float AsFloat(uint32_t word) {
	float value = 0.0f;
	std::memcpy(&value, &word, sizeof(value));
	return value;
}

bool IsDepthFormat(vk::Format format) {
	switch (format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32Sfloat:
		case vk::Format::eD32SfloatS8Uint:
		case vk::Format::eX8D24UnormPack32: return true;
		default: return false;
	}
}

} // namespace

void Transition(vk::CommandBuffer command, VulkanImage& image, vk::ImageAspectFlags aspect,
                vk::ImageLayout layout, vk::AccessFlags2 access, vk::PipelineStageFlags2 stage) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = image.state.pl_stage;
	barrier.srcAccessMask       = image.state.access_mask;
	barrier.dstStageMask        = stage;
	barrier.dstAccessMask       = access;
	barrier.oldLayout           = image.state.layout;
	barrier.newLayout           = layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image.image;
	barrier.subresourceRange    = {aspect, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
	image.state = {stage, access, layout};
}

// (Re)create `image` as a sampled copy target like `source` (of `size` when given: a blit target); with `attachment`, a
// color attachment too where the format allows it (*attachment tells).
void Configure(GraphicContext& graphics, VulkanImage& image, vk::ImageView& view,
               const VulkanImage& source, bool* attachment, vk::Extent2D size, bool* failed) {
	if (size.width == 0 || size.height == 0) size = {source.extent.width, source.extent.height};
	if (image.image != nullptr && image.format == source.format &&
	    image.extent.width == size.width && image.extent.height == size.height) {
		return;
	}
	if (view != nullptr) {
		graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
	}
	if (image.image != nullptr) {
		graphics.DeleteImage(image);
	}
	vk::ImageCreateInfo create {};
	create.imageType     = vk::ImageType::e2D;
	create.extent        = vk::Extent3D {size.width, size.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = source.format;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage         = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
	if (attachment != nullptr) {
		*attachment = !IsDepthFormat(source.format) &&
		              static_cast<bool>(graphics.physical_device.getFormatProperties(source.format).optimalTilingFeatures &
		                                vk::FormatFeatureFlagBits::eColorAttachment);
		if (*attachment) {
			create.usage |= vk::ImageUsageFlagBits::eColorAttachment;
		}
	}
	create.sharingMode   = vk::SharingMode::eExclusive;
	create.samples       = vk::SampleCountFlagBits::e1;
	if (!graphics.CreateImage(create, image)) {
		// (`failed`: an image the frame can do without, the HUD canvas's copy: none this time.)
		if (failed != nullptr) {
			*failed = true;
			return;
		}
		EXIT("frame generation: cannot allocate a %ux%u input image\n", size.width, size.height);
	}
	vk::ImageViewCreateInfo view_info {};
	view_info.image            = image.image;
	view_info.viewType         = vk::ImageViewType::e2D;
	view_info.format           = source.format;
	view_info.subresourceRange = {IsDepthFormat(source.format) ? vk::ImageAspectFlagBits::eDepth
	                                                           : vk::ImageAspectFlagBits::eColor,
	                              0, 1, 0, 1};
	RequireVulkanSuccess(graphics.device.createImageView(&view_info, nullptr, &view),
	                     "frame generation input view");
}

namespace {

#ifdef KYTY_FRAMEGEN_FSR_HUD
// `source` scaled into `target` (a color image; filtered).
void BlitInto(vk::CommandBuffer command, Image& source, VulkanImage& target) {
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {}, command);
	Transition(command, target, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferDstOptimal,
	           vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eTransfer);
	vk::ImageBlit blit {};
	blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	blit.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(source.backing.extent.width),
	                                    static_cast<int32_t>(source.backing.extent.height), 1};
	blit.dstOffsets[1]  = vk::Offset3D {static_cast<int32_t>(target.extent.width), static_cast<int32_t>(target.extent.height), 1};
	command.blitImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, target.image,
	                  vk::ImageLayout::eTransferDstOptimal, 1, &blit, vk::Filter::eLinear);
	Transition(command, target, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal,
	           vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eAllCommands);
}
#endif

void CopyInto(vk::CommandBuffer command, Image& source, VulkanImage& target) {
	const auto aspect = IsDepthFormat(source.backing.format) ? vk::ImageAspectFlagBits::eDepth
	                                                         : vk::ImageAspectFlagBits::eColor;
	source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
	               command);
	Transition(command, target, aspect, vk::ImageLayout::eTransferDstOptimal,
	           vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eTransfer);
	vk::ImageCopy copy {};
	copy.srcSubresource = {aspect, 0, 0, 1};
	copy.dstSubresource = {aspect, 0, 0, 1};
	copy.extent         = target.extent;
	command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal, target.image,
	                  vk::ImageLayout::eTransferDstOptimal, copy);
	Transition(command, target, aspect, vk::ImageLayout::eShaderReadOnlyOptimal,
	           vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eAllCommands);
}

// KYTY_FPS_HUD: frames shown per presented frame, and the panel's normalized rectangle (16 bits
// per coordinate: x0, y0, x1, y1; 0 = none).
std::atomic<uint32_t> g_presented_frames {1};
std::atomic<uint64_t> g_static_rect {0};

// No motion under the KYTY_FPS_HUD panel: DLSS-G would move it with the scene behind it.
void ZeroStaticRect(vk::CommandBuffer command, VulkanImage& motion, vk::ImageView view) {
	const auto packed = g_static_rect.load(std::memory_order_relaxed);
	if (packed == 0) {
		return;
	}
	const auto coordinate = [&](uint32_t index, uint32_t size) {
		return static_cast<int32_t>(((packed >> (16u * index)) & 0xffffu) * size / 0xffffu);
	};
	const int32_t x0 = coordinate(0, motion.extent.width), y0 = coordinate(1, motion.extent.height);
	const int32_t x1 = coordinate(2, motion.extent.width), y1 = coordinate(3, motion.extent.height);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	Transition(command, motion, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eColorAttachmentOptimal,
	           vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eColorAttachmentOutput);
	vk::RenderingAttachmentInfo color {};
	color.imageView   = view;
	color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	color.loadOp      = vk::AttachmentLoadOp::eLoad;
	color.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = vk::Extent2D {motion.extent.width, motion.extent.height};
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &color;
	command.beginRendering(rendering);
	vk::ClearAttachment clear {};
	clear.aspectMask       = vk::ImageAspectFlagBits::eColor;
	clear.colorAttachment  = 0;
	clear.clearValue.color = vk::ClearColorValue {std::array<float, 4> {0.0f, 0.0f, 0.0f, 0.0f}};
	vk::ClearRect rect {};
	rect.rect       = vk::Rect2D {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
	rect.layerCount = 1;
	command.clearAttachments(1, &clear, 1, &rect);
	command.endRendering();
	Transition(command, motion, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal,
	           vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eAllCommands);
}

#ifdef KYTY_FRAMEGEN_STREAMLINE
// Streamline entry points (sl.interposer.dll, loaded at run time) and feature functions.
struct Api {
	PFun_slInit*                 init                = nullptr;
	PFun_slIsFeatureSupported*   is_feature_supported = nullptr;
	PFun_slIsFeatureLoaded*      is_feature_loaded   = nullptr;
	PFun_slGetFeatureFunction*   get_feature_function = nullptr;
	PFun_slGetNewFrameToken*     get_new_frame_token = nullptr;
	PFun_slSetConstants*         set_constants       = nullptr;
	PFun_slSetTagForFrame*       set_tag_for_frame   = nullptr;
	PFun_slDLSSGSetOptions*      dlssg_set_options   = nullptr;
	PFun_slDLSSGGetState*        dlssg_get_state     = nullptr;
	PFun_slReflexSetOptions*     reflex_set_options  = nullptr;
	PFun_slPCLSetMarker*         pcl_set_marker      = nullptr;
	PFun_slReflexSleep*          reflex_sleep        = nullptr;
	PFun_slEvaluateFeature*      evaluate_feature    = nullptr;
	PFun_slDLSSSetOptions*       dlss_set_options    = nullptr;
	PFun_slDLSSGetOptimalSettings* dlss_get_optimal_settings = nullptr;
	PFun_slNISSetOptions*        nis_set_options     = nullptr;
	PFun_slFreeResources*        free_resources      = nullptr; // optional
};
Api              g_api;
// slGetNewFrameToken is not thread safe (Streamline's programming guide), and the present thread, the renderer thread
// (DLAA, late DLSS, NIS) and, with Reflex, a game thread all call it: one at a time.
std::mutex       g_token_mutex;
sl::Result NewFrameToken(sl::FrameToken*& token, uint32_t* index) {
	std::lock_guard<std::mutex> lock(g_token_mutex);
	return g_api.get_new_frame_token(token, index);
}
sl::FrameToken*  g_token = nullptr; // present thread: the frame being presented
uint32_t         g_frame_index = 0;
bool             g_options_set = false;
vk::Extent2D     g_options_backbuffer {};
vk::Extent2D     g_options_render {};
vk::Format       g_options_format = vk::Format::eUndefined; // SDR or HDR10 swapchain
uint64_t         g_presents = 0;
// DLSS-G on (generating) or off with its resources kept (eRetainResourcesWhenOff): off after TurnOffAfter presents in a
// row without inputs (menus, movies, loading), on after TurnOnAfter with them, so a fade or a cut does not flap it. On
// with null tags it was asked to generate frames it had no inputs for (Streamline "Failed to find global tag").
constexpr uint32_t TurnOffAfter = 30, TurnOnAfter = 10;
bool               g_mode_on    = false;
uint32_t           g_run_with   = 0, g_run_without = 0; // presents in a row with inputs, without them
// A frame with inputs the presenter left unshown (NoteDroppedFrame): the next one's motion spans two game frames.
std::atomic<bool>  g_discontinuity {false};
// KYTY_DLSS / KYTY_NIS (Initialize: requested; OnDevice: available).
bool                  g_dlss_requested = false, g_dlss_ready = false;
uint32_t              g_nis_height = 0;
bool                  g_nis_ready  = false;
// KYTY_LATE_DLSS=<height> (an experiment): DLSS super resolution of the finished picture (after the game's bloom, depth
// of field and the rest, at the render size) to <height> lines, in NIS's place before the final composite.
uint32_t              g_late_height = 0;
bool                  g_late_ready  = false;
bool                  g_late_aa     = false; // KYTY_LATE_DLSS_AA=1: no TAA before it; the jitter kept for DLSS
// Renderer thread: late DLSS's failed evaluations in a row (LateFailureLimit: given up; one that succeeds starts the count
// again - a run of 8 failures spread over 11 minutes had ended it for good, 10-26), and the TAA passes copied through since
// it last made a picture (LateUnresolvedLimit: the game's TAA again, rather than a jittered, aliased picture from then on).
constexpr uint32_t    LateFailureLimit = 60;
uint32_t              g_late_failures   = 0;
uint32_t              g_late_unresolved = 0;
constexpr uint32_t    LateUnresolvedLimit = 120;
// The full-size TAA pass's depth and motion vectors (OnLateDispatch: indoors the game runs the TAA shader again on a
// quarter-size lighting buffer, whose images g_sources may hold last).
struct LateSources {
	ImageId      depth, motion;
	uint64_t     depth_address = 0, motion_address = 0;
	vk::Extent2D extent {};
};
LateSources g_late_sources;
vk::Extent2D g_late_largest {};
// KYTY_LIVE_FILE switches (LiveCommand): any thread writes, the renderer thread reads.
std::atomic<bool>     g_dlss_off {false}, g_nis_off {false};
std::atomic<bool>     g_dlss_options_dirty {false}, g_nis_options_dirty {false};
std::atomic<int>      g_dlss_auto_exposure {1}, g_dlss_hdr {1};
// DLSS alpha upscaling (KYTY_DLSS_ALPHA=1 or live `dlssalpha on`; default off: the dark rooms were the exposure).
std::atomic<int>      g_dlss_alpha {-1};
// Renderer thread: the DLSS instance's viewport. Streamline applies DLSS options only when it creates the instance, so a
// live option change makes a new one (viewport 3, 4, ...); the old one is freed a few frames later.
uint32_t              g_dlss_viewport_id = 11, g_dlss_free_id = 0; // (the super resolution path's DLSS is viewport 1)
// The exposure pass after DLSS (live `dlssexposure on|off`, `taaalpha keep|<value>`, `exposurescale <x>`).
std::atomic<bool>     g_exposure_on {true};
std::atomic<int>      g_taa_alpha_mode {0};
std::atomic<float>    g_taa_alpha_value {4.0f};
std::atomic<float>    g_exposure_scale {1.0f};
uint64_t              g_dlss_evaluations = 0, g_dlss_free_at = 0;
std::atomic<int>      g_nis_hdr {-1};           // -1: KYTY_NIS_HDR (default none)
std::atomic<int>      g_nis_sharpness_x100 {-1}; // -1: KYTY_NIS_SHARPNESS (default 0.30)
// The index of the frame the present thread presents last: the renderer thread's Streamline calls use its token
// (the same index: the same token object; viewports 1 and 2 keep their constants and tags apart from DLSS-G's 0).
std::atomic<uint32_t> g_present_index {0};

// Matrices in Streamline's convention: row-major, row vectors (p' = p * M).
using Mat4 = sl::float4x4;

Mat4 Transposed(const std::array<float, 16>& m) { // our column-vector matrix -> SL
	Mat4 r;
	for (uint32_t i = 0; i < 4; ++i) {
		r[i] = sl::float4(m[i], m[4 + i], m[8 + i], m[12 + i]);
	}
	return r;
}

float At(const sl::float4& v, uint32_t i) {
	return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
	Mat4 r;
	for (uint32_t i = 0; i < 4; ++i) {
		float row[4] {};
		for (uint32_t j = 0; j < 4; ++j) {
			for (uint32_t k = 0; k < 4; ++k) row[j] += At(a[i], k) * At(b[k], j);
		}
		r[i] = sl::float4(row[0], row[1], row[2], row[3]);
	}
	return r;
}

bool Inverted(const Mat4& m, Mat4& out) {
	double a[4][8] {};
	for (uint32_t i = 0; i < 4; ++i) {
		for (uint32_t j = 0; j < 4; ++j) a[i][j] = At(m[i], j);
		a[i][4 + i] = 1.0;
	}
	for (uint32_t c = 0; c < 4; ++c) {
		uint32_t pivot = c;
		for (uint32_t r = c + 1; r < 4; ++r) {
			if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
		}
		if (std::fabs(a[pivot][c]) < 1e-12) return false;
		for (uint32_t j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
		const double scale = 1.0 / a[c][c];
		for (uint32_t j = 0; j < 8; ++j) a[c][j] *= scale;
		for (uint32_t r = 0; r < 4; ++r) {
			if (r == c) continue;
			const double f = a[r][c];
			for (uint32_t j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
		}
	}
	for (uint32_t i = 0; i < 4; ++i) {
		out[i] = sl::float4(static_cast<float>(a[i][4]), static_cast<float>(a[i][5]),
		                    static_cast<float>(a[i][6]), static_cast<float>(a[i][7]));
	}
	return true;
}

// The game's world->clip matrix is P * V with a perspective P whose w row is (0, 0, 1, 0) (w =
// view depth) and whose z row is constant (reversed-Z, infinite far). Split it into camera
// axes, position and projection terms.
struct CameraFrame {
	sl::float3 right, up, forward, position;
	float      sx = 1.0f, sy = 1.0f, cx = 0.0f, cy = 0.0f, z_near = 0.1f;
	bool       valid = false;
};

CameraFrame Decompose(const std::array<float, 16>& m) {
	CameraFrame c;
	const auto row = [&](uint32_t i) { return std::array<double, 4> {m[i * 4], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]}; };
	const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
	const double flen = std::sqrt(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
	if (flen < 1e-6) return c;
	const double f[3] {r3[0] / flen, r3[1] / flen, r3[2] / flen};
	const double tf = r3[3] / flen;
	const auto split = [&](const std::array<double, 4>& r, double& s, double& off, double axis[3], double& t) {
		off = (r[0] * f[0] + r[1] * f[1] + r[2] * f[2]) / flen;
		double v[3] {r[0] / flen - off * f[0], r[1] / flen - off * f[1], r[2] / flen - off * f[2]};
		s = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (s < 1e-9) return false;
		for (int i = 0; i < 3; ++i) axis[i] = v[i] / s;
		t = (r[3] / flen - off * tf) / s;
		return true;
	};
	double sx = 0, cx = 0, sy = 0, cy = 0, x[3] {}, y[3] {}, tx = 0, ty = 0;
	if (!split(r0, sx, cx, x, tx) || !split(r1, sy, cy, y, ty)) return c;
	// Up = forward x right (Streamline's convention); a flipped clip y shows up as a negative sy.
	const double up[3] {f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0]};
	if (up[0] * y[0] + up[1] * y[1] + up[2] * y[2] < 0) {
		sy = -sy;
		ty = -ty;
	}
	c.right    = sl::float3(float(x[0]), float(x[1]), float(x[2]));
	c.up       = sl::float3(float(up[0]), float(up[1]), float(up[2]));
	c.forward  = sl::float3(float(f[0]), float(f[1]), float(f[2]));
	// View translation (tx, ty, tf) = -axes . position.
	const double pos[3] {-(tx * x[0] + ty * up[0] + tf * f[0]), -(tx * x[1] + ty * up[1] + tf * f[1]),
	                     -(tx * x[2] + ty * up[2] + tf * f[2])};
	c.position = sl::float3(float(pos[0]), float(pos[1]), float(pos[2]));
	c.sx       = float(sx);
	c.sy       = float(sy);
	c.cx       = float(cx);
	c.cy       = float(cy);
	c.z_near   = float(r2[3] / flen);
	c.valid    = true;
	return c;
}

// DLSS evaluations NGX refused (Streamline's slEvaluateFeature says eOk all the same; its log has them): its output
// image is then never written, the game black (issue #87).
std::atomic<uint32_t> g_dlss_refusals {0};

void LogMessage(sl::LogType type, const char* message) {
	LOGF("Streamline[%d]: %s", static_cast<int>(type), message);
	if (type == sl::LogType::eError && message != nullptr && std::strstr(message, "[sl.dlss]") != nullptr &&
	    std::strstr(message, "evaluate feature failed") != nullptr)
		g_dlss_refusals.fetch_add(1, std::memory_order_relaxed);
	// KYTY_FRAMEGEN_VERBOSE: every message on stdout (the run log), not only errors.
	static const bool verbose = std::getenv("KYTY_FRAMEGEN_VERBOSE") != nullptr;
	if (type != sl::LogType::eError && !verbose) return;
	// The same message again and again (e.g. a frame's constants set twice): the first one, then a count every
	// 1000, so the console (slow to print to) stays quiet. The key is the text after the [time][tid] prefix.
	std::string_view key = message != nullptr ? message : "";
	while (!key.empty() && key.front() == '[') {
		const auto close = key.find(']');
		if (close == std::string_view::npos) break;
		key.remove_prefix(close + 1);
	}
	static std::mutex                                mutex;
	static std::unordered_map<std::string, uint64_t> seen;
	uint64_t                                         count = 0;
	{
		std::lock_guard<std::mutex> lock(mutex);
		count = ++seen[std::string(key)];
	}
	if (count == 1) {
		std::printf("Streamline %s: %s", type == sl::LogType::eError ? "error" : "log", message);
	} else if (count % 1000 == 0) {
		std::printf("Streamline %s (%llu times so far, repeats not shown): %.*s", type == sl::LogType::eError ? "error" : "log",
		            static_cast<unsigned long long>(count), static_cast<int>(key.size()), key.data());
	} else {
		return;
	}
	std::fflush(stdout);
}

template <typename F>
bool FeatureFunction(sl::Feature feature, const char* name, F*& function) {
	void* pointer = nullptr;
	if (g_api.get_feature_function(feature, name, pointer) != sl::Result::eOk || pointer == nullptr) {
		LOGF("frame generation: %s is unavailable\n", name);
		return false;
	}
	function = reinterpret_cast<F*>(pointer);
	return true;
}

// KYTY_FG_HUDLESS: the frame's image without its HUD, laid out as the backbuffer (`game`: where that shows the game
// image; frame-gen-hudless.h), or none for a frame without one (DLSS-G then takes the backbuffer as it is).
void TagHudless(sl::FrameToken& token, const FrameInputs& inputs, const sl::Extent& game) {
	const auto&  image = inputs.hudless.display;
	sl::Resource hudless(sl::ResourceType::eTex2d, static_cast<VkImage>(image.image), nullptr,
	                     static_cast<VkImageView>(inputs.hudless.display_view),
	                     static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	hudless.width        = image.extent.width;
	hudless.height       = image.extent.height;
	hudless.nativeFormat = static_cast<uint32_t>(image.format);
	hudless.mipLevels    = 1;
	hudless.arrayLayers  = 1;
	hudless.flags        = 0;
	hudless.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(image.usage));
	const sl::ResourceTag tag(inputs.hudless.has_display ? &hudless : nullptr, sl::kBufferTypeHUDLessColor,
	                          sl::ResourceLifecycle::eValidUntilPresent, &game);
	if (const auto result = g_api.set_tag_for_frame(token, sl::ViewportHandle {0}, &tag, 1, nullptr);
	    result != sl::Result::eOk && g_presents < 8) {
		LOGF("frame generation: slSetTagForFrame (HUD-less) failed (%d)\n", static_cast<int>(result));
	}
}
#endif

// KYTY_FRAMEGEN_BACKEND=fsr (UsesFsr): FSR 3.1 in place of Streamline.
bool g_fsr = false; // asked for and its runtime loaded
#ifdef KYTY_FRAMEGEN_FSR
struct FsrState {
	ffxFunctions api {};
	ffxContext   swapchain_context = nullptr; // the frame generation swapchain
	ffxContext   fg_context        = nullptr; // frame generation (made at the first frame with inputs)
	ffxQueryDescSwapchainReplacementFunctionsVK functions {};
	vk::SwapchainKHR swapchain = nullptr;
	vk::Extent2D     display {};
	vk::Format       format = vk::Format::eUndefined;
	bool             hdr    = false; // HDR10 (PQ) swapchain
	vk::Extent2D     max_render {};  // fg_context's
	uint64_t         frame_id = 0;
	bool             mode_on  = false;
	bool             reset    = false; // the next generated frame starts a new history
	uint32_t         run_with = 0, run_without = 0; // presents in a row with inputs, without them
	std::chrono::steady_clock::time_point last_frame {};
	GraphicContext*  graphics = nullptr;
};
FsrState g_fsr_state;
// FSR: generation on after FsrTurnOnAfter frames with inputs in a row; off after FsrTurnOffAfter without them (those
// before are not presented: a frame without inputs between generated ones would be shown as a still).
constexpr uint32_t FsrTurnOnAfter = 10, FsrTurnOffAfter = 4;
std::atomic<bool>  g_fsr_discontinuity {false};

bool FsrOk(ffxReturnCode_t code, const char* what) {
	if (code == FFX_API_RETURN_OK) return true;
	LOGF("frame generation: FSR %s failed (%u)\n", what, static_cast<uint32_t>(code));
	std::printf("Frame generation: FSR %s failed (%u)\n", what, static_cast<uint32_t>(code));
	std::fflush(stdout);
	return false;
}

void FsrMessage(uint32_t type, const wchar_t* message) {
	char text[1024] {};
	WideCharToMultiByte(CP_UTF8, 0, message, -1, text, static_cast<int>(sizeof(text)) - 1, nullptr, nullptr);
	LOGF("FSR[%u]: %s\n", type, text);
	if (std::getenv("KYTY_FRAMEGEN_VERBOSE") != nullptr || type == FFX_API_MESSAGE_TYPE_ERROR) {
		std::printf("FSR %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
		std::fflush(stdout);
	}
}

FfxApiResource FsrImage(const VulkanImage& image) {
	VkImageCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType   = VK_IMAGE_TYPE_2D;
	info.format      = static_cast<VkFormat>(image.format);
	info.extent      = {image.extent.width, image.extent.height, 1};
	info.mipLevels   = 1;
	info.arrayLayers = 1;
	info.usage       = static_cast<VkImageUsageFlags>(image.usage);
	return ffxApiGetResourceVK(static_cast<VkImage>(image.image), ffxApiGetImageResourceDescriptionVK(static_cast<VkImage>(image.image), info, 0),
	                           FFX_API_RESOURCE_STATE_COMPUTE_READ);
}

// The game's world->clip matrix split as in Decompose (Streamline's): the camera's axes and position in world space,
// the vertical scale of its projection and its near plane (reversed-Z, infinite far).
struct FsrCamera {
	float position[3] {}, up[3] {}, right[3] {}, forward[3] {};
	float sy = 1.0f, z_near = 0.1f;
	bool  valid = false;
};

FsrCamera FsrDecompose(const std::array<float, 16>& m) {
	FsrCamera  c;
	const auto row = [&](uint32_t i) { return std::array<double, 4> {m[i * 4], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]}; };
	const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
	const double flen = std::sqrt(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
	if (flen < 1e-6) return c;
	const double f[3] {r3[0] / flen, r3[1] / flen, r3[2] / flen};
	const double tf = r3[3] / flen;
	const auto split = [&](const std::array<double, 4>& r, double& s, double axis[3], double& t) {
		const double off = (r[0] * f[0] + r[1] * f[1] + r[2] * f[2]) / flen;
		double v[3] {r[0] / flen - off * f[0], r[1] / flen - off * f[1], r[2] / flen - off * f[2]};
		s = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (s < 1e-9) return false;
		for (int i = 0; i < 3; ++i) axis[i] = v[i] / s;
		t = (r[3] / flen - off * tf) / s;
		return true;
	};
	double sx = 0, sy = 0, x[3] {}, y[3] {}, tx = 0, ty = 0;
	if (!split(r0, sx, x, tx) || !split(r1, sy, y, ty)) return c;
	const double up[3] {f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0]};
	if (up[0] * y[0] + up[1] * y[1] + up[2] * y[2] < 0) {
		sy = -sy;
		ty = -ty;
	}
	for (int i = 0; i < 3; ++i) {
		c.right[i]    = float(x[i]);
		c.up[i]       = float(up[i]);
		c.forward[i]  = float(f[i]);
		c.position[i] = float(-(tx * x[i] + ty * up[i] + tf * f[i]));
	}
	c.sy     = float(sy);
	c.z_near = float(r2[3] / flen);
	c.valid  = true;
	return c;
}

void FsrDestroyFrameGeneration() {
	auto& s = g_fsr_state;
	if (s.fg_context == nullptr) return;
	if (s.swapchain != nullptr) {
		// (Off before its context goes: the swapchain then presents the frames as they are.)
		ffxConfigureDescFrameGeneration config {};
		config.header.type            = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
		config.swapChain              = static_cast<VkSwapchainKHR>(s.swapchain);
		config.frameGenerationEnabled = false;
		config.frameID                = ++s.frame_id;
		(void)s.api.Configure(&s.fg_context, &config.header);
	}
	(void)s.api.DestroyContext(&s.fg_context, nullptr);
	s.fg_context = nullptr;
	s.mode_on    = false;
}

// The frame generation context for render sizes up to `render` (made again for a larger one).
bool FsrEnsureFrameGeneration(vk::Extent2D render) {
	auto& s = g_fsr_state;
	if (s.fg_context != nullptr && render.width <= s.max_render.width && render.height <= s.max_render.height) return true;
	FsrDestroyFrameGeneration();
	ffxCreateBackendVKDesc backend {};
	backend.header.type      = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
	backend.vkDevice         = static_cast<VkDevice>(s.graphics->device);
	backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(s.graphics->physical_device);
	backend.vkDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
	ffxCreateContextDescFrameGeneration create {};
	create.header.type  = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
	create.header.pNext = &backend.header;
	// The game's depth: reversed-Z with an infinite far plane (Decompose); its vectors at the render size.
	create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
	if (std::getenv("KYTY_FRAMEGEN_DEBUG") != nullptr) create.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
	create.displaySize      = {s.display.width, s.display.height};
	s.max_render            = {std::max(render.width, s.display.width), std::max(render.height, s.display.height)};
	create.maxRenderSize    = {s.max_render.width, s.max_render.height};
	create.backBufferFormat = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(s.format));
	if (!FsrOk(s.api.CreateContext(&s.fg_context, &create.header, nullptr), "frame generation context")) {
		s.fg_context = nullptr;
		return false;
	}
	LOGF("frame generation: FSR context %ux%u, render up to %ux%u\n", s.display.width, s.display.height,
	     s.max_render.width, s.max_render.height);
	std::printf("Frame generation: FSR 3.1 ready (%ux%u, render up to %ux%u)\n", s.display.width, s.display.height,
	            s.max_render.width, s.max_render.height);
	std::fflush(stdout);
	return true;
}

#endif

#ifdef KYTY_FRAMEGEN_FSR_HUD
// The game's interface canvas (NoteUiCanvas: its guest address; the GPU thread's): 3840x2160, the HD output's
// 1920x1080, or KYTY_OUTPUT_HEIGHT's output where the interface is scaled to it (loader/demonsSoulsHdOutput.h).
std::atomic<uint64_t> g_ui_canvas {0};

vk::Extent2D UiCanvasExtent() {
	namespace Hd = ::Loader::DemonsSoulsHdOutput;
	if (Hd::InterfaceScaled() || Hd::InterfaceAtOutput()) return vk::Extent2D {Hd::UpscaledWidth(), Hd::UpscaledHeight()};
	return Hd::Active() ? vk::Extent2D {Hd::HdWidth, Hd::HdHeight} : vk::Extent2D {3840, 2160};
}

// FSR's HUD-less images (RecordHudless): the backbuffer with the HUD painted over by the scene around it, two in turn
// (the interpolation of one may still read it while the next frame's is made).
struct FsrHud {
	vk::Format                   format = vk::Format::eUndefined; // the pipeline's
	vk::Extent2D                 extent {};                       // the images'
	std::array<VulkanImage, 2>   images {};
	std::array<vk::ImageView, 2> views {};
	VulkanImage                  source;                // the backbuffer copied, the pass's input
	vk::ImageView                source_view = nullptr;
	vk::Sampler                  sampler    = nullptr;
	vk::DescriptorSetLayout      set_layout = nullptr;
	vk::PipelineLayout           layout     = nullptr;
	vk::ShaderModule             vertex = nullptr, fragment = nullptr;
	vk::Pipeline                 pipeline = nullptr;
	uint32_t                     index    = 0;
	bool                         ready    = false; // this frame's (images[index]) was recorded
};
FsrHud g_fsr_hud;

struct FsrHudParameters {
	float region[4];
	float extent[2];
	float threshold, invert;
};

void FsrHudDestroyImages(GraphicContext& graphics) {
	auto& h = g_fsr_hud;
	for (uint32_t i = 0; i <= h.images.size(); ++i) {
		auto& view  = i < h.images.size() ? h.views[i] : h.source_view;
		auto& image = i < h.images.size() ? h.images[i] : h.source;
		if (view != nullptr) graphics.device.destroyImageView(view, nullptr);
		view = nullptr;
		if (image.image != nullptr) graphics.DeleteImage(image);
		image.state = {};
	}
	h.extent = {};
	h.ready  = false;
}

vk::ShaderModule FsrShader(GraphicContext& graphics, const uint32_t* code, size_t words) {
	vk::ShaderModuleCreateInfo create {};
	create.codeSize         = words * sizeof(uint32_t);
	create.pCode            = code;
	vk::ShaderModule module = nullptr;
	RequireVulkanSuccess(graphics.device.createShaderModule(&create, nullptr, &module), "FSR HUD shader");
	return module;
}

// The HUD pass's pipeline for `format` and its images of `extent`.
bool FsrHudSetup(GraphicContext& graphics, vk::Format format, vk::Extent2D extent) {
	auto& h = g_fsr_hud;
	if (h.layout == nullptr) {
		vk::SamplerCreateInfo sampler {};
		sampler.magFilter    = vk::Filter::eLinear;
		sampler.minFilter    = vk::Filter::eLinear;
		sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		RequireVulkanSuccess(graphics.device.createSampler(&sampler, nullptr, &h.sampler), "FSR HUD sampler");
		// The canvas and the backbuffer's copy.
		std::array<vk::DescriptorSetLayoutBinding, 2> bindings {};
		for (uint32_t i = 0; i < bindings.size(); ++i) {
			bindings[i].binding            = i;
			bindings[i].descriptorType     = vk::DescriptorType::eCombinedImageSampler;
			bindings[i].descriptorCount    = 1;
			bindings[i].stageFlags         = vk::ShaderStageFlagBits::eFragment;
			bindings[i].pImmutableSamplers = &h.sampler;
		}
		vk::DescriptorSetLayoutCreateInfo set_layout {};
		set_layout.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
		set_layout.bindingCount = static_cast<uint32_t>(bindings.size());
		set_layout.pBindings    = bindings.data();
		RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&set_layout, nullptr, &h.set_layout),
		                     "FSR HUD descriptor layout");
		vk::PushConstantRange range {};
		range.stageFlags = vk::ShaderStageFlagBits::eFragment;
		range.size       = sizeof(FsrHudParameters);
		vk::PipelineLayoutCreateInfo layout {};
		layout.setLayoutCount         = 1;
		layout.pSetLayouts            = &h.set_layout;
		layout.pushConstantRangeCount = 1;
		layout.pPushConstantRanges    = &range;
		RequireVulkanSuccess(graphics.device.createPipelineLayout(&layout, nullptr, &h.layout), "FSR HUD pipeline layout");
		h.vertex   = FsrShader(graphics, GPU_BLIT_FS_TRIANGLE_SPV, std::size(GPU_BLIT_FS_TRIANGLE_SPV));
		h.fragment = FsrShader(graphics, FSR_HUD_MASK_SPV, std::size(FSR_HUD_MASK_SPV));
	}
	if (h.pipeline == nullptr || h.format != format) {
		if (h.pipeline != nullptr) graphics.device.destroyPipeline(h.pipeline, nullptr);
		h.pipeline = nullptr;
		std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
		stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
		stages[0].module = h.vertex;
		stages[0].pName  = "main";
		stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
		stages[1].module = h.fragment;
		stages[1].pName  = "main";
		vk::PipelineVertexInputStateCreateInfo   vertex_input {};
		vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
		input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
		vk::PipelineViewportStateCreateInfo viewport {};
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;
		vk::PipelineRasterizationStateCreateInfo rasterization {};
		rasterization.lineWidth = 1.0f;
		vk::PipelineMultisampleStateCreateInfo multisample {};
		multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
		// The HUD's pixels replaced (the shader discards the others).
		vk::PipelineColorBlendAttachmentState blend {};
		blend.blendEnable    = VK_FALSE;
		blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
		                       vk::ColorComponentFlagBits::eB;
		vk::PipelineColorBlendStateCreateInfo color_blend {};
		color_blend.attachmentCount = 1;
		color_blend.pAttachments    = &blend;
		const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic {};
		dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
		dynamic.pDynamicStates    = dynamic_states.data();
		vk::PipelineRenderingCreateInfo rendering {};
		rendering.colorAttachmentCount    = 1;
		rendering.pColorAttachmentFormats = &format;
		vk::GraphicsPipelineCreateInfo create {};
		create.pNext               = &rendering;
		create.stageCount          = static_cast<uint32_t>(stages.size());
		create.pStages             = stages.data();
		create.pVertexInputState   = &vertex_input;
		create.pInputAssemblyState = &input_assembly;
		create.pViewportState      = &viewport;
		create.pRasterizationState = &rasterization;
		create.pMultisampleState   = &multisample;
		create.pColorBlendState    = &color_blend;
		create.pDynamicState       = &dynamic;
		create.layout              = h.layout;
		RequireVulkanSuccess(graphics.device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &h.pipeline),
		                     "FSR HUD pipeline");
		h.format = format;
		FsrHudDestroyImages(graphics); // (of the old format)
	}
	if (h.extent != extent || h.images[0].image == nullptr) {
		FsrHudDestroyImages(graphics);
		for (uint32_t i = 0; i < h.images.size(); ++i) {
			vk::ImageCreateInfo create {};
			create.imageType     = vk::ImageType::e2D;
			create.extent        = vk::Extent3D {extent.width, extent.height, 1};
			create.mipLevels     = 1;
			create.arrayLayers   = 1;
			create.format        = format;
			create.tiling        = vk::ImageTiling::eOptimal;
			create.initialLayout = vk::ImageLayout::eUndefined;
			create.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eColorAttachment |
			               vk::ImageUsageFlagBits::eSampled;
			create.sharingMode = vk::SharingMode::eExclusive;
			create.samples     = vk::SampleCountFlagBits::e1;
			if (!graphics.CreateImage(create, h.images[i])) {
				FsrHudDestroyImages(graphics);
				return false;
			}
			vk::ImageViewCreateInfo view {};
			view.image            = h.images[i].image;
			view.viewType         = vk::ImageViewType::e2D;
			view.format           = format;
			view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
			RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &h.views[i]), "FSR HUD view");
		}
		vk::ImageCreateInfo create {};
		create.imageType     = vk::ImageType::e2D;
		create.extent        = vk::Extent3D {extent.width, extent.height, 1};
		create.mipLevels     = 1;
		create.arrayLayers   = 1;
		create.format        = format;
		create.tiling        = vk::ImageTiling::eOptimal;
		create.initialLayout = vk::ImageLayout::eUndefined;
		create.usage         = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
		create.sharingMode   = vk::SharingMode::eExclusive;
		create.samples       = vk::SampleCountFlagBits::e1;
		if (!graphics.CreateImage(create, h.source)) {
			FsrHudDestroyImages(graphics);
			return false;
		}
		vk::ImageViewCreateInfo view {};
		view.image            = h.source.image;
		view.viewType         = vk::ImageViewType::e2D;
		view.format           = format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(graphics.device.createImageView(&view, nullptr, &h.source_view), "FSR HUD source view");
		h.extent = extent;
	}
	return true;
}

void FsrHudDestroy(GraphicContext& graphics) {
	auto& h = g_fsr_hud;
	FsrHudDestroyImages(graphics);
	if (h.pipeline != nullptr) graphics.device.destroyPipeline(h.pipeline, nullptr);
	h.pipeline = nullptr;
	h.format   = vk::Format::eUndefined;
}

void ImageBarrier(vk::CommandBuffer command, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                  vk::AccessFlags2 src_access, vk::AccessFlags2 dst_access) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.srcAccessMask       = src_access;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	barrier.dstAccessMask       = dst_access;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency);
}

#endif

#ifdef KYTY_FRAMEGEN_FSR
// The swapchain calls this from its present (allowAsyncWorkloads off: on the game queue): the generated frame.
ffxReturnCode_t FsrGenerate(ffxDispatchDescFrameGeneration* params, void* context) {
	if (std::exchange(g_fsr_state.reset, false)) params->reset = true;
	if (g_fsr_state.hdr) {
		params->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
		params->minMaxLuminance[0]         = 0.0f;
		params->minMaxLuminance[1]         = 1000.0f;
	}
	return g_fsr_state.api.Dispatch(static_cast<ffxContext*>(context), &params->header);
}
#endif

} // namespace

#ifdef KYTY_FRAMEGEN_NGX
namespace {

const KytyDlssApi* g_dlss        = nullptr; // the bridge, when KYTY_UPSCALER=dlss found it
bool               g_dlss_device = false;   // the device has the extensions NGX needs

void DlssLog(int warning, const char* message) {
	LOGF("DLSS: %s\n", message);
	std::printf("DLSS%s: %s\n", warning != 0 ? " (warning)" : "", message);
	std::fflush(stdout);
}

// Every way DLSS cannot run: the game keeps its own TAA (demonsSoulsSuperResolution.cpp), no frames are generated.
void DlssOff(const char* why) {
	LOGF("DLSS: off (%s)\n", why);
	std::printf("DLSS: off (%s)\n", why);
	std::fflush(stdout);
	g_dlss     = nullptr;
	g_upscale  = false;
	g_generate = 0;
}

std::filesystem::path ExecutableFolder() {
	std::error_code error;
	auto            exe = std::filesystem::read_symlink("/proc/self/exe", error);
	return error ? std::filesystem::current_path(error) : exe.parent_path();
}

// The bridge: in KYTY_DLSS_DIR, else next to the emulator, else in a "dlss" folder next to it; NGX finds
// libnvidia-ngx-dlss.so.* (and libnvidia-ngx-dlssg.so.*) in the same folder.
bool LoadBridge() {
	std::vector<std::filesystem::path> candidates;
	if (const char* env = std::getenv("KYTY_DLSS_DIR"); env != nullptr && *env != 0) {
		candidates.emplace_back(env);
	} else {
		const auto exe = ExecutableFolder();
		candidates.push_back(exe);
		candidates.push_back(exe / "dlss");
	}
	for (const auto& folder: candidates) {
		const auto      path = folder / KYTY_DLSS_BRIDGE_LIBRARY;
		std::error_code error;
		if (!std::filesystem::exists(path, error)) continue;
		void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (module == nullptr) {
			std::printf("DLSS: cannot load %s (%s)\n", path.c_str(), dlerror());
			return false;
		}
		const auto         get = reinterpret_cast<KytyDlssGetApiFn>(dlsym(module, KYTY_DLSS_BRIDGE_ENTRY));
		const KytyDlssApi* api = get != nullptr ? get() : nullptr;
		if (api == nullptr || api->abi != KYTY_DLSS_BRIDGE_ABI) {
			std::printf("DLSS: %s is not a bridge of ABI %d (build it again with tools/local/dlss-bridge/build-dlss-linux.sh)\n",
			            path.c_str(), KYTY_DLSS_BRIDGE_ABI);
			dlclose(module);
			return false;
		}
		// NGX's own data (logs, its model cache): writable, next to the emulator's other temporary data.
		auto data = std::filesystem::current_path(error) / "_TempData" / "ngx";
		std::filesystem::create_directories(data, error);
		if (error) data = std::filesystem::temp_directory_path(error);
		const auto folder_text = std::filesystem::absolute(folder, error).string();
		if (api->Configure(folder_text.c_str(), data.c_str(), DlssLog) == 0) {
			std::printf("DLSS: the bridge refused its configuration\n");
			return false;
		}
		g_dlss = api;
		LOGF("DLSS: bridge %s loaded\n", path.c_str());
		std::printf("DLSS: bridge %s loaded\n", path.c_str());
		std::fflush(stdout);
		return true;
	}
	std::printf("DLSS: no %s (tools/local/dlss-bridge/build-dlss-linux.sh puts it next to kyty_emulator; or set "
	            "KYTY_DLSS_DIR)\n",
	            KYTY_DLSS_BRIDGE_LIBRARY);
	std::fflush(stdout);
	return false;
}

bool HasStencil(vk::Format format) {
	return format == vk::Format::eD16UnormS8Uint || format == vk::Format::eD24UnormS8Uint ||
	       format == vk::Format::eD32SfloatS8Uint;
}

KytyDlssImage BridgeImage(const UpscaleImage& image, vk::ImageAspectFlags aspect) {
	KytyDlssImage out {};
	out.image  = static_cast<VkImage>(image.image);
	out.view   = static_cast<VkImageView>(image.view);
	out.range  = {static_cast<VkImageAspectFlags>(aspect), image.base_level, 1, image.base_layer, 1};
	out.format = static_cast<VkFormat>(image.format);
	out.width  = image.extent.width;
	out.height = image.extent.height;
	return out;
}

// The depth as NGX samples it (shader read-only) and back to the layout the renderer left it in.
void DepthLayout(vk::CommandBuffer command, const UpscaleImage& depth, vk::ImageLayout from, vk::ImageLayout to,
                 const vk::detail::DispatchLoaderDynamic& dispatch) {
	vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eDepth;
	if (HasStencil(depth.format)) aspect |= vk::ImageAspectFlagBits::eStencil;
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = depth.image;
	barrier.subresourceRange    = {aspect, depth.base_level, 1, depth.base_layer, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency, dispatch);
}

KytyDlssImage BridgeImage(const VulkanImage& image, vk::ImageView view) {
	KytyDlssImage out {};
	out.image  = static_cast<VkImage>(image.image);
	out.view   = static_cast<VkImageView>(view);
	out.range  = {static_cast<VkImageAspectFlags>(IsDepthFormat(image.format) ? vk::ImageAspectFlagBits::eDepth
	                                                                          : vk::ImageAspectFlagBits::eColor),
	              0, 1, 0, 1};
	out.format = static_cast<VkFormat>(image.format);
	out.width  = image.extent.width;
	out.height = image.extent.height;
	return out;
}

// Frame generation through NGX (the presenter shows the frames, swapchain.cpp).
bool              g_fg_created = false; // the DLSS-G feature exists for these sizes
bool              g_fg_history = false; // the last frame given to it had inputs: the next one interpolates from it
bool              g_fg_failed  = false;
vk::Extent2D      g_fg_backbuffer {}, g_fg_render {};
vk::Format        g_fg_format = vk::Format::eUndefined;
std::atomic<bool> g_fg_discontinuity {false}; // a frame with inputs left unshown (NoteDroppedFrame)

// Matrices as Streamline and NGX take them: row-major, row vectors (p' = p * M), 16 floats.
using RowMatrix = std::array<float, 16>;

RowMatrix Identity() {
	RowMatrix m {};
	for (uint32_t i = 0; i < 4; ++i) m[i * 4 + i] = 1.0f;
	return m;
}

RowMatrix Transposed(const std::array<float, 16>& m) { // the game's column-vector matrix
	RowMatrix r {};
	for (uint32_t i = 0; i < 4; ++i)
		for (uint32_t j = 0; j < 4; ++j) r[i * 4 + j] = m[j * 4 + i];
	return r;
}

RowMatrix Multiply(const RowMatrix& a, const RowMatrix& b) {
	RowMatrix r {};
	for (uint32_t i = 0; i < 4; ++i)
		for (uint32_t j = 0; j < 4; ++j)
			for (uint32_t k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
	return r;
}

bool Inverted(const RowMatrix& m, RowMatrix& out) {
	double a[4][8] {};
	for (uint32_t i = 0; i < 4; ++i) {
		for (uint32_t j = 0; j < 4; ++j) a[i][j] = m[i * 4 + j];
		a[i][4 + i] = 1.0;
	}
	for (uint32_t c = 0; c < 4; ++c) {
		uint32_t pivot = c;
		for (uint32_t r = c + 1; r < 4; ++r) {
			if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
		}
		if (std::fabs(a[pivot][c]) < 1e-12) return false;
		for (uint32_t j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
		const double scale = 1.0 / a[c][c];
		for (uint32_t j = 0; j < 8; ++j) a[c][j] *= scale;
		for (uint32_t r = 0; r < 4; ++r) {
			if (r == c) continue;
			const double f = a[r][c];
			for (uint32_t j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
		}
	}
	for (uint32_t i = 0; i < 4; ++i)
		for (uint32_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<float>(a[i][4 + j]);
	return true;
}

// The game's world->clip matrix split into camera axes, position and projection terms, as the Streamline way does
// (its Decompose): P * V with a perspective P whose w row is (0, 0, 1, 0) and whose z row is constant (reversed-Z,
// infinite far).
struct CameraAxes {
	std::array<float, 3> right {}, up {}, forward {}, position {};
	float                sx = 1.0f, sy = 1.0f, cx = 0.0f, cy = 0.0f, z_near = 0.1f;
	bool                 valid = false;
};

CameraAxes Decompose(const std::array<float, 16>& m) {
	CameraAxes c;
	const auto row = [&](uint32_t i) { return std::array<double, 4> {m[i * 4], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]}; };
	const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
	const double flen = std::sqrt(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
	if (flen < 1e-6) return c;
	const double f[3] {r3[0] / flen, r3[1] / flen, r3[2] / flen};
	const double tf    = r3[3] / flen;
	const auto   split = [&](const std::array<double, 4>& r, double& s, double& off, double axis[3], double& t) {
		off = (r[0] * f[0] + r[1] * f[1] + r[2] * f[2]) / flen;
		double v[3] {r[0] / flen - off * f[0], r[1] / flen - off * f[1], r[2] / flen - off * f[2]};
		s = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (s < 1e-9) return false;
		for (int i = 0; i < 3; ++i) axis[i] = v[i] / s;
		t = (r[3] / flen - off * tf) / s;
		return true;
	};
	double sx = 0, cx = 0, sy = 0, cy = 0, x[3] {}, y[3] {}, tx = 0, ty = 0;
	if (!split(r0, sx, cx, x, tx) || !split(r1, sy, cy, y, ty)) return c;
	// Up = forward x right (Streamline's convention); a flipped clip y shows up as a negative sy.
	const double up[3] {f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0]};
	if (up[0] * y[0] + up[1] * y[1] + up[2] * y[2] < 0) {
		sy = -sy;
		ty = -ty;
	}
	for (int i = 0; i < 3; ++i) {
		c.right[i]   = static_cast<float>(x[i]);
		c.up[i]      = static_cast<float>(up[i]);
		c.forward[i] = static_cast<float>(f[i]);
		// View translation (tx, ty, tf) = -axes . position.
		c.position[i] = static_cast<float>(-(tx * x[i] + ty * up[i] + tf * f[i]));
	}
	c.sx     = static_cast<float>(sx);
	c.sy     = static_cast<float>(sy);
	c.cx     = static_cast<float>(cx);
	c.cy     = static_cast<float>(cy);
	c.z_near = static_cast<float>(r2[3] / flen);
	c.valid  = true;
	return c;
}

void Copy16(float (&out)[16], const RowMatrix& m) {
	for (uint32_t i = 0; i < 16; ++i) out[i] = m[i];
}

} // namespace
#endif

bool Enabled() {
	return g_ready;
}

bool Requested() {
	return g_generate > 0;
}

std::string SupportProblem(vk::PhysicalDevice physical_device) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (g_upscale) {
		sl::AdapterInfo adapter {};
		adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
		const auto result        = g_api.is_feature_supported(sl::kFeatureDLSS, adapter);
		std::printf("DLSS super resolution: %s\n", result == sl::Result::eOk
		                                               ? "Streamline offers it with this GPU and driver"
		                                               : (std::string("not offered here (") + sl::getResultAsStr(result) +
		                                                  "; an NVIDIA RTX GPU and a recent driver)")
		                                                     .c_str());
		std::fflush(stdout);
	}
#endif
	if (g_generate == 0 || g_fsr) {
		return {};
	}
#ifdef KYTY_FRAMEGEN_STREAMLINE
	// (Streamline answers before the device too: its programming guide asks while the adapters are enumerated. Issue
	// #45: a device refused with no word why; the driver's own refusal says nothing either.)
	sl::AdapterInfo adapter {};
	adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
	const auto  result       = g_api.is_feature_supported(sl::kFeatureDLSS_G, adapter);
	std::string problem;
	switch (result) {
		case sl::Result::eOk: break;
		case sl::Result::eErrorOSDisabledHWS:
			problem = "Windows' hardware-accelerated GPU scheduling is off (Settings > System > Display > Graphics > "
			          "Advanced graphics settings; then restart the PC)";
			break;
		case sl::Result::eErrorOSOutOfDate: problem = "this Windows version is too old for DLSS frame generation"; break;
		case sl::Result::eErrorDriverOutOfDate: problem = "the NVIDIA driver is too old for DLSS frame generation"; break;
		case sl::Result::eErrorNoSupportedAdapterFound:
		case sl::Result::eErrorAdapterNotSupported:
			problem = "this graphics card has no DLSS frame generation (NVIDIA RTX 40 or newer)";
			break;
		default: problem = std::string("Streamline does not offer DLSS frame generation here (") + sl::getResultAsStr(result) + ")"; break;
	}
	if (problem.empty()) {
		LOGF("frame generation: Streamline offers DLSS-G with this GPU, driver and Windows\n");
		std::printf("Frame generation: Streamline offers DLSS-G with this GPU, driver and Windows\n");
	} else {
		LOGF("frame generation: %s\n", problem.c_str());
		std::printf("Frame generation: %s\n", problem.c_str());
	}
	// (Where the driver's reservations go, for a refused device's log: RefusedDeviceAdvice.)
	PROCESS_MITIGATION_ASLR_POLICY aslr {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessASLRPolicy, &aslr, sizeof(aslr))) {
		std::printf("Frame generation: bottom-up ASLR %s for the emulator\n", aslr.EnableBottomUpRandomization != 0 ? "on" : "off");
	}
	std::fflush(stdout);
	return problem;
#else
	(void)physical_device;
	return {};
#endif
}

std::string RefusedDeviceAdvice() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	// (The emulator is built with fixed addresses, so without bottom-up ASLR its host allocations pack upward from the
	// guest's ceiling; an RTX 5080's driver refused the device until Exploit protection forced it on for the emulator.)
	PROCESS_MITIGATION_ASLR_POLICY aslr {};
	if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessASLRPolicy, &aslr, sizeof(aslr)) &&
	    aslr.EnableBottomUpRandomization == 0) {
		return " If it keeps failing, in Windows Security: App & browser control > Exploit protection > Program settings > "
		       "add kyty_emulator.exe > Randomize memory allocations (Bottom-up ASLR): Override system settings, On "
		       "(issue #45).";
	}
#endif
	return {};
}

bool WantsCamera() {
	return g_ready && !g_camera.valid;
}

bool InputsGathered() {
	return g_sources.valid && g_camera.valid;
}

void ReportLoader(const void* address, const char* what) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (g_generate == 0 && !g_upscale) return;
	HMODULE module = nullptr;
	wchar_t path[MAX_PATH] = {};
	if (address != nullptr &&
	    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       static_cast<LPCWSTR>(address), &module) &&
	    GetModuleFileNameW(module, path, MAX_PATH) != 0) {
		std::printf("Frame generation: %s comes from %ls\n", what, path);
	} else {
		std::printf("Frame generation: %s: module unknown\n", what);
	}
	std::fflush(stdout);
#else
	(void)address;
	(void)what;
#endif
}

void BypassCommandHooks(vk::Device device) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	// KYTY_FRAMEGEN_HOOKS=1 keeps Streamline's wrappers (for comparison).
	if ((g_generate == 0 && !g_upscale) || g_fsr || std::getenv("KYTY_FRAMEGEN_HOOKS") != nullptr) {
		return;
	}
	// The real loader: Streamline itself forwards to it.
	HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
	auto*   get_device_proc = loader != nullptr ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
	                                                reinterpret_cast<void*>(GetProcAddress(loader, "vkGetDeviceProcAddr")))
	                                          : nullptr;
	if (get_device_proc == nullptr) {
		return;
	}
	auto&      dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
	const auto native     = static_cast<VkDevice>(device);
	uint32_t   replaced   = 0;
	const auto direct     = [&](auto& function, const char* name) {
        if (auto* pointer = get_device_proc(native, name); pointer != nullptr) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(pointer);
            ++replaced;
        }
	};
	direct(dispatcher.vkBeginCommandBuffer, "vkBeginCommandBuffer");
	direct(dispatcher.vkCmdBindPipeline, "vkCmdBindPipeline");
	direct(dispatcher.vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets");
	direct(dispatcher.vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
	std::printf("Frame generation: %u command-buffer calls bypass Streamline\n", replaced);
#else
	(void)device;
#endif
}

namespace {
void LoadToggles(); // the in-game switches (below)
} // namespace

void Initialize() {
	LoadToggles();
	const char* value = std::getenv("KYTY_FRAMEGEN");
	g_generate        = value != nullptr ? std::clamp(std::atoi(value), 0, 5) : 0;
	const char* upscaler = std::getenv("KYTY_UPSCALER");
	g_upscale            = upscaler != nullptr && std::string_view(upscaler) == "dlss";
	// Linux: KYTY_FRAMEGEN_BACKEND=fsr is FSR 3 frame interpolation built in (fsr-native.h, KYTY_FRAMEGEN implied), shown by
	// the presenter as NGX's DLSS-G frames are; no NVIDIA runtime. DLSS super resolution may still load the bridge.
	if (FsrNative::InterpolationRequested()) {
		g_generate   = 1;
		g_fsr_native = true;
		std::printf("Frame generation: AMD FSR 3 (2x, built in)\n");
		std::fflush(stdout);
	}
	if (g_generate == 0 && !g_upscale) {
		return;
	}
	if (g_fsr_native && !g_upscale) {
		return;
	}
	if (const char* backend = std::getenv("KYTY_FRAMEGEN_BACKEND");
	    !g_fsr_native && backend != nullptr && std::string(backend) == "fsr") {
		// (No Streamline with FSR, so no DLSS super resolution either.)
		g_upscale = false;
#ifdef KYTY_FRAMEGEN_FSR
		// AMD's runtime next to the emulator, only AMD's own; FSR 3.1 makes one frame per frame.
		wchar_t exe[MAX_PATH] {};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		const auto runtime = std::filesystem::path(exe).parent_path() / "amd_fidelityfx_vk.dll";
		HMODULE    module  = nullptr;
		if (!LocalPlatform::IsAmdSigned(runtime.wstring())) {
			std::printf("Frame generation: %s is missing or not signed by AMD, FSR off\n", runtime.string().c_str());
		} else {
			module = LoadLibraryW(runtime.c_str());
		}
		if (module == nullptr) {
			LOGF("frame generation: no FSR runtime (amd_fidelityfx_vk.dll)\n");
			std::fflush(stdout);
			g_generate = 0;
			return;
		}
		ffxLoadFunctions(&g_fsr_state.api, module);
		if (g_fsr_state.api.CreateContext == nullptr || g_fsr_state.api.Configure == nullptr ||
		    g_fsr_state.api.Dispatch == nullptr || g_fsr_state.api.Query == nullptr || g_fsr_state.api.DestroyContext == nullptr) {
			std::printf("Frame generation: amd_fidelityfx_vk.dll lacks an entry point, FSR off\n");
			g_generate = 0;
			return;
		}
		ffxConfigureDescGlobalDebug1 debug {};
		debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
		debug.fpMessage   = FsrMessage;
		debug.debugLevel  = std::getenv("KYTY_FRAMEGEN_VERBOSE") != nullptr ? FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE
		                                                                    : FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
		(void)g_fsr_state.api.Configure(nullptr, &debug.header);
		g_generate = 1;
		g_fsr      = true;
		LOGF("frame generation: FSR 3.1 runtime loaded from %s\n", runtime.string().c_str());
		std::printf("Frame generation: AMD FSR 3.1 (2x)\n");
		std::fflush(stdout);
#else
		LOGF("frame generation: this build has no FSR support\n");
		g_generate = 0;
#endif
		return;
	}
	// (Every way out without Streamline turns both off.)
	struct Off {
		bool done = false;
		~Off() {
			if (!done) {
				if (!g_fsr_native) g_generate = 0;
				g_upscale = false;
			}
		}
	} off;
#ifdef KYTY_FRAMEGEN_STREAMLINE
	// NVIDIA's Streamline runtime, which no release ships (the user's own copy: the Streamline SDK's bin\x64):
	// KYTY_STREAMLINE_DIR (the launcher's "Streamline..." folder, run-windows -StreamlineDir), else a "streamline"
	// folder next to the emulator, else the SDK unpacked beside the build (_Build\deps\streamline\sdk). A folder
	// may be the SDK's own (its bin\x64 is taken).
	std::vector<std::filesystem::path> candidates;
	wchar_t                            exe[MAX_PATH] {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const auto exe_dir = std::filesystem::path(exe).parent_path();
	// (Wide: a folder's name in any language.)
	if (const wchar_t* env = _wgetenv(L"KYTY_STREAMLINE_DIR"); env != nullptr && *env != L'\0') {
		candidates.emplace_back(env);
	} else {
		candidates.push_back(exe_dir / "streamline");
		candidates.push_back(exe_dir.parent_path() / "deps" / "streamline" / "sdk");
	}
	std::filesystem::path dir;
	for (const auto& candidate: candidates) {
		for (const auto& folder: {candidate, candidate / "bin" / "x64"}) {
			std::error_code error;
			if (dir.empty() && std::filesystem::exists(folder / "sl.interposer.dll", error)) dir = folder;
		}
	}
	if (dir.empty()) {
		LOGF("frame generation: no sl.interposer.dll (KYTY_STREAMLINE_DIR or a streamline folder next to the emulator)\n");
		std::printf("Frame generation: NVIDIA's Streamline runtime was not found (choose its folder in the launcher)\n");
		return;
	}
	const auto interposer = dir / "sl.interposer.dll";
	// Only NVIDIA's own (Streamline then checks its plugins' signatures itself).
	if (!LocalPlatform::IsNvidiaSigned(interposer.wstring())) {
		LOGF("frame generation: %s is not signed by NVIDIA: not loaded\n", interposer.string().c_str());
		std::printf("Frame generation: %s is not signed by NVIDIA, not loaded\n", interposer.string().c_str());
		return;
	}
	if (std::getenv("KYTY_FRAMEGEN_VERBOSE") != nullptr) {
		// Graphics modules already loaded before Streamline (it must come first).
		for (const char* name: {"vulkan-1.dll", "nvoglv64.dll", "opengl32.dll", "dxgi.dll", "d3d11.dll", "d3d12.dll",
		                        "d3d9.dll", "nvapi64.dll", "nvcuda.dll", "nvofapi64.dll", "sl.interposer.dll"}) {
			if (GetModuleHandleA(name) != nullptr) std::printf("Frame generation: %s already loaded\n", name);
		}
	}
	HMODULE    module     = LoadLibraryW(interposer.c_str());
	if (module == nullptr) {
		LOGF("frame generation: cannot load %s\n", interposer.string().c_str());
		std::printf("Frame generation: cannot load %s\n", interposer.string().c_str());
		return;
	}
	const auto load = [&](auto& function, const char* name) {
		function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
		    reinterpret_cast<void*>(GetProcAddress(module, name)));
		return function != nullptr;
	};
	if (!load(g_api.init, "slInit") || !load(g_api.is_feature_supported, "slIsFeatureSupported") ||
	    !load(g_api.is_feature_loaded, "slIsFeatureLoaded") ||
	    !load(g_api.get_feature_function, "slGetFeatureFunction") ||
	    !load(g_api.get_new_frame_token, "slGetNewFrameToken") ||
	    !load(g_api.set_constants, "slSetConstants") ||
	    !load(g_api.set_tag_for_frame, "slSetTagForFrame") ||
	    !load(g_api.evaluate_feature, "slEvaluateFeature")) {
		LOGF("frame generation: sl.interposer.dll lacks an entry point\n");
		return;
	}
	static const std::wstring plugin_dir = dir.wstring();
	static const std::wstring log_dir    = (dir.parent_path() / "logs").wstring();
	static const wchar_t*     plugin_paths[] {plugin_dir.c_str()};
	// KYTY_FRAMEGEN_FEATURES=dlssg,reflex,pcl: the plugins to load (a diagnostic subset).
	static std::vector<sl::Feature> features;
	if (g_generate > 0) features = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
	if (const char* list = std::getenv("KYTY_FRAMEGEN_FEATURES"); list != nullptr && g_generate > 0) {
		const std::string text = list;
		features.clear();
		if (text.find("dlssg") != std::string::npos) features.push_back(sl::kFeatureDLSS_G);
		if (text.find("reflex") != std::string::npos) features.push_back(sl::kFeatureReflex);
		if (text.find("pcl") != std::string::npos) features.push_back(sl::kFeaturePCL);
	}
	if (g_upscale) features.push_back(sl::kFeatureDLSS);
	if (const char* dlss = std::getenv("KYTY_DLSS"); dlss != nullptr && std::atoi(dlss) > 0) g_dlss_requested = true;
	if (const char* nis = std::getenv("KYTY_NIS"); nis != nullptr) {
		const int height = std::atoi(nis);
		if (height >= 720 && height <= 2160) g_nis_height = static_cast<uint32_t>(height);
	}
	if (const char* late = std::getenv("KYTY_LATE_DLSS"); late != nullptr) {
		const int height = std::atoi(late);
		if (height >= 720 && height <= 2160) g_late_height = static_cast<uint32_t>(height);
	}
	if (const char* aa = std::getenv("KYTY_LATE_DLSS_AA"); aa != nullptr && aa[0] == '1') g_late_aa = true;
	// (DLAA and NIS work on the game's own TAA and 4K composite: the super resolution path replaces both.)
	if ((g_dlss_requested || g_nis_height != 0 || g_late_height != 0) && (g_upscale || g_generate == 0)) {
		std::printf("Frame generation: KYTY_DLSS / KYTY_NIS / KYTY_LATE_DLSS ignored (%s)\n",
		            g_upscale ? "the super resolution path (KYTY_UPSCALER=dlss) is on" : "they need frame generation");
		g_dlss_requested = false;
		g_nis_height     = 0;
		g_late_height    = 0;
	}
	if ((g_dlss_requested || g_nis_height != 0 || g_late_height != 0) &&
	    !load(g_api.evaluate_feature, "slEvaluateFeature")) {
		LOGF("frame generation: sl.interposer.dll lacks slEvaluateFeature\n");
		g_dlss_requested = false;
		g_nis_height     = 0;
		g_late_height    = 0;
	}
	if (g_dlss_requested && !load(g_api.free_resources, "slFreeResources")) g_api.free_resources = nullptr;
	if ((g_dlss_requested || g_late_height != 0) && !g_upscale) features.push_back(sl::kFeatureDLSS);
	if (g_nis_height != 0) features.push_back(sl::kFeatureNIS);
	sl::Preferences           preferences {};
	preferences.showConsole        = false;
	preferences.logLevel           = std::getenv("KYTY_FRAMEGEN_VERBOSE") ? sl::LogLevel::eVerbose
	                                                                      : sl::LogLevel::eDefault;
	preferences.pathsToPlugins     = plugin_paths;
	preferences.numPathsToPlugins  = 1;
	preferences.pathToLogsAndData  = nullptr;
	preferences.logMessageCallback = LogMessage;
	preferences.flags              = sl::PreferenceFlags::eDisableCLStateTracking |
	                    sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	preferences.featuresToLoad    = features.data();
	preferences.numFeaturesToLoad = static_cast<uint32_t>(features.size());
	preferences.engine            = sl::EngineType::eCustom;
	preferences.engineVersion     = "1.0";
	preferences.projectId         = "6a3f1d5e-4b7c-4f2a-9d8e-1c2b3a4d5e6f";
	preferences.renderAPI         = sl::RenderAPI::eVulkan;
	if (const auto result = g_api.init(preferences, sl::kSDKVersion); result != sl::Result::eOk) {
		LOGF("frame generation: slInit failed (%d)\n", static_cast<int>(result));
		std::printf("Frame generation: slInit failed (%d)\n", static_cast<int>(result));
		return;
	}
	// SDL loads the interposer as the Vulkan loader: every instance/device/swapchain call then
	// goes through Streamline, which adds what DLSS-G needs.
	SetEnvironmentVariableW(L"SDL_VULKAN_LIBRARY", interposer.c_str());
	_putenv_s("SDL_VULKAN_LIBRARY", interposer.string().c_str());
	LOGF("frame generation: Streamline loaded from %s, %d generated frame(s), DLSS super resolution %s\n",
	     dir.string().c_str(), g_generate, g_upscale ? "asked for" : "off");
	off.done = true;
#elif defined(KYTY_FRAMEGEN_NGX)
	// (DLSS super resolution, frame generation, or both: the bridge either way. NGX makes the frames, the presenter
	// shows them: swapchain.cpp, PresenterGenerates.)
	if (!LoadBridge()) return;
	off.done = true;
#else
	LOGF("frame generation: this build has no Streamline support\n");
	std::printf("DLSS: this build has no Streamline support\n");
#endif
}

void AddInstanceExtensions(const std::vector<vk::ExtensionProperties>& available, std::vector<const char*>& extensions) {
#ifdef KYTY_FRAMEGEN_NGX
	if (g_dlss == nullptr) return;
	uint32_t                     count  = 0;
	const VkExtensionProperties* wanted = nullptr;
	if (g_dlss->InstanceExtensions(&count, &wanted) == 0) return DlssOff("NGX named no instance extensions");
	for (uint32_t i = 0; i < count; ++i) {
		const char* name = wanted[i].extensionName;
		if (std::none_of(available.begin(), available.end(),
		                 [&](const vk::ExtensionProperties& item) { return std::strcmp(item.extensionName.data(), name) == 0; })) {
			std::printf("DLSS: the Vulkan instance has no %s\n", name);
			return DlssOff("an instance extension NGX needs is missing");
		}
	}
	for (uint32_t i = 0; i < count; ++i) {
		const char* name = wanted[i].extensionName;
		if (std::none_of(extensions.begin(), extensions.end(), [&](const char* have) { return std::strcmp(have, name) == 0; }))
			extensions.push_back(name);
	}
	LOGF("DLSS: %u instance extension(s) for NGX\n", count);
#else
	(void)available;
	(void)extensions;
#endif
}

void AddDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical_device,
                         const std::vector<vk::ExtensionProperties>& available, std::vector<const char*>& extensions) {
#ifdef KYTY_FRAMEGEN_NGX
	if (g_dlss == nullptr) return;
	uint32_t                     count  = 0;
	const VkExtensionProperties* wanted = nullptr;
	if (g_dlss->DeviceExtensions(static_cast<VkInstance>(instance), static_cast<VkPhysicalDevice>(physical_device), &count,
	                             &wanted) == 0)
		return DlssOff("NGX does not offer DLSS with this GPU and driver (an NVIDIA RTX GPU and a recent driver)");
	for (uint32_t i = 0; i < count; ++i) {
		const char* name = wanted[i].extensionName;
		if (std::none_of(available.begin(), available.end(),
		                 [&](const vk::ExtensionProperties& item) { return std::strcmp(item.extensionName.data(), name) == 0; })) {
			std::printf("DLSS: the GPU has no %s\n", name);
			return DlssOff("a device extension NGX needs is missing");
		}
	}
	for (uint32_t i = 0; i < count; ++i) {
		const char* name = wanted[i].extensionName;
		if (std::none_of(extensions.begin(), extensions.end(), [&](const char* have) { return std::strcmp(have, name) == 0; }))
			extensions.push_back(name);
	}
	g_dlss_device = true;
	LOGF("DLSS: %u device extension(s) for NGX\n", count);
#else
	(void)instance;
	(void)physical_device;
	(void)available;
	(void)extensions;
#endif
}

void OnDevice(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device) {
	(void)instance;
	(void)device;
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (g_upscale) {
		sl::AdapterInfo adapter {};
		adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
		bool loaded              = false;
		if (g_api.is_feature_supported(sl::kFeatureDLSS, adapter) == sl::Result::eOk &&
		    g_api.is_feature_loaded(sl::kFeatureDLSS, loaded) == sl::Result::eOk && loaded &&
		    FeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", g_api.dlss_set_options) &&
		    FeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", g_api.dlss_get_optimal_settings)) {
			g_upscale_ready = true;
			std::printf("DLSS super resolution: ready\n");
		} else {
			std::printf("DLSS super resolution: not available on this device (the game's image is upscaled without it)\n");
		}
		std::fflush(stdout);
	}
#elif defined(KYTY_FRAMEGEN_NGX)
	if ((g_upscale || g_generate > 0) && g_dlss != nullptr && g_dlss_device) {
		auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
		if (g_dlss->Initialize(static_cast<VkInstance>(instance), static_cast<VkPhysicalDevice>(physical_device),
		                       static_cast<VkDevice>(device), dispatch.vkGetInstanceProcAddr, dispatch.vkGetDeviceProcAddr) != 0) {
			if (g_upscale) {
				g_upscale_ready = true;
				std::printf("DLSS super resolution: ready (NVIDIA NGX)\n");
			}
			if (g_generate > 0 && !g_fsr_native) {
				const int most = g_dlss->FrameGenMaxCount();
				if (most < 1) {
					LOGF("frame generation: NGX offers no DLSS-G on this GPU or driver\n");
					std::printf("Frame generation: not available on this GPU or driver (NVIDIA RTX 40 or newer)\n");
					g_generate = 0;
				} else {
					if (g_generate > most) {
						std::printf("Frame generation: %d generated frame(s) asked for, the GPU makes up to %d\n", g_generate, most);
						g_generate = most;
					}
					g_ready = true;
					InitializeHudless();
					LOGF("frame generation: DLSS-G ready through NGX, %d generated frame(s) per frame\n", g_generate);
					std::printf("Frame generation: DLSS-G ready (NVIDIA NGX, %d generated frame(s) per frame, %dx)\n",
					            g_generate, g_generate + 1);
				}
			}
		} else {
			std::printf("DLSS: NGX did not start on this device (the game keeps its own TAA, no frame generation)\n");
			g_generate = 0;
		}
		std::fflush(stdout);
	}
	// (FSR 3 built in: its contexts are made by the present thread with the first frame it generates.)
	if (g_fsr_native) g_ready = true;
	if (g_generate > 0 && !g_ready) g_generate = 0; // (no NGX: Requested() and the rest see it off)
#endif
	if (g_generate == 0 || g_fsr || g_fsr_native) {
		return;
	}
#ifdef KYTY_FRAMEGEN_STREAMLINE
	sl::AdapterInfo adapter {};
	adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
	bool loaded              = false;
	if (g_api.is_feature_supported(sl::kFeatureDLSS_G, adapter) != sl::Result::eOk ||
	    g_api.is_feature_loaded(sl::kFeatureDLSS_G, loaded) != sl::Result::eOk || !loaded) {
		LOGF("frame generation: DLSS-G is not supported on this device\n");
		std::printf("Frame generation: DLSS-G is not supported on this device\n");
		return;
	}
	if (!FeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", g_api.dlssg_set_options) ||
	    !FeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", g_api.dlssg_get_state) ||
	    !FeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", g_api.reflex_set_options) ||
	    !FeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", g_api.pcl_set_marker)) {
		return;
	}
	// (Required with DLSS-G, whatever the Reflex mode: Streamline's DLSS-G and Reflex guides.)
	(void)FeatureFunction(sl::kFeatureReflex, "slReflexSleep", g_api.reflex_sleep);
	sl::ReflexOptions reflex {};
	reflex.mode = sl::ReflexMode::eLowLatency;
	if (const auto result = g_api.reflex_set_options(reflex); result != sl::Result::eOk) {
		LOGF("frame generation: slReflexSetOptions failed (%d)\n", static_cast<int>(result));
	}
	g_ready = true;
	InitializeHudless();
	LOGF("frame generation: DLSS-G ready\n");
	std::printf("Frame generation: DLSS-G ready (%d generated frame(s) per frame)\n", g_generate);
	const auto feature_ready = [&](sl::Feature feature, const char* name) {
		bool feature_loaded = false;
		if (g_api.is_feature_supported(feature, adapter) != sl::Result::eOk ||
		    g_api.is_feature_loaded(feature, feature_loaded) != sl::Result::eOk || !feature_loaded) {
			std::printf("Frame generation: %s is not available on this device\n", name);
			return false;
		}
		return true;
	};
	if ((g_dlss_requested || g_late_height != 0) && feature_ready(sl::kFeatureDLSS, "DLSS") &&
	    FeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", g_api.dlss_set_options)) {
		g_dlss_ready = g_dlss_requested;
		g_late_ready = g_late_height != 0;
		if (g_dlss_ready) std::printf("Frame generation: DLSS (DLAA) replaces the game's TAA\n");
		if (g_late_ready)
			std::printf("Frame generation: late DLSS: the finished picture upscaled by DLSS to %u lines before the final "
			            "composite (KYTY_LATE_DLSS; Shift+F6 on/off)%s\n",
			            g_late_height,
			            g_late_aa ? "; the game's TAA not run, DLSS anti-aliases the jittered picture (KYTY_LATE_DLSS_AA)"
			                      : "");
	}
	if (g_nis_height != 0 && feature_ready(sl::kFeatureNIS, "NIS") &&
	    FeatureFunction(sl::kFeatureNIS, "slNISSetOptions", g_api.nis_set_options)) {
		g_nis_ready = true;
		std::printf("Frame generation: NIS scales the picture to %u lines before the final composite\n", g_nis_height);
	}
	std::fflush(stdout);
#endif
}

#ifdef KYTY_FRAMEGEN_STREAMLINE
namespace {
sl::Constants DirectConstants(float width, float height, bool reset); // (below: a game without a camera of its own)
// KYTY_PERF_PROBE=<control file>: the frames per second of each 5 s window, logged with the late DLSS / NIS state; a control
// file holding "auto" flips that switch every window (A B A B in one process), "on" / "off" sets it.
void PerfProbe() {
	static const char* control = std::getenv("KYTY_PERF_PROBE");
	if (control == nullptr) return;
	using Clock = std::chrono::steady_clock;
	static auto     start  = Clock::now();
	static uint32_t frames = 0;
	++frames;
	const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
	if (seconds < 5.0) return;
	std::printf("Perf probe: %.1f fps over %.1f s, late DLSS/NIS %s\n", frames / seconds, seconds,
	            g_nis_off.load() ? "off" : "on");
	std::fflush(stdout);
	std::string want;
	if (std::ifstream file(control); file) file >> want;
	bool target = g_nis_off.load();
	if (want == "auto") target = !target;
	else if (want == "on") target = false;
	else if (want == "off") target = true;
	if (target != g_nis_off.load()) g_nis_off = target;
	start  = Clock::now();
	frames = 0;
}
} // namespace
#endif

void BeforePresent(const FrameInputs* inputs, vk::Extent2D backbuffer, vk::Rect2D region,
                   vk::Format format, uint32_t image_count) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!g_ready || g_fsr) {
		return;
	}
	PerfProbe();
	const sl::ViewportHandle viewport {0};
	const bool               usable = inputs != nullptr && inputs->valid;
	g_run_with    = usable ? g_run_with + 1 : 0;
	g_run_without = usable ? 0 : g_run_without + 1;
	const bool mode_on = g_mode_on ? g_run_without < TurnOffAfter : g_run_with >= TurnOnAfter;
	// DLSS-G options: on while frames carry inputs; sizes are hints for its allocations.
	if (!g_options_set || g_options_format != format || mode_on != g_mode_on ||
	    (usable && (g_options_backbuffer != backbuffer || g_options_render != inputs->render_extent))) {
		sl::DLSSGOptions options {};
		options.mode                = mode_on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
		options.flags               = sl::DLSSGFlags::eRetainResourcesWhenOff;
		options.numFramesToGenerate = static_cast<uint32_t>(g_generate);
		options.numBackBuffers      = image_count;
		options.colorWidth          = backbuffer.width;
		options.colorHeight         = backbuffer.height;
		options.colorBufferFormat   = static_cast<uint32_t>(format);
		if (HudlessOn()) {
			options.hudLessBufferFormat = static_cast<uint32_t>(format); // (ScaleHudless: the backbuffer's)
		}
		if (usable) {
			options.mvecDepthWidth    = inputs->render_extent.width;
			options.mvecDepthHeight   = inputs->render_extent.height;
			options.mvecBufferFormat  = static_cast<uint32_t>(inputs->motion.format);
			options.depthBufferFormat = static_cast<uint32_t>(inputs->depth.format);
			g_options_render          = inputs->render_extent;
		}
		if (const auto result = g_api.dlssg_set_options(viewport, options); result != sl::Result::eOk) {
			LOGF("frame generation: slDLSSGSetOptions failed (%d)\n", static_cast<int>(result));
		}
		g_options_set        = true;
		g_options_backbuffer = backbuffer;
		g_options_format     = format;
		if (mode_on != g_mode_on) {
			LOGF("frame generation: DLSS-G %s\n", mode_on ? "on" : "off (no inputs)");
			g_mode_on = mode_on;
		}
	}

	++g_frame_index;
	if (NewFrameToken(g_token, &g_frame_index) != sl::Result::eOk || g_token == nullptr) {
		g_token = nullptr;
		return;
	}
	g_present_index.store(g_frame_index, std::memory_order_release);
	auto& token = *g_token;
	// The frame's Reflex sleep, then its markers (all at its present: the game's frame was made before).
	if (g_api.reflex_sleep != nullptr && kyty_local_framegen_sleep.load(std::memory_order_relaxed) != 0)
		g_api.reflex_sleep(token);
	g_api.pcl_set_marker(sl::PCLMarker::eSimulationStart, token);
	g_api.pcl_set_marker(sl::PCLMarker::eSimulationEnd, token);
	g_api.pcl_set_marker(sl::PCLMarker::eRenderSubmitStart, token);

	sl::Constants constants {};
	const auto    camera = usable ? Decompose(inputs->camera.view_proj) : CameraFrame {};
	Mat4          identity;
	for (uint32_t i = 0; i < 4; ++i) {
		identity[i] = sl::float4(i == 0, i == 1, i == 2, i == 3);
	}
	constants.cameraViewToClip = identity;
	constants.clipToCameraView = identity;
	constants.clipToLensClip   = identity;
	constants.clipToPrevClip   = identity;
	constants.prevClipToClip   = identity;
	constants.jitterOffset     = sl::float2(0.0f, 0.0f);
	constants.mvecScale        = sl::float2(1.0f, 1.0f);
	constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
	constants.cameraPos        = sl::float3(0, 0, 0);
	constants.cameraUp         = sl::float3(0, 1, 0);
	constants.cameraRight      = sl::float3(1, 0, 0);
	constants.cameraFwd        = sl::float3(0, 0, 1);
	constants.cameraNear       = 0.1f;
	constants.cameraFar        = 10000.0f;
	constants.cameraFOV        = 1.0f;
	constants.cameraAspectRatio = 16.0f / 9.0f;
	constants.motionVectorsInvalidValue = sl::INVALID_FLOAT;
	constants.depthInverted       = sl::Boolean::eTrue;
	constants.cameraMotionIncluded = sl::Boolean::eTrue;
	constants.motionVectors3D     = sl::Boolean::eFalse;
	// (A frame left unshown before this one: its motion and matrices are to the game's previous frame, not to the one
	// presented last.)
	const bool discontinuity      = g_discontinuity.exchange(false, std::memory_order_relaxed) || (usable && inputs->cut);
	constants.reset               = usable && !discontinuity ? sl::Boolean::eFalse : sl::Boolean::eTrue;
	constants.orthographicProjection = sl::Boolean::eFalse;
	constants.motionVectorsDilated   = sl::Boolean::eFalse;
	constants.motionVectorsJittered  = sl::Boolean::eFalse;
	if (usable && camera.valid) {
		// Projection in view space: x' = sx*x + cx*z, y' = sy*y + cy*z, z' = near, w' = z.
		Mat4 projection;
		projection[0] = sl::float4(camera.sx, 0, 0, 0);
		projection[1] = sl::float4(0, camera.sy, 0, 0);
		projection[2] = sl::float4(camera.cx, camera.cy, 0, 1);
		projection[3] = sl::float4(0, 0, camera.z_near, 0);
		constants.cameraViewToClip = projection;
		Inverted(projection, constants.clipToCameraView);
		const auto current  = Transposed(inputs->camera.view_proj);
		const auto previous = Transposed(inputs->camera.prev_view_proj);
		Mat4       clip_to_world;
		if (Inverted(current, clip_to_world)) {
			constants.clipToPrevClip = Multiply(clip_to_world, previous);
			Inverted(constants.clipToPrevClip, constants.prevClipToClip);
		}
		const float width  = static_cast<float>(inputs->render_extent.width);
		const float height = static_cast<float>(inputs->render_extent.height);
		constants.jitterOffset = sl::float2(inputs->camera.jitter_ndc[0] * width * 0.5f,
		                                    -inputs->camera.jitter_ndc[1] * height * 0.5f);
		static const bool pixels_mv = [] {
			const char* mv = std::getenv("KYTY_FRAMEGEN_MV");
			return mv != nullptr && std::string(mv) == "px";
		}();
		// The game's vectors point along the motion, in UV units: its TAA reads the history at
		// uv - mv. DLSS wants them towards the previous position (uv + mv = previous uv), so they
		// are negated; with them as they were, generated frames moved the scene the wrong way
		// when the camera turned. KYTY_FRAMEGEN_MV_SIGN=1 keeps the game's sign (a comparison).
		static const float mv_sign = [] {
			const char* sign = std::getenv("KYTY_FRAMEGEN_MV_SIGN");
			return sign != nullptr && std::atof(sign) > 0 ? 1.0f : -1.0f;
		}();
		constants.mvecScale = pixels_mv ? sl::float2(mv_sign / width, mv_sign / height) : sl::float2(mv_sign, mv_sign);
		constants.cameraPos   = camera.position;
		constants.cameraUp    = camera.up;
		constants.cameraRight = camera.right;
		constants.cameraFwd   = camera.forward;
		constants.cameraNear  = camera.z_near;
		constants.cameraFar   = 100000.0f;
		constants.cameraFOV   = 2.0f * std::atan(1.0f / std::fabs(camera.sy));
		constants.cameraAspectRatio = std::fabs(camera.sy / camera.sx);
		if (inputs->direct) {
			const auto direct = DirectConstants(width, height, false);
			constants.mvecScale     = direct.mvecScale;
			constants.depthInverted = direct.depthInverted;
		}
	}
	if (const auto result = g_api.set_constants(constants, token, viewport); result != sl::Result::eOk &&
	                                                                          g_presents < 8) {
		LOGF("frame generation: slSetConstants failed (%d)\n", static_cast<int>(result));
	}

	if (usable) {
		sl::Resource depth(sl::ResourceType::eTex2d, static_cast<VkImage>(inputs->depth.image), nullptr,
		                   static_cast<VkImageView>(inputs->depth_view),
		                   static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		depth.width        = inputs->depth.extent.width;
		depth.height       = inputs->depth.extent.height;
		depth.nativeFormat = static_cast<uint32_t>(inputs->depth.format);
		depth.mipLevels    = 1;
		depth.arrayLayers  = 1;
		depth.flags        = 0;
		depth.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(inputs->depth.usage));
		sl::Resource motion(sl::ResourceType::eTex2d, static_cast<VkImage>(inputs->motion.image), nullptr,
		                    static_cast<VkImageView>(inputs->motion_view),
		                    static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
		motion.width        = inputs->motion.extent.width;
		motion.height       = inputs->motion.extent.height;
		motion.nativeFormat = static_cast<uint32_t>(inputs->motion.format);
		motion.mipLevels    = 1;
		motion.arrayLayers  = 1;
		motion.flags        = 0;
		motion.usage = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(inputs->motion.usage));
		const sl::Extent full {0, 0, inputs->render_extent.width, inputs->render_extent.height};
		const sl::Extent game {static_cast<uint32_t>(region.offset.y), static_cast<uint32_t>(region.offset.x),
		                       region.extent.width, region.extent.height};
		// The backbuffer is tagged only when the game image covers part of it (KYTY_PRESENT_ASPECT).
		const bool      partial = region.offset.x != 0 || region.offset.y != 0 ||
		                     region.extent.width != backbuffer.width || region.extent.height != backbuffer.height;
		sl::ResourceTag tags[] {
		    sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &full),
		    sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent,
		                    &full),
		    sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent,
		                    &game),
		};
		if (const auto result = g_api.set_tag_for_frame(token, viewport, tags, partial ? 3u : 2u, nullptr);
		    result != sl::Result::eOk && g_presents < 8) {
			LOGF("frame generation: slSetTagForFrame failed (%d)\n", static_cast<int>(result));
		}
		if (HudlessOn()) {
			TagHudless(token, *inputs, game);
		}
	} else {
		// Menus, movies, loading: no valid depth and motion (DLSS-G then presents the frame as is).
		sl::ResourceTag tags[] {
		    sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
		    sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
		};
		g_api.set_tag_for_frame(token, viewport, tags, static_cast<uint32_t>(std::size(tags)), nullptr);
	}
	g_api.pcl_set_marker(sl::PCLMarker::eRenderSubmitEnd, token);
	g_api.pcl_set_marker(sl::PCLMarker::ePresentStart, token);
#else
	(void)inputs;
	(void)backbuffer;
	(void)region;
	(void)format;
	(void)image_count;
#endif
}

void AfterPresent() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!g_ready || g_fsr || g_token == nullptr) {
		return;
	}
	g_api.pcl_set_marker(sl::PCLMarker::ePresentEnd, *g_token);
	const auto presents = ++g_presents;
	if (presents % 30 != 1) {
		return;
	}
	sl::DLSSGState state {};
	if (g_api.dlssg_get_state(sl::ViewportHandle {0}, state, nullptr) != sl::Result::eOk) {
		return;
	}
	g_presented_frames.store(std::max(state.numFramesActuallyPresented, 1u), std::memory_order_relaxed);
	// More generated frames asked for than the GPU makes (the launcher's 3x and up): as many as it makes.
	if (state.numFramesToGenerateMax != 0 && static_cast<uint32_t>(g_generate) > state.numFramesToGenerateMax) {
		LOGF("frame generation: %d generated frames asked for, the GPU makes up to %u\n", g_generate,
		     state.numFramesToGenerateMax);
		std::printf("Frame generation: %d generated frame(s) asked for, the GPU makes up to %u\n", g_generate,
		            state.numFramesToGenerateMax);
		g_generate    = static_cast<int>(state.numFramesToGenerateMax);
		g_options_set = false;
	}
	if (presents % 600 == 1) {
		LOGF("frame generation: status=0x%x presented=%u max_generated=%u vram=%" PRIu64 " MiB\n",
		     static_cast<uint32_t>(state.status), state.numFramesActuallyPresented,
		     state.numFramesToGenerateMax, state.estimatedVRAMUsageInBytes >> 20u);
		std::printf("Frame generation: status=0x%x presented=%u max_generated=%u\n",
		            static_cast<uint32_t>(state.status), state.numFramesActuallyPresented,
		            state.numFramesToGenerateMax);
		std::fflush(stdout);
	}
#endif
}

void SwapchainChanging() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	// Off before the swapchain is made again (DLSS-G guide: before a resize or a full screen change).
	if (!g_ready || g_fsr || !g_mode_on) return;
	sl::DLSSGOptions options {};
	options.mode                = sl::DLSSGMode::eOff;
	options.flags               = sl::DLSSGFlags::eRetainResourcesWhenOff;
	options.numFramesToGenerate = static_cast<uint32_t>(g_generate);
	if (const auto result = g_api.dlssg_set_options(sl::ViewportHandle {0}, options); result != sl::Result::eOk)
		LOGF("frame generation: slDLSSGSetOptions (off) failed (%d)\n", static_cast<int>(result));
	g_mode_on     = false;
	g_run_with    = 0;
	g_options_set = false; // (the next present sends its sizes again)
#elif defined(KYTY_FRAMEGEN_NGX)
	g_fg_history = false; // (the next frame starts DLSS-G again)
#endif
}

void NoteDroppedFrame() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	g_discontinuity.store(true, std::memory_order_relaxed);
#elif defined(KYTY_FRAMEGEN_NGX)
	g_fg_discontinuity.store(true, std::memory_order_relaxed);
#endif
#ifdef KYTY_FRAMEGEN_FSR
	g_fsr_discontinuity.store(true, std::memory_order_relaxed);
#endif
	static std::atomic<uint32_t> dropped {0};
	if (const auto count = dropped.fetch_add(1, std::memory_order_relaxed) + 1; count <= 8 || count % 600 == 0)
		LOGF("frame generation: %u frame(s) with inputs left unshown (the next resets DLSS-G's history)\n", count);
}

uint32_t PresentedFrames() {
#ifdef KYTY_FRAMEGEN_FSR
	if (g_fsr) return g_fsr_state.mode_on ? 2u : 1u;
#endif
	return g_presented_frames.load(std::memory_order_relaxed);
}

void SetStaticRect(float x0, float y0, float x1, float y1) {
	const auto pack = [](float value, uint32_t index) {
		return static_cast<uint64_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f)) << (16u * index);
	};
	g_static_rect.store(pack(x0, 0) | pack(y0, 1) | pack(x1, 2) | pack(y1, 3), std::memory_order_relaxed);
}

void OnDraw(uint64_t vs_hash, std::span<const uint32_t> srt) {
	const auto& profile = ::GameProfile::Current();
	const uint32_t SrtViewProj = profile.srt_view_proj, SrtPrevViewProj = profile.srt_prev_view_proj,
	               SrtJitter = profile.srt_jitter;
	if (g_camera.valid || !profile.HasCamera() || srt.size() < std::max({SrtViewProj + 16u, SrtPrevViewProj + 12u, SrtJitter + 2u}) ||
	    !IsGbufferVertexShader(vs_hash)) {
		return;
	}
	for (uint32_t i = 0; i < 16; ++i) {
		g_camera.view_proj[i] = AsFloat(srt[SrtViewProj + i]);
	}
	// The previous matrix has rows x, y, w; its z row is taken from the current one.
	for (uint32_t i = 0; i < 4; ++i) {
		g_camera.prev_view_proj[i]      = AsFloat(srt[SrtPrevViewProj + i]);
		g_camera.prev_view_proj[4 + i]  = AsFloat(srt[SrtPrevViewProj + 4 + i]);
		g_camera.prev_view_proj[8 + i]  = g_camera.view_proj[8 + i];
		g_camera.prev_view_proj[12 + i] = AsFloat(srt[SrtPrevViewProj + 8 + i]);
	}
	g_camera.jitter_ndc = {AsFloat(srt[SrtJitter]), AsFloat(srt[SrtJitter + 1])};
	g_camera.valid      = true;
}

void NoteScene() {
	g_scene.store(true, std::memory_order_relaxed);
}

static bool g_cut = false; // Kyty.Gpu: the current guest frame's upscale started a new history (NoteCut)

void NoteCut() {
	g_cut = true;
}

bool TakeScene() {
	const bool scene = g_scene.exchange(false, std::memory_order_relaxed);
	g_last_scene.store(scene, std::memory_order_relaxed);
	g_flips_taken.fetch_add(1, std::memory_order_relaxed);
	return scene;
}

uint64_t GuestFlips() {
	return g_flips_taken.load(std::memory_order_relaxed);
}

bool SceneNoted() {
	return g_scene.load(std::memory_order_relaxed) || g_last_scene.load(std::memory_order_relaxed);
}

void OnDispatch(uint64_t cs_hash, ImageId depth, uint64_t depth_address, ImageId motion,
                uint64_t motion_address) {
	if (!IsTaaShader(cs_hash)) {
		return;
	}
	g_sources = {depth, motion, depth_address, motion_address, true};
}

void OnLateDispatch(ImageId depth, uint64_t depth_address, ImageId motion, uint64_t motion_address, vk::Extent2D extent) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!g_late_ready) return;
	if (uint64_t {extent.width} * extent.height > uint64_t {g_late_largest.width} * g_late_largest.height)
		g_late_largest = extent;
	if (extent == g_late_largest) g_late_sources = {depth, motion, depth_address, motion_address, extent};
#else
	(void)depth;
	(void)depth_address;
	(void)motion;
	(void)motion_address;
	(void)extent;
#endif
}

// Sub-pixel jitter for a game that has none (its profile's direct_motion_target): the scene passes' viewports are moved by
// this many pixels (SetGraphicsDynamicParams), so DLSS sees a different sampling of the picture each frame; Halton(2,3),
// 16 phases, one per late DLSS evaluation. KYTY_LATE_JITTER=0 off, KYTY_LATE_JITTER_SIGN=-1 flips the direction told to
// DLSS (a comparison), KYTY_LATE_JITTER_SCALE the amplitude in pixels (default 1: +-0.5).
std::atomic<uint32_t> g_jitter_phase {0};

float Halton(uint32_t index, uint32_t base) {
	float f = 1.0f, r = 0.0f;
	while (index > 0) {
		f /= static_cast<float>(base);
		r += f * static_cast<float>(index % base);
		index /= base;
	}
	return r;
}

void JitterFor(uint32_t phase, float& x, float& y) {
	static const float scale = [] {
		const char* value = std::getenv("KYTY_LATE_JITTER_SCALE");
		return value != nullptr && std::atof(value) > 0.0 ? static_cast<float>(std::atof(value)) : 1.0f;
	}();
	const uint32_t index = phase % 16u + 1u;
	x = (Halton(index, 2) - 0.5f) * scale;
	y = (Halton(index, 3) - 0.5f) * scale;
}

std::atomic<bool> g_direct_fg {false};    // the same, for the frame's DLSS-G inputs (PrepareInputs takes it)
std::atomic<bool> g_direct_valid {false}; // this guest frame's G-buffer draw gave late DLSS its images (OnDirectGbuffer)

bool DirectCapture() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return g_late_ready && ::GameProfile::Current().direct_motion_target >= 0;
#else
	return false;
#endif
}

void OnDirectGbuffer(ImageId depth, uint64_t depth_address, ImageId motion, uint64_t motion_address, vk::Extent2D extent) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!g_late_ready) return;
	g_late_sources = {depth, motion, depth_address, motion_address, extent};
	g_late_largest = extent;
	g_direct_valid.store(true, std::memory_order_relaxed);
	g_direct_fg.store(true, std::memory_order_relaxed);
#else
	(void)depth;
	(void)depth_address;
	(void)motion;
	(void)motion_address;
	(void)extent;
#endif
}

// KYTY_CB_DUMP=<trigger file>: when it appears, the first G-buffer draw of 8 consecutive frames writes its vertex shader's
// constant buffers (V#, up to 64 KiB each) and flattened SRT next to the trigger (cb-f<k>-b<i>.bin, cb-f<k>-srt.bin), read
// with ReadProcessMemory (an unmapped range is skipped). Finding the game's jitter (debugging).
void OnDirectGbufferBuffers(uint64_t vs_hash, const std::vector<std::array<uint32_t, 4>>& buffers,
                            const std::vector<uint32_t>& srt) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	static const char* trigger = std::getenv("KYTY_CB_DUMP");
	if (trigger == nullptr) return;
	static int      frames_left = 0;
	static uint64_t last_frame  = UINT64_MAX;
	static uint32_t polls       = 0;
	if (frames_left == 0) {
		if (++polls % 64 != 0) return;
		std::error_code error;
		if (!std::filesystem::exists(trigger, error)) return;
		std::filesystem::rename(trigger, std::filesystem::path(trigger).concat(".done"), error);
		frames_left = 8;
		last_frame  = UINT64_MAX;
	}
	if (g_guest_frame == last_frame) return; // (the frame's first G-buffer draw only)
	last_frame        = g_guest_frame;
	const int  k      = 8 - frames_left;
	const auto folder = std::filesystem::path(trigger).parent_path();
	const auto write  = [&](const std::string& name, const void* data, size_t size) {
		if (std::ofstream file(folder / name, std::ios::binary); file) file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	};
	write("cb-f" + std::to_string(k) + "-srt.bin", srt.data(), srt.size() * 4);
	std::string summary;
	for (size_t i = 0; i < buffers.size() && i < 16; i++) {
		const auto&    v      = buffers[i];
		const uint64_t base   = (v[0] | (static_cast<uint64_t>(v[1]) << 32u)) & 0xFFFFFFFFFFFFull;
		const uint32_t stride = (v[1] >> 16u) & 0x3FFFu;
		const uint64_t size   = std::min<uint64_t>(stride == 0 ? v[2] : static_cast<uint64_t>(stride) * v[2], 65536u);
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		SIZE_T               read = 0;
		if (base == 0 || size == 0 ||
		    !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(base), bytes.data(), bytes.size(), &read)) {
			summary += " b" + std::to_string(i) + ":unreadable";
			continue;
		}
		write("cb-f" + std::to_string(k) + "-b" + std::to_string(i) + ".bin", bytes.data(), read);
		char text[64];
		std::snprintf(text, sizeof(text), " b%zu:0x%llx+%llu", i, static_cast<unsigned long long>(base),
		              static_cast<unsigned long long>(size));
		summary += text;
	}
	std::printf("CB dump frame %d (guest frame %llu, vs 0x%016llx, srt %zu words):%s\n", k,
	            static_cast<unsigned long long>(g_guest_frame), static_cast<unsigned long long>(vs_hash), srt.size(),
	            summary.c_str());
	std::fflush(stdout);
	--frames_left;
#else
	(void)vs_hash;
	(void)buffers;
	(void)srt;
#endif
}

bool LateJitter(uint32_t width, uint32_t height, float& x, float& y) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_LATE_JITTER");
		return value == nullptr || value[0] != '0';
	}();
	if (!enabled || !g_late_ready || g_nis_off.load(std::memory_order_relaxed) || ::GameProfile::Current().direct_motion_target < 0 ||
	    g_late_largest.width != width || g_late_largest.height != height || width == 0) {
		return false;
	}
	JitterFor(g_jitter_phase.load(std::memory_order_relaxed), x, y);
	return true;
#else
	(void)width;
	(void)height;
	(void)x;
	(void)y;
	return false;
#endif
}

void OnUpscale(ImageId depth, uint64_t depth_address, ImageId motion, uint64_t motion_address) {
	g_sources = {depth, motion, depth_address, motion_address, true};
}

void PrepareInputs(GraphicContext& graphics, TextureCache& cache, CommandBuffer& command,
                   FrameInputs& inputs) {
	inputs.valid       = false;
	inputs.cut         = std::exchange(g_cut, false);
	bool       ready   = g_sources.valid && g_camera.valid;
	auto       sources = g_sources;
	auto       camera  = g_camera;
	// A game without a camera of its own (late DLSS's direct G-buffer capture): a fixed camera looking down +z, 60 degrees
	// vertical field of view, near plane 0.1; DLSS-G works from the motion vectors and depth.
	static const bool direct_fg_enabled = [] {
		const char* value = std::getenv("KYTY_DIRECT_FG");
		return value == nullptr || value[0] != '0';
	}();
	const bool direct_frame = g_direct_fg.exchange(false, std::memory_order_relaxed) && direct_fg_enabled &&
	                          ::GameProfile::Current().direct_motion_target >= 0 && g_late_sources.extent.width != 0;
	if (!ready && direct_frame) {
		const float sy = 1.0f / std::tan(0.5f * 1.0471976f);
		const float sx = sy * static_cast<float>(g_late_sources.extent.height) / static_cast<float>(g_late_sources.extent.width);
		sources        = {g_late_sources.depth, g_late_sources.motion, g_late_sources.depth_address, g_late_sources.motion_address, true};
		camera.view_proj      = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 0, 0.1f, 0, 0, 1, 0};
		camera.prev_view_proj = camera.view_proj;
		camera.jitter_ndc     = {0, 0};
		camera.valid          = true;
		ready                 = true;
		inputs.direct         = true;
	}
	g_sources.valid    = false;
	g_camera.valid     = false;
	++g_guest_frame;
	if (!ready) {
		return;
	}
	auto& depth  = cache.GetImage(sources.depth);
	auto& motion = cache.GetImage(sources.motion);
	if (depth.info.data.address != sources.depth_address ||
	    motion.info.data.address != sources.motion_address ||
	    depth.backing.extent.width != motion.backing.extent.width ||
	    depth.backing.extent.height != motion.backing.extent.height) {
		return; // the images were replaced after the TAA pass
	}
	Configure(graphics, inputs.depth, inputs.depth_view, depth.backing);
	Configure(graphics, inputs.motion, inputs.motion_view, motion.backing, &inputs.motion_attachment);
	command.EndRendering();
	auto vk_command = command.Handle();
	CopyInto(vk_command, depth, inputs.depth);
	CopyInto(vk_command, motion, inputs.motion);
	if (inputs.motion_attachment) {
		ZeroStaticRect(vk_command, inputs.motion, inputs.motion_view);
	}
	inputs.render_extent = {motion.backing.extent.width, motion.backing.extent.height};
	inputs.camera        = camera;
	inputs.guest_frame   = g_guest_frame;
	inputs.valid         = true;
#ifdef KYTY_FRAMEGEN_FSR_HUD
	// FSR: the interface canvas as it is at the flip (RecordHudless reads its alpha).
	inputs.ui_valid = false;
	if (const uint64_t canvas = g_ui_canvas.load(std::memory_order_relaxed); (g_fsr || g_fsr_native) && canvas != 0) {
		// (Another image can start at the canvas's address later on - a pass reusing its memory -: the canvas is the
		// one of its size used last, not the first registered there. Each change of state reported, up to 32.)
		static int        state   = 0; // 1: copied, 2: unusable, 3: not in the cache
		static uint32_t   reports = 0;
		static vk::Format format  = vk::Format::eUndefined; // the canvas's, from its first copy
		const vk::Extent2D size  = UiCanvasExtent();
		uint32_t           count = 0;
		auto               id    = cache.FindImageAt(canvas, size.width, size.height, format, &count);
		// (A canvas made again in another format - a game setting changed - is still the canvas: the one of its size used
		// last, its format taken from then on.)
		if (!id && format != vk::Format::eUndefined) id = cache.FindImageAt(canvas, size.width, size.height, vk::Format::eUndefined, &count);
		int                now   = 3;
		if (id) {
			auto& ui = cache.GetImage(id);
			now      = 2;
			if (!IsDepthFormat(ui.backing.format)) {
				// (At most 1920x1080, the 4K canvas at half its size: its alpha is a mask, read a few pixels wide; a
				// quarter of the video memory.)
				bool failed = false;
				Configure(graphics, inputs.ui, inputs.ui_view, ui.backing, nullptr,
				          vk::Extent2D {std::min(size.width, 1920u), std::min(size.height, 1080u)}, &failed);
				if (!failed) {
					BlitInto(vk_command, ui, inputs.ui);
					inputs.ui_valid = true;
					now             = 1;
					format          = ui.backing.format;
				}
			}
			if (now != state && reports < 32) {
				++reports;
				std::printf("Frame generation: FSR HUD canvas %s (%ux%u, format %d; %u image(s) at 0x%" PRIx64 ")\n",
				            now == 1 ? "copied" : "image unusable", ui.backing.extent.width, ui.backing.extent.height,
				            static_cast<int>(ui.backing.format), count, canvas);
				std::fflush(stdout);
			}
		} else if (now != state && reports < 32) {
			++reports;
			std::printf("Frame generation: FSR HUD canvas at 0x%" PRIx64 " not in the texture cache as %ux%u (%u image(s) there)\n",
			            canvas, size.width, size.height, count);
			std::fflush(stdout);
		}
		state = now;
	} else if ((g_fsr || g_fsr_native) && canvas == 0) {
		static uint32_t scenes = 0;
		if (++scenes == 300) {
			std::printf("Frame generation: no FSR HUD canvas seen in 300 frames with inputs\n");
			std::fflush(stdout);
		}
	}
#endif
}

bool UpscalerReady() {
	return g_upscale_ready;
}

bool Upscale(vk::CommandBuffer command, const UpscaleInputs& inputs, const vk::detail::DispatchLoaderDynamic& dispatch) {
	(void)dispatch;
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!g_upscale_ready) return false;
	// Its own viewport: DLSS-G's (0) takes its constants and tags at the present.
	const sl::ViewportHandle viewport {1};
	// DLSS given up (the game's image stretched instead, renderSuperResolution.cpp): an input outside the sizes the
	// mode takes (the NVIDIA app's DLSS override of the program can ask for another scale: 3802x2138-3840x2160 for a
	// 1920x1080 input, issue #87), or NGX refusing its evaluations.
	static bool given_up = false;
	if (given_up) return false;
	if (g_dlss_refusals.load(std::memory_order_relaxed) >= 3) {
		given_up = true;
		std::printf("DLSS super resolution: NGX refused it %u times (Streamline errors above): the game's image is "
		            "upscaled without it from here on\n",
		            g_dlss_refusals.load(std::memory_order_relaxed));
		std::fflush(stdout);
		return false;
	}
	static vk::Extent2D options_render {}, options_output {};
	if (options_render != inputs.color.extent || options_output != inputs.output.extent) {
		// The mode by the input's share of the output's height (DLSS takes any input size; the mode picks its model and
		// limits): PS5 Pro's 1512 of 2160 and 1440 are quality, 1080 performance, the output's own DLAA.
		const double share = static_cast<double>(inputs.color.extent.height) / inputs.output.extent.height;
		sl::DLSSOptions options {};
		options.mode = share >= 0.95   ? sl::DLSSMode::eDLAA
		               : share >= 0.62 ? sl::DLSSMode::eMaxQuality
		               : share >= 0.55 ? sl::DLSSMode::eBalanced
		               : share >= 0.45 ? sl::DLSSMode::eMaxPerformance
		                               : sl::DLSSMode::eUltraPerformance;
		options.outputWidth     = inputs.output.extent.width;
		options.outputHeight    = inputs.output.extent.height;
		options.colorBuffersHDR = sl::Boolean::eTrue; // (the game's linear HDR scene, before its tone mapping)
		options.useAutoExposure = sl::Boolean::eTrue;
		// KYTY_DLSS_PRESET=<letter> (a test): that DLSS model for every mode instead of the mode's default.
		if (const char* preset = std::getenv("KYTY_DLSS_PRESET"); preset != nullptr && *preset >= 'A' && *preset <= 'O') {
			const auto value = static_cast<sl::DLSSPreset>(*preset - 'A' + 1);
			options.dlaaPreset = options.qualityPreset = options.balancedPreset = options.performancePreset =
			    options.ultraPerformancePreset = options.ultraQualityPreset = value;
		}
		if (const auto result = g_api.dlss_set_options(viewport, options); result != sl::Result::eOk) {
			std::printf("DLSS super resolution: slDLSSSetOptions failed (%s)\n", sl::getResultAsStr(result));
			std::fflush(stdout);
			return false;
		}
		sl::DLSSOptimalSettings settings {};
		(void)g_api.dlss_get_optimal_settings(options, settings);
		std::printf("DLSS super resolution: %ux%u -> %ux%u, mode %u (its range %ux%u-%ux%u)\n", inputs.color.extent.width,
		            inputs.color.extent.height, inputs.output.extent.width, inputs.output.extent.height,
		            static_cast<uint32_t>(options.mode), settings.renderWidthMin, settings.renderHeightMin,
		            settings.renderWidthMax, settings.renderHeightMax);
		std::fflush(stdout);
		if (settings.renderWidthMax != 0 && settings.renderHeightMax != 0 &&
		    (inputs.color.extent.width < settings.renderWidthMin || inputs.color.extent.width > settings.renderWidthMax ||
		     inputs.color.extent.height < settings.renderHeightMin || inputs.color.extent.height > settings.renderHeightMax)) {
			given_up = true;
			std::printf("DLSS super resolution: the game renders %ux%u, outside what DLSS takes in this mode: the game's "
			            "image is upscaled without it. A DLSS override for this program in the NVIDIA app (Super Resolution "
			            "mode or scaling ratio) does this; set it back to the default for kyty_emulator.exe\n",
			            inputs.color.extent.width, inputs.color.extent.height);
			std::fflush(stdout);
			return false;
		}
		options_render = inputs.color.extent;
		options_output = inputs.output.extent;
	}
	static uint32_t frame = 0;
	++frame;
	sl::FrameToken* token = nullptr;
	if (NewFrameToken(token, &frame) != sl::Result::eOk || token == nullptr) return false;

	sl::Constants constants {};
	Mat4          identity;
	for (uint32_t i = 0; i < 4; ++i) {
		identity[i] = sl::float4(i == 0, i == 1, i == 2, i == 3);
	}
	Mat4 view_to_clip;
	for (uint32_t i = 0; i < 4; ++i) {
		view_to_clip[i] = sl::float4(inputs.view_to_clip[i * 4], inputs.view_to_clip[i * 4 + 1], inputs.view_to_clip[i * 4 + 2],
		                             inputs.view_to_clip[i * 4 + 3]);
	}
	constants.cameraViewToClip = view_to_clip;
	if (!Inverted(view_to_clip, constants.clipToCameraView)) constants.clipToCameraView = identity;
	constants.clipToLensClip      = identity;
	constants.clipToPrevClip      = identity;
	constants.prevClipToClip      = identity;
	constants.jitterOffset        = sl::float2(inputs.jitter_x, inputs.jitter_y);
	// (Streamline's motion vectors are in render-size UV units: its scale makes them so from the game's pixels.)
	constants.mvecScale = sl::float2(inputs.motion_scale_x / static_cast<float>(inputs.color.extent.width),
	                                 inputs.motion_scale_y / static_cast<float>(inputs.color.extent.height));
	constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
	constants.cameraPos           = sl::float3(0, 0, 0);
	constants.cameraUp            = sl::float3(0, 1, 0);
	constants.cameraRight         = sl::float3(1, 0, 0);
	constants.cameraFwd           = sl::float3(0, 0, 1);
	constants.cameraNear          = inputs.near_plane;
	constants.cameraFar           = inputs.far_plane;
	constants.cameraFOV           = inputs.view_to_clip[5] != 0.0f ? 2.0f * std::atan(1.0f / std::fabs(inputs.view_to_clip[5])) : 1.0f;
	constants.cameraAspectRatio   = static_cast<float>(inputs.color.extent.width) / static_cast<float>(inputs.color.extent.height);
	constants.motionVectorsInvalidValue = sl::INVALID_FLOAT;
	constants.depthInverted             = sl::Boolean::eTrue;
	constants.cameraMotionIncluded      = sl::Boolean::eTrue;
	constants.motionVectors3D           = sl::Boolean::eFalse;
	constants.reset                     = inputs.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	constants.orthographicProjection    = sl::Boolean::eFalse;
	constants.motionVectorsDilated      = sl::Boolean::eFalse;
	constants.motionVectorsJittered     = sl::Boolean::eFalse;
	if (const auto result = g_api.set_constants(constants, *token, viewport); result != sl::Result::eOk) {
		static std::atomic<uint32_t> reports {0};
		if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
			std::printf("DLSS super resolution: slSetConstants failed (%s)\n", sl::getResultAsStr(result));
		return false;
	}
	const auto resource = [](const UpscaleImage& image) {
		sl::Resource r(sl::ResourceType::eTex2d, static_cast<VkImage>(image.image), nullptr, static_cast<VkImageView>(image.view),
		               static_cast<uint32_t>(static_cast<VkImageLayout>(image.layout)));
		r.width        = image.extent.width;
		r.height       = image.extent.height;
		r.nativeFormat = static_cast<uint32_t>(static_cast<VkFormat>(image.format));
		r.mipLevels    = 1;
		r.arrayLayers  = 1;
		r.flags        = 0;
		r.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(image.usage));
		return r;
	};
	auto             color  = resource(inputs.color);
	auto             depth  = resource(inputs.depth);
	auto             motion = resource(inputs.motion);
	auto             output = resource(inputs.output);
	// (KYTY_SR_LOG: the first two evaluations' images and constants.)
	static const bool describe = std::getenv("KYTY_SR_LOG") != nullptr;
	if (static std::atomic<uint32_t> described {0}; describe && described.fetch_add(1, std::memory_order_relaxed) < 2) {
		const std::pair<const char*, const UpscaleImage*> images[] {
		    {"color", &inputs.color}, {"depth", &inputs.depth}, {"motion", &inputs.motion}, {"output", &inputs.output}};
		for (const auto& [name, image]: images) {
			std::printf("DLSS super resolution: %s image %p view %p layout %d format %d %ux%u usage 0x%x\n", name,
			            static_cast<void*>(static_cast<VkImage>(image->image)),
			            static_cast<void*>(static_cast<VkImageView>(image->view)), static_cast<int>(image->layout),
			            static_cast<int>(image->format), image->extent.width, image->extent.height,
			            static_cast<uint32_t>(static_cast<VkImageUsageFlags>(image->usage)));
		}
		std::printf("DLSS super resolution: jitter %g %g, motion scale %g %g, reset %d, near %g far %g\n", inputs.jitter_x,
		            inputs.jitter_y, inputs.motion_scale_x, inputs.motion_scale_y, inputs.reset ? 1 : 0, inputs.near_plane,
		            inputs.far_plane);
		std::fflush(stdout);
	}
	const sl::Extent render {0, 0, inputs.color.extent.width, inputs.color.extent.height};
	const sl::Extent full {0, 0, inputs.output.extent.width, inputs.output.extent.height};
	const sl::ResourceTag tags[] {
	    sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &render),
	    sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &full),
	    sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &render),
	    sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &render),
	};
	auto* command_buffer = reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(command));
	if (const auto result = g_api.set_tag_for_frame(*token, viewport, tags, static_cast<uint32_t>(std::size(tags)), command_buffer);
	    result != sl::Result::eOk) {
		static std::atomic<uint32_t> reports {0};
		if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
			std::printf("DLSS super resolution: slSetTagForFrame failed (%s)\n", sl::getResultAsStr(result));
		return false;
	}
	const sl::BaseStructure* evaluate_inputs[] {&viewport};
	// (Result::eWarnOutOfVRAM is not a failure: Streamline returns it in place of eOk, DLSS recorded, while the process
	// uses all the video memory Windows budgets it - issue #85: an RTX 2060 at 5030 of 5187 MiB had DLSS's image
	// stretched over from then on.)
	if (const auto result = g_api.evaluate_feature(sl::kFeatureDLSS, *token, evaluate_inputs, 1, command_buffer);
	    result == sl::Result::eWarnOutOfVRAM) {
		static std::atomic<bool> reported {false};
		if (!reported.exchange(true, std::memory_order_relaxed))
			std::printf("DLSS super resolution: the video memory budget is used up (Streamline's warning); DLSS still runs\n");
	} else if (result != sl::Result::eOk) {
		static std::atomic<uint32_t> reports {0};
		if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
			std::printf("DLSS super resolution: slEvaluateFeature failed (%s)\n", sl::getResultAsStr(result));
		return false;
	}
	return true;
#elif defined(KYTY_FRAMEGEN_NGX)
	if (!g_upscale_ready || g_dlss == nullptr) return false;
	// (The renderer thread, or the recording worker replaying in order: one at a time.)
	static vk::Extent2D created_render {}, created_output {};
	static bool         created = false, failed = false;
	static uint32_t     reports = 0;
	if (failed) return false; // (the stretch, every frame, without asking NGX again)
	if (!created || created_render != inputs.color.extent || created_output != inputs.output.extent) {
		// The mode by the input's share of the output's height, as Windows' (Streamline) way: PS5 Pro's 1512 of 2160 and
		// 1440 are quality, 1080 performance, the output's own DLAA.
		const double    share = static_cast<double>(inputs.color.extent.height) / inputs.output.extent.height;
		KytyDlssFeature feature {};
		feature.input_width    = inputs.color.extent.width;
		feature.input_height   = inputs.color.extent.height;
		feature.output_width   = inputs.output.extent.width;
		feature.output_height  = inputs.output.extent.height;
		feature.quality        = share >= 0.95 ? 0 : share >= 0.62 ? 1 : share >= 0.55 ? 2 : share >= 0.45 ? 3 : 4;
		feature.depth_inverted = 1;
		feature.hdr            = 1; // (the game's linear HDR scene, before its tone mapping; automatic exposure)
		// KYTY_DLSS_PRESET=<letter> (a test): that DLSS model for every mode instead of the mode's default.
		if (const char* preset = std::getenv("KYTY_DLSS_PRESET"); preset != nullptr && *preset >= 'A' && *preset <= 'O')
			feature.preset = static_cast<uint32_t>(*preset - 'A' + 1);
		if (g_dlss->CreateFeature(static_cast<VkCommandBuffer>(command), &feature) == 0) {
			std::printf("DLSS super resolution: NGX did not make the feature for %ux%u -> %ux%u (the stretch instead)\n",
			            feature.input_width, feature.input_height, feature.output_width, feature.output_height);
			std::fflush(stdout);
			failed = true;
			return false;
		}
		static constexpr const char* Modes[] {"DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
		std::printf("DLSS super resolution: %ux%u -> %ux%u, %s (NVIDIA NGX)\n", feature.input_width, feature.input_height,
		            feature.output_width, feature.output_height, Modes[feature.quality]);
		std::fflush(stdout);
		created        = true;
		created_render = inputs.color.extent;
		created_output = inputs.output.extent;
	}
	static auto last = std::chrono::steady_clock::now();
	const auto  now  = std::chrono::steady_clock::now();
	const float frame_ms =
	    std::clamp(std::chrono::duration<float, std::milli>(now - last).count(), 0.0f, 100.0f);
	last = now;

	KytyDlssEvaluate evaluate {};
	evaluate.color  = BridgeImage(inputs.color, vk::ImageAspectFlagBits::eColor);
	evaluate.depth  = BridgeImage(inputs.depth, IsDepthFormat(inputs.depth.format) ? vk::ImageAspectFlagBits::eDepth
	                                                                               : vk::ImageAspectFlagBits::eColor);
	evaluate.motion = BridgeImage(inputs.motion, vk::ImageAspectFlagBits::eColor);
	evaluate.output = BridgeImage(inputs.output, vk::ImageAspectFlagBits::eColor);
	evaluate.jitter_x   = inputs.jitter_x; // (render pixels, as NGX takes them)
	evaluate.jitter_y   = inputs.jitter_y;
	evaluate.mv_scale_x = inputs.motion_scale_x; // (the game's vectors to render pixels, NGX's units)
	evaluate.mv_scale_y = inputs.motion_scale_y;
	evaluate.reset      = inputs.reset ? 1 : 0;
	evaluate.frame_ms   = frame_ms;
	// KYTY_DLSS_SHARPNESS=0..1 (a test): NGX's own sharpening of the output.
	static const float sharpness = [] {
		const char* value = std::getenv("KYTY_DLSS_SHARPNESS");
		return value != nullptr ? std::clamp(static_cast<float>(std::atof(value)), 0.0f, 1.0f) : 0.0f;
	}();
	evaluate.sharpness = sharpness;
	// (KYTY_SR_LOG: the first two evaluations' images and constants.)
	static const bool describe = std::getenv("KYTY_SR_LOG") != nullptr;
	if (describe && reports < 2) {
		++reports;
		const std::pair<const char*, const UpscaleImage*> images[] {
		    {"color", &inputs.color}, {"depth", &inputs.depth}, {"motion", &inputs.motion}, {"output", &inputs.output}};
		for (const auto& [name, image]: images) {
			std::printf("DLSS super resolution: %s image %p view %p layout %d format %d %ux%u level %u layer %u\n", name,
			            static_cast<void*>(static_cast<VkImage>(image->image)),
			            static_cast<void*>(static_cast<VkImageView>(image->view)), static_cast<int>(image->layout),
			            static_cast<int>(image->format), image->extent.width, image->extent.height, image->base_level,
			            image->base_layer);
		}
		std::printf("DLSS super resolution: jitter %g %g, motion scale %g %g, reset %d, %.2f ms\n", inputs.jitter_x,
		            inputs.jitter_y, inputs.motion_scale_x, inputs.motion_scale_y, inputs.reset ? 1 : 0, frame_ms);
		std::fflush(stdout);
	}
	// The depth in the layout NGX samples it in, for the evaluation only.
	const bool depth_moved =
	    IsDepthFormat(inputs.depth.format) && inputs.depth.layout != vk::ImageLayout::eShaderReadOnlyOptimal;
	if (depth_moved)
		DepthLayout(command, inputs.depth, inputs.depth.layout, vk::ImageLayout::eShaderReadOnlyOptimal, dispatch);
	const bool done = g_dlss->Evaluate(static_cast<VkCommandBuffer>(command), &evaluate) != 0;
	if (depth_moved)
		DepthLayout(command, inputs.depth, vk::ImageLayout::eShaderReadOnlyOptimal, inputs.depth.layout, dispatch);
	if (!done) {
		// (As the Streamline way, issue #87: NGX refusing its evaluations three times gives DLSS up for good, the game's
		// image stretched from then on.)
		static uint32_t failures = 0;
		if (++failures >= 3) {
			failed = true;
			std::printf("DLSS super resolution: NGX refused it %u times: the game's image is upscaled without it from here "
			            "on\n",
			            failures);
		} else {
			std::printf("DLSS super resolution: NGX's evaluation failed (the stretch for this frame)\n");
		}
		std::fflush(stdout);
	}
	return done;
#else
	(void)command;
	(void)inputs;
	return false;
#endif
}

void DestroyInputs(GraphicContext& graphics, FrameInputs& inputs) {
	for (auto* view: {&inputs.depth_view, &inputs.motion_view, &inputs.ui_view}) {
		if (*view != nullptr) {
			graphics.device.destroyImageView(*view, nullptr);
			*view = nullptr;
		}
	}
	inputs.ui_valid = false;
	for (auto* image: {&inputs.depth, &inputs.motion, &inputs.ui}) {
		if (image->image != nullptr) {
			graphics.DeleteImage(*image);
		}
	}
	DestroyHudlessInputs(graphics, inputs);
	inputs.valid = false;
}

// ---- KYTY_DLSS (DLAA in place of the TAA) and KYTY_NIS (scaling before the final composite) ----

bool DlssEnabled() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return g_ready && g_dlss_ready && !g_dlss_off.load(std::memory_order_relaxed);
#else
	return false;
#endif
}

bool NisAvailable() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return g_ready && (g_nis_ready || g_late_ready);
#else
	return false;
#endif
}

namespace {
std::atomic<uint64_t> g_nis_scene_extent {0}; // width << 32 | height
} // namespace

vk::Extent2D NisSceneExtent() {
	const uint64_t packed = g_nis_scene_extent.load(std::memory_order_relaxed);
	return {static_cast<uint32_t>(packed >> 32u), static_cast<uint32_t>(packed & 0xffffffffu)};
}

int NisHdrCompositeSlot(uint64_t pixel_shader) {
	if (!Config::HdrEnabled()) return -1;
	static const std::vector<std::pair<uint64_t, int>> composites = [] {
		std::vector<std::pair<uint64_t, int>> list {{0x3359df0b33704924ULL, 1}};
		if (const char* value = std::getenv("KYTY_NIS_SHADERS"); value != nullptr) {
			std::string_view text = value;
			while (!text.empty()) {
				const auto comma = text.find(',');
				const auto item  = std::string(text.substr(0, comma));
				text             = comma == std::string_view::npos ? std::string_view {} : text.substr(comma + 1);
				const auto colon = item.find(':');
				const uint64_t hash = std::strtoull(item.c_str(), nullptr, 16);
				const int      slot = colon != std::string::npos ? std::atoi(item.c_str() + colon + 1) : 1;
				if (hash != 0 && slot >= 0 && slot < 16) list.emplace_back(hash, slot);
			}
		}
		std::printf("Frame generation: NIS in HDR takes %zu more composite shader(s) (KYTY_NIS_SHADERS)\n", list.size());
		std::fflush(stdout);
		return list;
	}();
	for (const auto& [hash, slot]: composites)
		if (hash == pixel_shader) return slot;
	return -1;
}

bool NisEnabled() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return g_ready && (g_nis_ready || g_late_ready) && !g_nis_off.load(std::memory_order_relaxed);
#else
	return false;
#endif
}

bool LateAaEnabled() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	// (Not once late DLSS gave up or stopped making pictures: the TAA's copy would be shown jittered and aliased.)
	return g_ready && g_late_ready && g_late_aa && !g_nis_off.load(std::memory_order_relaxed) && g_late_failures < LateFailureLimit &&
	       g_late_unresolved < LateUnresolvedLimit;
#else
	return false;
#endif
}

#ifdef KYTY_FRAMEGEN_STREAMLINE
namespace {

// Streamline records into the command buffer on this thread, straight to the driver: the recording worker
// (KYTY_VULKAN_RECORDING) first records what this thread queued, and the barriers around Streamline's work are
// recorded directly too (the renderer's barrier dedupe does not see Streamline's work).
const vk::detail::DispatchLoaderDynamic& Direct() {
#ifdef KYTY_LOCAL_VULKAN_RECORDING
	return LocalVulkanRecording::DirectDispatch();
#else
	return VULKAN_HPP_DEFAULT_DISPATCHER;
#endif
}

// The recording worker's queue played (this thread's commands before Streamline's), and the bound pipelines and dynamic
// state its draw replays reuse forgotten (Streamline binds its own): what a direct call does (ReplayInline).
void DrainRecording() {
#ifdef KYTY_LOCAL_VULKAN_RECORDING
	LocalVulkanRecording::ReplayInline([](std::span<const LocalVulkanRecording::Segment>,
	                                      const vk::detail::DispatchLoaderDynamic&) {},
	                                   {});
#endif
}

void DirectFullBarrier(vk::CommandBuffer command) {
	vk::MemoryBarrier2 barrier {};
	barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	vk::DependencyInfo dependency {};
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency, Direct());
}

void DirectLayout(vk::CommandBuffer command, VulkanImage& image, vk::ImageLayout layout) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	barrier.oldLayout           = image.state.layout;
	barrier.newLayout           = layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image.image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency, Direct());
	image.state = {vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite, layout};
}

sl::Resource SlResource(const SrImage& image) {
	sl::Resource resource(sl::ResourceType::eTex2d, static_cast<VkImage>(image.image), nullptr,
	                      static_cast<VkImageView>(image.view), static_cast<uint32_t>(image.layout));
	resource.width        = image.extent.width;
	resource.height       = image.extent.height;
	resource.nativeFormat = static_cast<uint32_t>(image.format);
	resource.mipLevels    = 1;
	resource.arrayLayers  = 1;
	resource.flags        = 0;
	resource.usage        = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(image.usage));
	return resource;
}

// Streamline's evaluation recorded its work: eWarnOutOfVRAM only warns that the video memory budget is exceeded
// (sl.common checks it after a successful evaluation).
bool Evaluated(sl::Result result) {
	return result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM;
}

// The renderer thread's frame token: the present thread's current index (see g_present_index).
sl::FrameToken* RendererToken() {
	sl::FrameToken* token = nullptr;
	uint32_t        index = g_present_index.load(std::memory_order_acquire);
	if (NewFrameToken(token, &index) != sl::Result::eOk) return nullptr;
	return token;
}

// Constants without a camera (NIS needs none, but each frame's viewport gets a set).
sl::Constants IdentityConstants() {
	sl::Constants constants {};
	Mat4          identity;
	for (uint32_t i = 0; i < 4; ++i) identity[i] = sl::float4(i == 0, i == 1, i == 2, i == 3);
	constants.cameraViewToClip          = identity;
	constants.clipToCameraView          = identity;
	constants.clipToLensClip            = identity;
	constants.clipToPrevClip            = identity;
	constants.prevClipToClip            = identity;
	constants.jitterOffset              = sl::float2(0.0f, 0.0f);
	constants.mvecScale                 = sl::float2(1.0f, 1.0f);
	constants.cameraPinholeOffset       = sl::float2(0.0f, 0.0f);
	constants.cameraPos                 = sl::float3(0, 0, 0);
	constants.cameraUp                  = sl::float3(0, 1, 0);
	constants.cameraRight               = sl::float3(1, 0, 0);
	constants.cameraFwd                 = sl::float3(0, 0, 1);
	constants.cameraNear                = 0.1f;
	constants.cameraFar                 = 10000.0f;
	constants.cameraFOV                 = 1.0f;
	constants.cameraAspectRatio         = 16.0f / 9.0f;
	constants.motionVectorsInvalidValue = sl::INVALID_FLOAT;
	constants.depthInverted             = sl::Boolean::eTrue;
	constants.cameraMotionIncluded      = sl::Boolean::eTrue;
	constants.motionVectors3D           = sl::Boolean::eFalse;
	constants.reset                     = sl::Boolean::eFalse;
	constants.orthographicProjection    = sl::Boolean::eFalse;
	constants.motionVectorsDilated      = sl::Boolean::eFalse;
	constants.motionVectorsJittered     = sl::Boolean::eFalse;
	return constants;
}

// The same camera, jitter and motion-vector conventions as frame generation's (BeforePresent).
sl::Constants CameraConstants(const Camera& input, float width, float height, bool reset) {
	sl::Constants constants = IdentityConstants();
	constants.reset         = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	const auto camera       = Decompose(input.view_proj);
	if (!camera.valid) return constants;
	Mat4 projection;
	projection[0]              = sl::float4(camera.sx, 0, 0, 0);
	projection[1]              = sl::float4(0, camera.sy, 0, 0);
	projection[2]              = sl::float4(camera.cx, camera.cy, 0, 1);
	projection[3]              = sl::float4(0, 0, camera.z_near, 0);
	constants.cameraViewToClip = projection;
	Inverted(projection, constants.clipToCameraView);
	const auto current  = Transposed(input.view_proj);
	const auto previous = Transposed(input.prev_view_proj);
	Mat4       clip_to_world;
	if (Inverted(current, clip_to_world)) {
		constants.clipToPrevClip = Multiply(clip_to_world, previous);
		Inverted(constants.clipToPrevClip, constants.prevClipToClip);
	}
	constants.jitterOffset = sl::float2(input.jitter_ndc[0] * width * 0.5f, -input.jitter_ndc[1] * height * 0.5f);
	// KYTY_DLSS_JITTER_SIGN=-1: the jitter the other way (a comparison).
	static const float jitter_sign = [] {
		const char* sign = std::getenv("KYTY_DLSS_JITTER_SIGN");
		return sign != nullptr && std::atof(sign) < 0 ? -1.0f : 1.0f;
	}();
	constants.jitterOffset = sl::float2(constants.jitterOffset.x * jitter_sign, constants.jitterOffset.y * jitter_sign);
	static const float mv_sign = [] {
		const char* sign = std::getenv("KYTY_FRAMEGEN_MV_SIGN");
		return sign != nullptr && std::atof(sign) > 0 ? 1.0f : -1.0f;
	}();
	constants.mvecScale         = sl::float2(mv_sign, mv_sign);
	constants.cameraPos         = camera.position;
	constants.cameraUp          = camera.up;
	constants.cameraRight       = camera.right;
	constants.cameraFwd         = camera.forward;
	constants.cameraNear        = camera.z_near;
	constants.cameraFar         = 100000.0f;
	constants.cameraFOV         = 2.0f * std::atan(1.0f / std::fabs(camera.sy));
	constants.cameraAspectRatio = std::fabs(camera.sy / camera.sx);
	return constants;
}

// Late DLSS of a game without a camera to read (the profile's direct_motion_target): no matrices, no jitter. The motion
// vectors' unit: KYTY_DIRECT_MV=pixel (default: pixels of the render size), uv (0..1 of the picture), ndc (-1..1);
// KYTY_DIRECT_MV_SIGN=-1 flips them; KYTY_DIRECT_DEPTH_INVERTED=0 for a depth that grows with distance.
sl::Constants DirectConstants(float width, float height, bool reset) {
	sl::Constants constants = IdentityConstants();
	constants.reset         = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	const char* unit        = std::getenv("KYTY_DIRECT_MV");
	float       sx = 1.0f / width, sy = 1.0f / height;
	if (unit != nullptr && std::string_view {unit} == "uv") sx = sy = 1.0f;
	if (unit != nullptr && std::string_view {unit} == "ndc") sx = sy = 0.5f;
	const char* sign = std::getenv("KYTY_DIRECT_MV_SIGN");
	const float flip = sign != nullptr && std::atof(sign) < 0 ? -1.0f : 1.0f;
	constants.mvecScale = sl::float2(sx * flip, sy * flip);
	if (const char* inverted = std::getenv("KYTY_DIRECT_DEPTH_INVERTED"); inverted != nullptr && inverted[0] == '0')
		constants.depthInverted = sl::Boolean::eFalse;
	constants.cameraAspectRatio = width / height;
	return constants;
}

vk::Extent2D g_dlss_extent {};
uint64_t     g_dlss_guest_frame = UINT64_MAX; // the guest frame DLSS last ran in
uint32_t     g_dlss_failures    = 0;
VulkanImage  g_nis_image;
vk::ImageView g_nis_view   = nullptr;
uint32_t      g_nis_failures = 0;

} // namespace
#endif

#ifdef KYTY_FRAMEGEN_STREAMLINE
// ---- After DLSS: the game's exposure (its TAA multiplies the color input by it; DLSS does not) ----
namespace {

// taa_exposure.comp:
/*
#version 450
// After DLSS (in place of Demon's Souls' TAA): the game's TAA multiplies its color input by the eye-adaptation
// exposure (the first float of a small buffer a pass just before writes on the GPU); DLSS does not, so its result is
// multiplied here. Alpha: `alpha_mode` 0 keeps DLSS's, 1 writes `alpha_value`.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, rgba16f) uniform image2D picture;
layout(set = 0, binding = 1, std430) readonly buffer Exposure { float values[]; } exposure;
layout(push_constant) uniform Push {
	uint  index;      // the exposure's float index in the bound range
	uint  width;
	uint  height;
	uint  alpha_mode;
	float alpha_value;
	float scale;      // an extra factor (1)
} push;
void main() {
	const uvec2 p = gl_GlobalInvocationID.xy;
	if (p.x >= push.width || p.y >= push.height) return;
	vec4 c = imageLoad(picture, ivec2(p));
	c.rgb *= exposure.values[push.index] * push.scale;
	if (push.alpha_mode == 1u) c.a = push.alpha_value;
	imageStore(picture, ivec2(p), c);
}
*/
constexpr uint32_t kTaaExposureSpv[] = {
// glslangValidator -V --target-env vulkan1.3 -Os taa_exposure.comp (frame-gen.cpp)
0x07230203u, 0x00010600u, 0x0008000bu, 0x00000075u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
0x0009000fu, 0x00000005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000cu, 0x00000017u, 0x00000031u,
0x0000003au, 0x00060010u, 0x00000004u, 0x00000011u, 0x00000008u, 0x00000008u, 0x00000001u, 0x00030003u,
0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00080005u, 0x0000000cu,
0x475f6c67u, 0x61626f6cu, 0x766e496cu, 0x7461636fu, 0x496e6f69u, 0x00000044u, 0x00040005u, 0x00000015u,
0x68737550u, 0x00000000u, 0x00050006u, 0x00000015u, 0x00000000u, 0x65646e69u, 0x00000078u, 0x00050006u,
0x00000015u, 0x00000001u, 0x74646977u, 0x00000068u, 0x00050006u, 0x00000015u, 0x00000002u, 0x67696568u,
0x00007468u, 0x00060006u, 0x00000015u, 0x00000003u, 0x68706c61u, 0x6f6d5f61u, 0x00006564u, 0x00060006u,
0x00000015u, 0x00000004u, 0x68706c61u, 0x61765f61u, 0x0065756cu, 0x00050006u, 0x00000015u, 0x00000005u,
0x6c616373u, 0x00000065u, 0x00040005u, 0x00000017u, 0x68737570u, 0x00000000u, 0x00040005u, 0x00000031u,
0x74636970u, 0x00657275u, 0x00050005u, 0x00000038u, 0x6f707845u, 0x65727573u, 0x00000000u, 0x00050006u,
0x00000038u, 0x00000000u, 0x756c6176u, 0x00007365u, 0x00050005u, 0x0000003au, 0x6f707865u, 0x65727573u,
0x00000000u, 0x00040047u, 0x0000000cu, 0x0000000bu, 0x0000001cu, 0x00030047u, 0x00000015u, 0x00000002u,
0x00050048u, 0x00000015u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00050048u, 0x00000015u, 0x00000001u,
0x00000023u, 0x00000004u, 0x00050048u, 0x00000015u, 0x00000002u, 0x00000023u, 0x00000008u, 0x00050048u,
0x00000015u, 0x00000003u, 0x00000023u, 0x0000000cu, 0x00050048u, 0x00000015u, 0x00000004u, 0x00000023u,
0x00000010u, 0x00050048u, 0x00000015u, 0x00000005u, 0x00000023u, 0x00000014u, 0x00040047u, 0x00000031u,
0x00000021u, 0x00000000u, 0x00040047u, 0x00000031u, 0x00000022u, 0x00000000u, 0x00040047u, 0x00000037u,
0x00000006u, 0x00000004u, 0x00030047u, 0x00000038u, 0x00000002u, 0x00040048u, 0x00000038u, 0x00000000u,
0x00000018u, 0x00050048u, 0x00000038u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00030047u, 0x0000003au,
0x00000018u, 0x00040047u, 0x0000003au, 0x00000021u, 0x00000001u, 0x00040047u, 0x0000003au, 0x00000022u,
0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00040015u, 0x00000006u,
0x00000020u, 0x00000000u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000002u, 0x00040017u, 0x0000000au,
0x00000006u, 0x00000003u, 0x00040020u, 0x0000000bu, 0x00000001u, 0x0000000au, 0x0004003bu, 0x0000000bu,
0x0000000cu, 0x00000001u, 0x00020014u, 0x0000000fu, 0x0004002bu, 0x00000006u, 0x00000010u, 0x00000000u,
0x00030016u, 0x00000014u, 0x00000020u, 0x0008001eu, 0x00000015u, 0x00000006u, 0x00000006u, 0x00000006u,
0x00000006u, 0x00000014u, 0x00000014u, 0x00040020u, 0x00000016u, 0x00000009u, 0x00000015u, 0x0004003bu,
0x00000016u, 0x00000017u, 0x00000009u, 0x00040015u, 0x00000018u, 0x00000020u, 0x00000001u, 0x0004002bu,
0x00000018u, 0x00000019u, 0x00000001u, 0x00040020u, 0x0000001au, 0x00000009u, 0x00000006u, 0x0004002bu,
0x00000006u, 0x00000021u, 0x00000001u, 0x0004002bu, 0x00000018u, 0x00000024u, 0x00000002u, 0x00040017u,
0x0000002cu, 0x00000014u, 0x00000004u, 0x00090019u, 0x0000002fu, 0x00000014u, 0x00000001u, 0x00000000u,
0x00000000u, 0x00000000u, 0x00000002u, 0x00000002u, 0x00040020u, 0x00000030u, 0x00000000u, 0x0000002fu,
0x0004003bu, 0x00000030u, 0x00000031u, 0x00000000u, 0x00040017u, 0x00000034u, 0x00000018u, 0x00000002u,
0x0003001du, 0x00000037u, 0x00000014u, 0x0003001eu, 0x00000038u, 0x00000037u, 0x00040020u, 0x00000039u,
0x0000000cu, 0x00000038u, 0x0004003bu, 0x00000039u, 0x0000003au, 0x0000000cu, 0x0004002bu, 0x00000018u,
0x0000003bu, 0x00000000u, 0x00040020u, 0x0000003eu, 0x0000000cu, 0x00000014u, 0x0004002bu, 0x00000018u,
0x00000041u, 0x00000005u, 0x00040020u, 0x00000042u, 0x00000009u, 0x00000014u, 0x00040017u, 0x00000046u,
0x00000014u, 0x00000003u, 0x0004002bu, 0x00000018u, 0x00000052u, 0x00000003u, 0x0004002bu, 0x00000018u,
0x00000058u, 0x00000004u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
0x00000005u, 0x000300f7u, 0x00000063u, 0x00000000u, 0x000300fbu, 0x00000010u, 0x00000064u, 0x000200f8u,
0x00000064u, 0x0004003du, 0x0000000au, 0x0000000du, 0x0000000cu, 0x0007004fu, 0x00000007u, 0x0000000eu,
0x0000000du, 0x0000000du, 0x00000000u, 0x00000001u, 0x00050051u, 0x00000006u, 0x00000013u, 0x0000000du,
0x00000000u, 0x00050041u, 0x0000001au, 0x0000001bu, 0x00000017u, 0x00000019u, 0x0004003du, 0x00000006u,
0x0000001cu, 0x0000001bu, 0x000500aeu, 0x0000000fu, 0x0000001du, 0x00000013u, 0x0000001cu, 0x000400a8u,
0x0000000fu, 0x0000001eu, 0x0000001du, 0x000300f7u, 0x00000020u, 0x00000000u, 0x000400fau, 0x0000001eu,
0x0000001fu, 0x00000020u, 0x000200f8u, 0x0000001fu, 0x00050051u, 0x00000006u, 0x00000023u, 0x0000000du,
0x00000001u, 0x00050041u, 0x0000001au, 0x00000025u, 0x00000017u, 0x00000024u, 0x0004003du, 0x00000006u,
0x00000026u, 0x00000025u, 0x000500aeu, 0x0000000fu, 0x00000027u, 0x00000023u, 0x00000026u, 0x000200f9u,
0x00000020u, 0x000200f8u, 0x00000020u, 0x000700f5u, 0x0000000fu, 0x00000028u, 0x0000001du, 0x00000064u,
0x00000027u, 0x0000001fu, 0x000300f7u, 0x0000002au, 0x00000000u, 0x000400fau, 0x00000028u, 0x00000029u,
0x0000002au, 0x000200f8u, 0x00000029u, 0x000200f9u, 0x00000063u, 0x000200f8u, 0x0000002au, 0x0004003du,
0x0000002fu, 0x00000032u, 0x00000031u, 0x0004007cu, 0x00000034u, 0x00000035u, 0x0000000eu, 0x00050062u,
0x0000002cu, 0x00000036u, 0x00000032u, 0x00000035u, 0x00050041u, 0x0000001au, 0x0000003cu, 0x00000017u,
0x0000003bu, 0x0004003du, 0x00000006u, 0x0000003du, 0x0000003cu, 0x00060041u, 0x0000003eu, 0x0000003fu,
0x0000003au, 0x0000003bu, 0x0000003du, 0x0004003du, 0x00000014u, 0x00000040u, 0x0000003fu, 0x00050041u,
0x00000042u, 0x00000043u, 0x00000017u, 0x00000041u, 0x0004003du, 0x00000014u, 0x00000044u, 0x00000043u,
0x00050085u, 0x00000014u, 0x00000045u, 0x00000040u, 0x00000044u, 0x0008004fu, 0x00000046u, 0x00000048u,
0x00000036u, 0x00000036u, 0x00000000u, 0x00000001u, 0x00000002u, 0x0005008eu, 0x00000046u, 0x00000049u,
0x00000048u, 0x00000045u, 0x00050051u, 0x00000014u, 0x0000004cu, 0x00000049u, 0x00000000u, 0x00060052u,
0x0000002cu, 0x0000006cu, 0x0000004cu, 0x00000036u, 0x00000000u, 0x00050051u, 0x00000014u, 0x0000004eu,
0x00000049u, 0x00000001u, 0x00060052u, 0x0000002cu, 0x0000006eu, 0x0000004eu, 0x0000006cu, 0x00000001u,
0x00050051u, 0x00000014u, 0x00000051u, 0x00000049u, 0x00000002u, 0x00060052u, 0x0000002cu, 0x00000070u,
0x00000051u, 0x0000006eu, 0x00000002u, 0x00050041u, 0x0000001au, 0x00000053u, 0x00000017u, 0x00000052u,
0x0004003du, 0x00000006u, 0x00000054u, 0x00000053u, 0x000500aau, 0x0000000fu, 0x00000055u, 0x00000054u,
0x00000021u, 0x000300f7u, 0x00000057u, 0x00000000u, 0x000400fau, 0x00000055u, 0x00000056u, 0x00000057u,
0x000200f8u, 0x00000056u, 0x00050041u, 0x00000042u, 0x00000059u, 0x00000017u, 0x00000058u, 0x0004003du,
0x00000014u, 0x0000005au, 0x00000059u, 0x00070050u, 0x0000002cu, 0x00000074u, 0x0000004cu, 0x0000004eu,
0x00000051u, 0x0000005au, 0x000200f9u, 0x00000057u, 0x000200f8u, 0x00000057u, 0x000700f5u, 0x0000002cu,
0x00000073u, 0x00000070u, 0x0000002au, 0x00000074u, 0x00000056u, 0x0004003du, 0x0000002fu, 0x0000005du,
0x00000031u, 0x00040063u, 0x0000005du, 0x00000035u, 0x00000073u, 0x000200f9u, 0x00000063u, 0x000200f8u,
0x00000063u, 0x000100fdu, 0x00010038u
};

struct ExposurePass {
	vk::DescriptorSetLayout              set_layout = nullptr;
	vk::PipelineLayout                   layout     = nullptr;
	vk::Pipeline                         pipeline   = nullptr;
	vk::DescriptorPool                   pool       = nullptr;
	std::array<vk::DescriptorSet, 16>    sets {}; // a ring: more than the frames in flight
	uint32_t                             next   = 0;
	bool                                 failed = false;
};
ExposurePass g_exposure_pass;

struct ExposurePush {
	uint32_t index, width, height, alpha_mode;
	float    alpha_value, scale;
};

bool CreateExposurePass(GraphicContext& graphics) {
	auto& pass = g_exposure_pass;
	if (pass.pipeline) return true;
	if (pass.failed) return false;
	pass.failed        = true;
	const auto device  = graphics.device;
	vk::DescriptorSetLayoutBinding bindings[2] {};
	bindings[0].binding         = 0;
	bindings[0].descriptorType  = vk::DescriptorType::eStorageImage;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags      = vk::ShaderStageFlagBits::eCompute;
	bindings[1].binding         = 1;
	bindings[1].descriptorType  = vk::DescriptorType::eStorageBuffer;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags      = vk::ShaderStageFlagBits::eCompute;
	vk::DescriptorSetLayoutCreateInfo set_info {};
	set_info.bindingCount = 2;
	set_info.pBindings    = bindings;
	if (device.createDescriptorSetLayout(&set_info, nullptr, &pass.set_layout) != vk::Result::eSuccess) return false;
	vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, sizeof(ExposurePush)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &pass.set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push;
	if (device.createPipelineLayout(&layout_info, nullptr, &pass.layout) != vk::Result::eSuccess) return false;
	vk::ShaderModuleCreateInfo module_info {};
	module_info.codeSize = sizeof(kTaaExposureSpv);
	module_info.pCode    = kTaaExposureSpv;
	vk::ShaderModule module = nullptr;
	if (device.createShaderModule(&module_info, nullptr, &module) != vk::Result::eSuccess) return false;
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	pipeline_info.stage.module = module;
	pipeline_info.stage.pName  = "main";
	pipeline_info.layout       = pass.layout;
	const auto created = device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &pass.pipeline);
	device.destroyShaderModule(module, nullptr);
	if (created != vk::Result::eSuccess) return false;
	vk::DescriptorPoolSize sizes[2] {{vk::DescriptorType::eStorageImage, 16}, {vk::DescriptorType::eStorageBuffer, 16}};
	vk::DescriptorPoolCreateInfo pool_info {};
	pool_info.maxSets       = 16;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes    = sizes;
	if (device.createDescriptorPool(&pool_info, nullptr, &pass.pool) != vk::Result::eSuccess) return false;
	std::array<vk::DescriptorSetLayout, 16> layouts {};
	layouts.fill(pass.set_layout);
	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool     = pass.pool;
	allocate.descriptorSetCount = 16;
	allocate.pSetLayouts        = layouts.data();
	if (device.allocateDescriptorSets(&allocate, pass.sets.data()) != vk::Result::eSuccess) return false;
	pass.failed = false;
	std::printf("Frame generation: DLSS's result gets the game's exposure (the TAA's)\n");
	std::fflush(stdout);
	return true;
}

// Outside rendering, after DLSS wrote `output` (GENERAL): output.rgb *= exposure.
void ApplyExposure(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& output,
                   const TaaExposure& exposure) {
	if (!g_exposure_on.load(std::memory_order_relaxed) || !exposure.buffer || output.view == nullptr ||
	    output.layout != vk::ImageLayout::eGeneral || output.format != vk::Format::eR16G16B16A16Sfloat) {
		return;
	}
	if (!CreateExposurePass(graphics)) {
		static bool told = false;
		if (!told) {
			told = true;
			std::printf("Frame generation: the exposure pass could not be created (DLSS's result stays unexposed)\n");
			std::fflush(stdout);
		}
		return;
	}
	static bool described = false;
	if (!described) {
		described = true;
		std::printf("Frame generation: exposure from float %u of a %llu-byte range (offset %llu)\n", exposure.index,
		            static_cast<unsigned long long>(exposure.range), static_cast<unsigned long long>(exposure.offset));
		std::fflush(stdout);
	}
	auto&      pass = g_exposure_pass;
	const auto set  = pass.sets[pass.next++ % pass.sets.size()];
	vk::DescriptorImageInfo  image_info {nullptr, output.view, vk::ImageLayout::eGeneral};
	vk::DescriptorBufferInfo buffer_info {exposure.buffer, exposure.offset, exposure.range};
	vk::WriteDescriptorSet   writes[2] {};
	writes[0].dstSet          = set;
	writes[0].dstBinding      = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType  = vk::DescriptorType::eStorageImage;
	writes[0].pImageInfo      = &image_info;
	writes[1].dstSet          = set;
	writes[1].dstBinding      = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType  = vk::DescriptorType::eStorageBuffer;
	writes[1].pBufferInfo     = &buffer_info;
	graphics.device.updateDescriptorSets(2, writes, 0, nullptr);
	const ExposurePush push {exposure.index, output.extent.width, output.extent.height,
	                         static_cast<uint32_t>(g_taa_alpha_mode.load()), g_taa_alpha_value.load(),
	                         g_exposure_scale.load()};
	command.bindPipeline(vk::PipelineBindPoint::eCompute, pass.pipeline, Direct());
	command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pass.layout, 0, 1, &set, 0, nullptr, Direct());
	command.pushConstants(pass.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push, Direct());
	command.dispatch((output.extent.width + 7) / 8, (output.extent.height + 7) / 8, 1, Direct());
	DirectFullBarrier(command);
}

} // namespace

void ProbeReport(); // `taaprobe` (below)
#endif

bool EvaluateDlss(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& color, const SrImage& depth,
                  const SrImage& motion, const SrImage& output, const TaaExposure& exposure) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!DlssEnabled() || !g_camera.valid || g_dlss_failures >= 8) return false;
	if (color.image == nullptr || depth.image == nullptr || motion.image == nullptr || output.image == nullptr ||
	    color.view == nullptr || depth.view == nullptr || motion.view == nullptr || output.view == nullptr) {
		return false;
	}
	// DLAA: the TAA's output is its input's size.
	if (output.extent != color.extent || depth.extent != color.extent || motion.extent != color.extent) return false;
	// Only the full picture's TAA. Indoors the game runs the same shader again on a quarter-size lighting buffer
	// (400x225 at 900p): that one stays the game's (DLSS there darkened indoor scenes, and switching DLSS between the
	// two sizes reset it every frame).
	static vk::Extent2D largest {};
	if (uint64_t {color.extent.width} * color.extent.height > uint64_t {largest.width} * largest.height) {
		largest = color.extent;
	}
	g_nis_scene_extent.store(uint64_t {largest.width} << 32u | largest.height, std::memory_order_relaxed);
	if (color.extent != largest) {
		static bool told = false;
		if (!told) {
			told = true;
			std::printf("Frame generation: the %ux%u TAA pass stays the game's (DLSS takes the %ux%u one)\n",
			            color.extent.width, color.extent.height, largest.width, largest.height);
			std::fflush(stdout);
		}
		return false;
	}
	const bool options_changed = g_dlss_extent != output.extent || g_dlss_options_dirty.exchange(false);
	if (options_changed && g_dlss_extent == output.extent) {
		// (A previous instance still waiting: freed now only if its frames are done, else left as it is.)
		if (g_dlss_free_id != 0 && g_api.free_resources != nullptr && g_dlss_evaluations >= g_dlss_free_at) {
			(void)g_api.free_resources(sl::kFeatureDLSS, sl::ViewportHandle {g_dlss_free_id});
		}
		g_dlss_free_id     = g_dlss_viewport_id;
		g_dlss_free_at     = g_dlss_evaluations + 8;
		g_dlss_viewport_id = g_dlss_viewport_id < 13 ? 13 : g_dlss_viewport_id + 1;
	}
	const sl::ViewportHandle viewport {g_dlss_viewport_id};
	bool                     reset = g_dlss_guest_frame == UINT64_MAX || g_guest_frame != g_dlss_guest_frame + 1;
	if (options_changed) {
		sl::DLSSOptions options {};
		options.mode            = sl::DLSSMode::eDLAA;
		options.outputWidth     = output.extent.width;
		options.outputHeight    = output.extent.height;
		options.colorBuffersHDR = g_dlss_hdr.load() != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.useAutoExposure = g_dlss_auto_exposure.load() != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		if (g_dlss_alpha.load() < 0) {
			const char* alpha = std::getenv("KYTY_DLSS_ALPHA");
			g_dlss_alpha      = alpha != nullptr && std::atoi(alpha) != 0 ? 1 : 0;
		}
		options.alphaUpscalingEnabled = g_dlss_alpha.load() != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		// KYTY_DLSS_PRESET=J|K|L|M: a DLSS model preset (default: the driver's choice for DLAA).
		if (const char* preset = std::getenv("KYTY_DLSS_PRESET"); preset != nullptr && *preset >= 'J' && *preset <= 'M') {
			options.dlaaPreset = static_cast<sl::DLSSPreset>(static_cast<uint32_t>(sl::DLSSPreset::ePresetJ) + (*preset - 'J'));
		}
		if (const auto result = g_api.dlss_set_options(viewport, options); result != sl::Result::eOk) {
			std::printf("Frame generation: slDLSSSetOptions failed (%d): the game's TAA stays\n", static_cast<int>(result));
			std::fflush(stdout);
			g_dlss_failures = 8;
			return false;
		}
		g_dlss_extent = output.extent;
		reset         = true;
		std::printf("Frame generation: DLSS (DLAA) at %ux%u, hdr %d, auto exposure %d, alpha %d\n", output.extent.width,
		            output.extent.height, g_dlss_hdr.load(), g_dlss_auto_exposure.load(), g_dlss_alpha.load());
		std::fflush(stdout);
	}
	// HDR: a frame index of DLAA's own, one per evaluation (as the super resolution path's): the present's index (RendererToken)
	// was the same for two game frames now and then - the renderer runs ahead of the present - and Streamline then
	// refused the second frame's camera ("different 'common' constants within the same frame": 6% of the frames, 18%
	// with Reflex back at the present, 10-22).
	// (The HDR launcher's, --hdr; SDR keeps the present's index, 10-22.)
	sl::FrameToken* token = nullptr;
	if (Config::HdrEnabled()) {
		static uint32_t dlaa_frame = 0;
		uint32_t        index      = ++dlaa_frame;
		if (NewFrameToken(token, &index) != sl::Result::eOk) token = nullptr;
	} else {
		token = RendererToken();
	}
	if (token == nullptr) return false;
	const auto constants = CameraConstants(g_camera, static_cast<float>(color.extent.width),
	                                       static_cast<float>(color.extent.height), reset);
	// (Two guest frames can fall within one presented frame index: Streamline then reports the second, different set
	// as an error but keeps it, the one this evaluation reads.)
	(void)g_api.set_constants(constants, *token, viewport);

	DrainRecording();
	DirectFullBarrier(command);
	auto       color_resource  = SlResource(color);
	auto       depth_resource  = SlResource(depth);
	auto       motion_resource = SlResource(motion);
	auto       output_resource = SlResource(output);
	const sl::Extent full {0, 0, color.extent.width, color.extent.height};
	sl::ResourceTag tags[] {
	    sl::ResourceTag(&color_resource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &full),
	    sl::ResourceTag(&depth_resource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &full),
	    sl::ResourceTag(&motion_resource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &full),
	    sl::ResourceTag(&output_resource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &full),
	};
	auto* sl_command = reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(command));
	sl::Result result = g_api.set_tag_for_frame(*token, viewport, tags, static_cast<uint32_t>(std::size(tags)), sl_command);
	if (Evaluated(result)) {
		const sl::BaseStructure* inputs[] {&viewport};
		result = g_api.evaluate_feature(sl::kFeatureDLSS, *token, inputs, 1, sl_command);
	}
	DirectFullBarrier(command);
	if (!Evaluated(result)) {
		std::printf("Frame generation: DLSS evaluation failed (%d)%s\n", static_cast<int>(result),
		            ++g_dlss_failures >= 8 ? ": the game's TAA stays" : "");
		std::fflush(stdout);
		return false;
	}
	ApplyExposure(graphics, command, output, exposure);
	g_dlss_guest_frame = g_guest_frame;
	const uint64_t evaluations = ++g_dlss_evaluations;
	ProbeReport();
	if (g_dlss_free_id != 0 && evaluations >= g_dlss_free_at) {
		if (g_api.free_resources != nullptr) (void)g_api.free_resources(sl::kFeatureDLSS, sl::ViewportHandle {g_dlss_free_id});
		g_dlss_free_id = 0;
	}
	if (evaluations == 1 || evaluations == 600) {
		std::printf("Frame generation: DLSS evaluated %llu time(s)%s\n", static_cast<unsigned long long>(evaluations),
		            result == sl::Result::eWarnOutOfVRAM ? " (Streamline: video memory budget exceeded)" : "");
		std::fflush(stdout);
	}
	return true;
#else
	(void)graphics;
	(void)command;
	(void)color;
	(void)depth;
	(void)motion;
	(void)output;
	(void)exposure;
	return false;
#endif
}

bool PassThroughTaa(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& color, const SrImage& output,
                    const TaaExposure& exposure) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (!LateAaEnabled() || color.image == nullptr || output.image == nullptr || color.extent != output.extent ||
	    color.layout == vk::ImageLayout::eUndefined)
		return false;
	// Only the full picture's TAA (as DLAA): the quarter-size indoor lighting pass stays the game's.
	static vk::Extent2D largest {};
	if (uint64_t {color.extent.width} * color.extent.height > uint64_t {largest.width} * largest.height) largest = color.extent;
	if (color.extent != largest) return false;
	static int unusable = -1; // (told once: the TAA's images cannot be copied; its TAA runs)
	const bool same_format = color.format == output.format;
	const auto features    = [&](vk::Format format) {
        return graphics.physical_device.getFormatProperties(format).optimalTilingFeatures;
	};
	const bool can = (color.usage & vk::ImageUsageFlagBits::eTransferSrc) &&
	                 (output.usage & vk::ImageUsageFlagBits::eTransferDst) && output.layout == vk::ImageLayout::eGeneral &&
	                 (same_format || ((features(color.format) & vk::FormatFeatureFlagBits::eBlitSrc) &&
	                                  (features(output.format) & vk::FormatFeatureFlagBits::eBlitDst)));
	if (unusable < 0) {
		unusable = can ? 0 : 1;
		std::printf("Late DLSS: the TAA pass %s (color %d -> output %d, %ux%u, output layout %d)\n",
		            can ? "replaced by a copy, DLSS anti-aliases later" : "cannot be copied through: the game's TAA runs",
		            static_cast<int>(color.format), static_cast<int>(output.format), color.extent.width,
		            color.extent.height, static_cast<int>(output.layout));
		std::fflush(stdout);
	}
	if (!can) return false;
	DrainRecording();
	DirectFullBarrier(command);
	// The color image in a transfer layout for the copy, then back as the texture cache has it.
	const bool move = color.layout != vk::ImageLayout::eGeneral && color.layout != vk::ImageLayout::eTransferSrcOptimal;
	const auto src_layout = move ? vk::ImageLayout::eTransferSrcOptimal : color.layout;
	const auto relayout   = [&](vk::ImageLayout from, vk::ImageLayout to) {
        vk::ImageMemoryBarrier2 barrier {};
        barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
        barrier.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
        barrier.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
        barrier.dstAccessMask       = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
        barrier.oldLayout           = from;
        barrier.newLayout           = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = color.image;
        barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::DependencyInfo dependency {};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers    = &barrier;
        command.pipelineBarrier2(dependency, Direct());
	};
	if (move) relayout(color.layout, src_layout);
	const vk::ImageSubresourceLayers layers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	if (same_format) {
		vk::ImageCopy copy {};
		copy.srcSubresource = layers;
		copy.dstSubresource = layers;
		copy.extent         = vk::Extent3D {color.extent.width, color.extent.height, 1};
		command.copyImage(color.image, src_layout, output.image, vk::ImageLayout::eGeneral, 1, &copy, Direct());
	} else {
		vk::ImageBlit blit {};
		blit.srcSubresource = layers;
		blit.dstSubresource = layers;
		blit.srcOffsets[1]  = vk::Offset3D {static_cast<int32_t>(color.extent.width), static_cast<int32_t>(color.extent.height), 1};
		blit.dstOffsets[1]  = blit.srcOffsets[1];
		command.blitImage(color.image, src_layout, output.image, vk::ImageLayout::eGeneral, 1, &blit, vk::Filter::eNearest,
		                  Direct());
	}
	if (move) relayout(src_layout, color.layout);
	DirectFullBarrier(command);
	ApplyExposure(graphics, command, output, exposure);
	// (Late DLSS resets the count when it makes the frame's picture, after this.)
	if (++g_late_unresolved == LateUnresolvedLimit) {
		std::printf("Late DLSS: no picture made in %u frames whose TAA was copied through: the game's TAA runs again\n",
		            LateUnresolvedLimit);
		std::fflush(stdout);
	}
	return true;
#else
	(void)graphics;
	(void)command;
	(void)color;
	(void)output;
	(void)exposure;
	return false;
#endif
}

#ifdef KYTY_FRAMEGEN_STREAMLINE
vk::ImageView EvaluateLateDlss(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& scene,
                               TextureCache& cache);
#endif

vk::ImageView EvaluateNis(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& scene, TextureCache* cache) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (g_late_ready) return NisEnabled() && cache != nullptr ? EvaluateLateDlss(graphics, command, scene, *cache) : nullptr;
	if (!NisEnabled() || g_nis_failures >= 8 || scene.image == nullptr || scene.view == nullptr) return nullptr;
	const uint32_t height = g_nis_height;
	const uint32_t width  = (height * 16u / 9u + 7u) / 8u * 8u;
	// Only an upscale (a picture already this large stays as it is).
	if (scene.extent.height >= height) return nullptr;
	const sl::ViewportHandle viewport {2};
	if (g_nis_image.image == nullptr) {
		vk::ImageCreateInfo create {};
		create.imageType     = vk::ImageType::e2D;
		create.extent        = vk::Extent3D {width, height, 1};
		create.mipLevels     = 1;
		create.arrayLayers   = 1;
		create.format        = vk::Format::eR16G16B16A16Sfloat;
		create.tiling        = vk::ImageTiling::eOptimal;
		create.initialLayout = vk::ImageLayout::eUndefined;
		create.usage         = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
		create.sharingMode   = vk::SharingMode::eExclusive;
		create.samples       = vk::SampleCountFlagBits::e1;
		if (!graphics.CreateImage(create, g_nis_image)) {
			std::printf("Frame generation: cannot allocate the %ux%u NIS image\n", width, height);
			g_nis_failures = 8;
			return nullptr;
		}
		vk::ImageViewCreateInfo view_info {};
		view_info.image            = g_nis_image.image;
		view_info.viewType         = vk::ImageViewType::e2D;
		view_info.format           = create.format;
		view_info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(graphics.device.createImageView(&view_info, nullptr, &g_nis_view), "NIS output view");
		g_nis_image.state.layout = vk::ImageLayout::eUndefined;
		g_nis_options_dirty      = true;
	}
	if (g_nis_options_dirty.exchange(false)) {
		sl::NISOptions options {};
		options.mode = sl::NISMode::eScaler;
		options.sharpness = 0.3f;
		if (const char* sharpness = std::getenv("KYTY_NIS_SHARPNESS"); sharpness != nullptr) {
			options.sharpness = std::clamp(static_cast<float>(std::atof(sharpness)), 0.0f, 1.0f);
		}
		if (const int live = g_nis_sharpness_x100.load(); live >= 0) options.sharpness = static_cast<float>(live) / 100.0f;
		// KYTY_NIS_HDR=linear|pq: the picture's encoding (default: SDR).
		if (const char* hdr = std::getenv("KYTY_NIS_HDR"); hdr != nullptr) {
			const std::string mode = hdr;
			options.hdrMode = mode == "pq" ? sl::NISHDR::ePQ : mode == "linear" ? sl::NISHDR::eLinear : sl::NISHDR::eNone;
		}
		if (const int live = g_nis_hdr.load(); live >= 0) options.hdrMode = static_cast<sl::NISHDR>(live);
		if (const auto result = g_api.nis_set_options(viewport, options); result != sl::Result::eOk) {
			std::printf("Frame generation: slNISSetOptions failed (%d)\n", static_cast<int>(result));
			g_nis_failures = 8;
			return nullptr;
		}
		std::printf("Frame generation: NIS %ux%u -> %ux%u, sharpness %.2f, hdr mode %d\n", scene.extent.width,
		            scene.extent.height, width, height, static_cast<double>(options.sharpness),
		            static_cast<int>(options.hdrMode));
		std::fflush(stdout);
	}
	auto* token = RendererToken();
	if (token == nullptr) return nullptr;
	(void)g_api.set_constants(IdentityConstants(), *token, viewport);

	DrainRecording();
	DirectFullBarrier(command);
	DirectLayout(command, g_nis_image, vk::ImageLayout::eGeneral);
	SrImage output {g_nis_image.image, g_nis_view, vk::Format::eR16G16B16A16Sfloat, {width, height},
	                vk::ImageLayout::eGeneral, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled};
	auto             input_resource  = SlResource(scene);
	auto             output_resource = SlResource(output);
	const sl::Extent input_extent {0, 0, scene.extent.width, scene.extent.height};
	const sl::Extent output_extent {0, 0, width, height};
	sl::ResourceTag  tags[] {
        sl::ResourceTag(&input_resource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &input_extent),
        sl::ResourceTag(&output_resource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &output_extent),
    };
	auto* sl_command = reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(command));
	sl::Result result = g_api.set_tag_for_frame(*token, viewport, tags, static_cast<uint32_t>(std::size(tags)), sl_command);
	if (Evaluated(result)) {
		const sl::BaseStructure* inputs[] {&viewport};
		result = g_api.evaluate_feature(sl::kFeatureNIS, *token, inputs, 1, sl_command);
	}
	DirectLayout(command, g_nis_image, vk::ImageLayout::eShaderReadOnlyOptimal);
	if (!Evaluated(result)) {
		std::printf("Frame generation: NIS evaluation failed (%d)\n", static_cast<int>(result));
		std::fflush(stdout);
		++g_nis_failures;
		return nullptr;
	}
	static uint32_t evaluations = 0;
	if (++evaluations == 1 || evaluations == 600) {
		std::printf("Frame generation: NIS evaluated %u time(s)\n", evaluations);
		std::fflush(stdout);
	}
	return g_nis_view;
#else
	(void)graphics;
	(void)command;
	(void)scene;
	(void)cache;
	return nullptr;
#endif
}

#ifdef KYTY_FRAMEGEN_STREAMLINE
namespace {
VulkanImage   g_late_image, g_late_depth, g_late_motion;
vk::ImageView g_late_view = nullptr, g_late_depth_view = nullptr, g_late_motion_view = nullptr;
uint64_t      g_late_guest_frame = UINT64_MAX;
uint64_t      g_late_evaluations = 0;
vk::Extent2D  g_late_input {};

// KYTY_LATE_DUMP=<trigger file>: when that file appears, one late DLSS frame's images are written next to it as raw files
// (late-input / late-output RGBA16F, late-motion RG16F, late-depth D32 floats), the trigger renamed to .done. Debugging.
struct LateDump {
	VkBuffer      buffer     = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	VmaAllocator  allocator  = nullptr;
	void*         mapped     = nullptr;
	uint64_t      ready_at   = 0;
	bool          pending    = false;
	struct Part {
		std::string    name;
		vk::DeviceSize offset = 0, size = 0;
		uint32_t       width = 0, height = 0;
	};
	std::vector<Part> parts;
	std::string       folder;
	bool              next_pending = false; // the next evaluation's input goes to the part "next" (the motion's check)
	vk::ImageLayout   next_layout  = vk::ImageLayout::eUndefined;
};
LateDump g_late_dump;

void DumpLayout(vk::CommandBuffer command, vk::Image image, vk::ImageAspectFlags aspect, vk::ImageLayout from,
                vk::ImageLayout to) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {aspect, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency, Direct());
}

bool LateDumpWanted() {
	static const char* trigger = std::getenv("KYTY_LATE_DUMP");
	if (trigger == nullptr || g_late_dump.pending) return false;
	static uint32_t calls = 0;
	if (++calls % 30 != 0) return false;
	std::error_code error;
	return std::filesystem::exists(trigger, error);
}

// Records the copies (after the evaluation); LateDumpReport writes the files some frames later.
void LateDumpRecord(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& scene, float jitter_x, float jitter_y) {
	const std::filesystem::path trigger_path(std::getenv("KYTY_LATE_DUMP"));
	{
		std::error_code error;
		std::filesystem::rename(trigger_path, std::filesystem::path(trigger_path).concat(".done"), error);
	}
	struct Source {
		const char*          name;
		vk::Image            image;
		vk::ImageAspectFlags aspect;
		vk::ImageLayout      layout;
		uint32_t             width, height, bytes;
		vk::ImageUsageFlags  usage;
	};
	const Source sources[] {
	    {"input", scene.image, vk::ImageAspectFlagBits::eColor, scene.layout, scene.extent.width, scene.extent.height, 8,
	     scene.usage},
	    {"output", g_late_image.image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal,
	     g_late_image.extent.width, g_late_image.extent.height, 8, vk::ImageUsageFlagBits::eTransferSrc},
	    {"motion", g_late_motion.image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal,
	     g_late_motion.extent.width, g_late_motion.extent.height, 4, vk::ImageUsageFlagBits::eTransferSrc},
	    {"depth", g_late_depth.image, vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eShaderReadOnlyOptimal,
	     g_late_depth.extent.width, g_late_depth.extent.height, 4, vk::ImageUsageFlagBits::eTransferSrc},
	};
	vk::DeviceSize total = 0;
	g_late_dump.parts.clear();
	std::vector<const Source*> used;
	for (const auto& source: sources) {
		if (source.image == nullptr || !(source.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
		    source.layout == vk::ImageLayout::eUndefined) {
			std::printf("Late DLSS dump: %s skipped (no image, no transfer-source usage or undefined layout)\n", source.name);
			continue;
		}
		const vk::DeviceSize size = static_cast<vk::DeviceSize>(source.width) * source.height * source.bytes;
		g_late_dump.parts.push_back({source.name, total, size, source.width, source.height});
		used.push_back(&source);
		total += (size + 255u) / 256u * 256u;
	}
	if (!used.empty() && used[0]->name == std::string("input")) {
		const vk::DeviceSize size = g_late_dump.parts[0].size;
		g_late_dump.parts.push_back({"next", total, size, scene.extent.width, scene.extent.height});
		total += (size + 255u) / 256u * 256u;
		const vk::DeviceSize depth_size = static_cast<vk::DeviceSize>(g_late_depth.extent.width) * g_late_depth.extent.height * 4u;
		g_late_dump.parts.push_back({"nextdepth", total, depth_size, g_late_depth.extent.width, g_late_depth.extent.height});
		total += (depth_size + 255u) / 256u * 256u;
		const vk::DeviceSize motion_size = static_cast<vk::DeviceSize>(g_late_motion.extent.width) * g_late_motion.extent.height * 4u;
		g_late_dump.parts.push_back({"nextmotion", total, motion_size, g_late_motion.extent.width, g_late_motion.extent.height});
		total += (motion_size + 255u) / 256u * 256u;
	}
	if (g_late_dump.buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(g_late_dump.allocator, g_late_dump.buffer, g_late_dump.allocation);
		g_late_dump.buffer = VK_NULL_HANDLE;
	}
	VkBufferCreateInfo buffer_info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer_info.size        = total;
	buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo allocation_info {};
	allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
	allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	VmaAllocationInfo info {};
	if (total == 0 || vmaCreateBuffer(graphics.allocator, &buffer_info, &allocation_info, &g_late_dump.buffer,
	                                  &g_late_dump.allocation, &info) != VK_SUCCESS || info.pMappedData == nullptr) {
		g_late_dump.buffer = VK_NULL_HANDLE;
		std::printf("Late DLSS dump: no readback buffer (%llu bytes)\n", static_cast<unsigned long long>(total));
		std::fflush(stdout);
		return;
	}
	g_late_dump.allocator = graphics.allocator;
	g_late_dump.mapped    = info.pMappedData;
	for (size_t i = 0; i < used.size(); i++) {
		const auto& source = *used[i];
		const auto& part   = g_late_dump.parts[i];
		DumpLayout(command, source.image, source.aspect, source.layout, vk::ImageLayout::eTransferSrcOptimal);
		vk::BufferImageCopy region {};
		region.bufferOffset     = part.offset;
		region.imageSubresource = vk::ImageSubresourceLayers {source.aspect, 0, 0, 1};
		region.imageExtent      = vk::Extent3D {source.width, source.height, 1};
		command.copyImageToBuffer(source.image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer(g_late_dump.buffer), 1,
		                          &region, Direct());
		DumpLayout(command, source.image, source.aspect, vk::ImageLayout::eTransferSrcOptimal, source.layout);
	}
	g_late_dump.next_pending = g_late_dump.parts.back().name == "nextmotion";
	g_late_dump.folder   = trigger_path.parent_path().string();
	g_late_dump.ready_at = g_late_evaluations + 30;
	g_late_dump.pending  = true;
	std::printf("Late DLSS dump: %zu image(s) recorded, jitter %.4f %.4f px\n", g_late_dump.parts.size(),
	            static_cast<double>(jitter_x), static_cast<double>(jitter_y));
	std::fflush(stdout);
}

void LateDumpReport() {
	if (!g_late_dump.pending || g_late_evaluations < g_late_dump.ready_at) return;
	g_late_dump.pending = false;
	vmaInvalidateAllocation(g_late_dump.allocator, g_late_dump.allocation, 0, VK_WHOLE_SIZE);
	for (const auto& part: g_late_dump.parts) {
		const auto path = std::filesystem::path(g_late_dump.folder) /
		                  ("late-" + part.name + "-" + std::to_string(part.width) + "x" + std::to_string(part.height) + ".bin");
		if (std::ofstream file(path, std::ios::binary); file) {
			file.write(static_cast<const char*>(g_late_dump.mapped) + part.offset, static_cast<std::streamsize>(part.size));
		}
		std::printf("Late DLSS dump: wrote %s\n", path.string().c_str());
	}
	std::fflush(stdout);
}
} // namespace

// KYTY_LATE_DLSS: the finished picture at the render size (`scene`, the final composite's) upscaled by DLSS super
// resolution, with the TAA pass's depth and motion vectors (copied: the game may reuse them before the flip) and the
// G-buffer camera. The picture is already resolved (the TAA or DLAA ran, without jitter) and graded: no jitter, no
// HDR, no exposure for DLSS. The view to bind in the scene's place (shader-read-only), or null (the game's stretch).
vk::ImageView EvaluateLateDlss(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& scene,
                               TextureCache& cache) {
	LateDumpReport();
	if (g_late_failures >= LateFailureLimit || scene.image == nullptr || scene.view == nullptr) return nullptr;
	// (A game without a TAA pass: its G-buffer's depth and motion vectors, no camera: DirectConstants.)
	const bool direct = g_direct_valid.load(std::memory_order_relaxed) && ::GameProfile::Current().direct_motion_target >= 0;
	if (!direct && (!g_sources.valid || !g_camera.valid)) return nullptr; // (a menu, a movie: no 3D this frame)
	const uint32_t height = g_late_height;
	const uint32_t width  = (height * 16u / 9u + 7u) / 8u * 8u;
	if (scene.extent.height >= height) return nullptr;
	if (g_late_sources.extent != scene.extent) return nullptr; // (no full-size TAA pass seen of the picture's size)
	// (The sources may be an earlier frame's, their images retired since - the texture cache retires a world's images
	// after a world change, TextureCache::RetireStaleImages: TryGetImage, not GetImage, on a stale id.)
	auto* depth_image  = cache.TryGetImage(g_late_sources.depth);
	auto* motion_image = cache.TryGetImage(g_late_sources.motion);
	if (depth_image == nullptr || motion_image == nullptr) {
		g_late_sources = {};
		g_late_largest = {};
		return nullptr;
	}
	auto& depth  = *depth_image;
	auto& motion = *motion_image;
	if (depth.info.data.address != g_late_sources.depth_address ||
	    motion.info.data.address != g_late_sources.motion_address ||
	    motion.backing.extent.width != scene.extent.width || motion.backing.extent.height != scene.extent.height ||
	    depth.backing.extent.width != scene.extent.width || depth.backing.extent.height != scene.extent.height) {
		static uint32_t told = 0;
		if (told++ < 4) {
			std::printf("Late DLSS: the TAA's depth %ux%u / motion %ux%u do not match the picture %ux%u: not upscaled\n",
			            depth.backing.extent.width, depth.backing.extent.height, motion.backing.extent.width,
			            motion.backing.extent.height, scene.extent.width, scene.extent.height);
			std::fflush(stdout);
		}
		return nullptr;
	}
	const sl::ViewportHandle viewport {1000}; // (DLAA's viewports count up from 11 with each live option change)
	if (g_late_image.image == nullptr) {
		vk::ImageCreateInfo create {};
		create.imageType     = vk::ImageType::e2D;
		create.extent        = vk::Extent3D {width, height, 1};
		create.mipLevels     = 1;
		create.arrayLayers   = 1;
		create.format        = vk::Format::eR16G16B16A16Sfloat;
		create.tiling        = vk::ImageTiling::eOptimal;
		create.initialLayout = vk::ImageLayout::eUndefined;
		create.usage         = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
		                       vk::ImageUsageFlagBits::eTransferDst;
		create.sharingMode   = vk::SharingMode::eExclusive;
		create.samples       = vk::SampleCountFlagBits::e1;
		if (!graphics.CreateImage(create, g_late_image)) {
			std::printf("Late DLSS: cannot allocate the %ux%u output image\n", width, height);
			g_late_failures = LateFailureLimit;
			return nullptr;
		}
		vk::ImageViewCreateInfo view_info {};
		view_info.image            = g_late_image.image;
		view_info.viewType         = vk::ImageViewType::e2D;
		view_info.format           = create.format;
		view_info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		RequireVulkanSuccess(graphics.device.createImageView(&view_info, nullptr, &g_late_view), "late DLSS output view");
		g_late_image.state.layout = vk::ImageLayout::eUndefined;
	}
	bool reset = g_late_guest_frame == UINT64_MAX || g_guest_frame != g_late_guest_frame + 1;
	if (g_late_input != scene.extent) {
		const float share = static_cast<float>(scene.extent.height) / static_cast<float>(height);
		sl::DLSSOptions options {};
		options.mode = share >= 0.95f   ? sl::DLSSMode::eDLAA
		               : share >= 0.62f ? sl::DLSSMode::eMaxQuality
		               : share >= 0.55f ? sl::DLSSMode::eBalanced
		               : share >= 0.45f ? sl::DLSSMode::eMaxPerformance
		                                : sl::DLSSMode::eUltraPerformance;
		options.outputWidth     = width;
		options.outputHeight    = height;
		const char* hdr         = std::getenv("KYTY_LATE_DLSS_HDR");
		options.colorBuffersHDR = hdr != nullptr && hdr[0] == '1' ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.useAutoExposure = options.colorBuffersHDR;
		if (const char* preset = std::getenv("KYTY_DLSS_PRESET"); preset != nullptr && *preset >= 'A' && *preset <= 'O') {
			const auto value = static_cast<sl::DLSSPreset>(*preset - 'A' + 1);
			options.dlaaPreset = options.qualityPreset = options.balancedPreset = options.performancePreset =
			    options.ultraPerformancePreset = options.ultraQualityPreset = value;
		}
		if (const auto result = g_api.dlss_set_options(viewport, options); result != sl::Result::eOk) {
			std::printf("Late DLSS: slDLSSSetOptions failed (%d): the game's TAA and stretch stay\n", static_cast<int>(result));
			std::fflush(stdout);
			g_late_failures = LateFailureLimit;
			return nullptr;
		}
		g_late_input = scene.extent;
		reset        = true;
		std::printf("Late DLSS: %ux%u -> %ux%u, mode %u, preset %u (13 = M), picture format %d, hdr %d\n", scene.extent.width,
		            scene.extent.height, width, height, static_cast<uint32_t>(options.mode),
		            static_cast<uint32_t>(options.qualityPreset), static_cast<int>(scene.format),
		            options.colorBuffersHDR == sl::Boolean::eTrue ? 1 : 0);
		std::fflush(stdout);
	}
	static uint32_t late_frame = 0;
	uint32_t        index      = ++late_frame;
	sl::FrameToken* token      = nullptr;
	if (NewFrameToken(token, &index) != sl::Result::eOk || token == nullptr) return nullptr;
	auto constants = direct ? DirectConstants(static_cast<float>(scene.extent.width), static_cast<float>(scene.extent.height), reset)
	                        : CameraConstants(g_camera, static_cast<float>(scene.extent.width),
	                                          static_cast<float>(scene.extent.height), reset);
	// (Resolved by the TAA or DLAA: no jitter left in it. KYTY_LATE_DLSS_AA: the game's jitter, its TAA not run.)
	if (!g_late_aa) constants.jitterOffset = sl::float2(0.0f, 0.0f);
	if (direct) {
		float jx = 0.0f, jy = 0.0f;
		if (LateJitter(scene.extent.width, scene.extent.height, jx, jy)) {
			static const float sign = [] {
				const char* value = std::getenv("KYTY_LATE_JITTER_SIGN");
				return value != nullptr && std::atof(value) < 0 ? -1.0f : 1.0f;
			}();
			constants.jitterOffset = sl::float2(jx * sign, jy * sign);
			g_jitter_phase.fetch_add(1, std::memory_order_relaxed); // (the next frame's draws take the next offset)
		}
	}
	(void)g_api.set_constants(constants, *token, viewport);

	// The TAA's depth and motion vectors copied (recorded), then DLSS (direct, after the recorded work).
	Configure(graphics, g_late_depth, g_late_depth_view, depth.backing);
	Configure(graphics, g_late_motion, g_late_motion_view, motion.backing);
	CopyInto(command, depth, g_late_depth);
	CopyInto(command, motion, g_late_motion);
	DrainRecording();
	DirectFullBarrier(command);
	DirectLayout(command, g_late_image, vk::ImageLayout::eGeneral);
	const SrImage depth_copy {g_late_depth.image, g_late_depth_view, g_late_depth.format,
	                          {g_late_depth.extent.width, g_late_depth.extent.height},
	                          vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageUsageFlagBits::eSampled};
	const SrImage motion_copy {g_late_motion.image, g_late_motion_view, g_late_motion.format,
	                           {g_late_motion.extent.width, g_late_motion.extent.height},
	                           vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageUsageFlagBits::eSampled};
	const SrImage output {g_late_image.image, g_late_view, vk::Format::eR16G16B16A16Sfloat, {width, height},
	                      vk::ImageLayout::eGeneral, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled};
	auto             color_resource  = SlResource(scene);
	auto             depth_resource  = SlResource(depth_copy);
	auto             motion_resource = SlResource(motion_copy);
	auto             output_resource = SlResource(output);
	const sl::Extent input_extent {0, 0, scene.extent.width, scene.extent.height};
	const sl::Extent output_extent {0, 0, width, height};
	sl::ResourceTag  tags[] {
        sl::ResourceTag(&color_resource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &input_extent),
        sl::ResourceTag(&depth_resource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &input_extent),
        sl::ResourceTag(&motion_resource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &input_extent),
        sl::ResourceTag(&output_resource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &output_extent),
    };
	auto* sl_command = reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(command));
	// KYTY_LATE_DLSS_SKIP_EVAL=1: the copies are made but DLSS is not run, the game's picture stays (a crash bisection).
	static const bool skip_eval = [] {
		const char* value = std::getenv("KYTY_LATE_DLSS_SKIP_EVAL");
		return value != nullptr && value[0] == '1';
	}();
	if (skip_eval) {
		DirectLayout(command, g_late_image, vk::ImageLayout::eShaderReadOnlyOptimal);
		return nullptr;
	}
	sl::Result result = g_api.set_tag_for_frame(*token, viewport, tags, static_cast<uint32_t>(std::size(tags)), sl_command);
	if (Evaluated(result)) {
		const sl::BaseStructure* inputs[] {&viewport};
		result = g_api.evaluate_feature(sl::kFeatureDLSS, *token, inputs, 1, sl_command);
	}
	// KYTY_LATE_DLSS_PROOF=1: DLSS's picture painted magenta (a check that the final composite shows it: Shift+F6 off
	// brings the game's own back).
	static const bool proof = [] {
		const char* value = std::getenv("KYTY_LATE_DLSS_PROOF");
		return value != nullptr && value[0] == '1';
	}();
	if (proof && Evaluated(result)) {
		DirectLayout(command, g_late_image, vk::ImageLayout::eTransferDstOptimal);
		const vk::ClearColorValue magenta(std::array<float, 4> {1.0f, 0.0f, 1.0f, 1.0f});
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		command.clearColorImage(g_late_image.image, vk::ImageLayout::eTransferDstOptimal, &magenta, 1, &range, Direct());
	}
	DirectLayout(command, g_late_image, vk::ImageLayout::eShaderReadOnlyOptimal);
	if (!Evaluated(result)) {
		// (The frame's stretch; the next frame tries again. The first 8 and then every 100th are logged.)
		static uint32_t failed = 0;
		++g_late_failures;
		if (++failed <= 8 || failed % 100 == 0 || g_late_failures == LateFailureLimit) {
			std::printf("Late DLSS: evaluation failed (%d), %u in a row, %u in all%s\n", static_cast<int>(result),
			            g_late_failures, failed,
			            g_late_failures >= LateFailureLimit ? ": the game's TAA and stretch stay" : "");
			std::fflush(stdout);
		}
		return nullptr;
	}
	if (g_late_dump.next_pending && g_late_dump.buffer != VK_NULL_HANDLE) {
		g_late_dump.next_pending = false;
		const auto& part         = g_late_dump.parts[g_late_dump.parts.size() - 3];
		const auto& depth_part   = g_late_dump.parts[g_late_dump.parts.size() - 2];
		const auto& motion_part  = g_late_dump.parts.back();
		if (g_late_motion.image != nullptr && g_late_motion.extent.width == motion_part.width) {
			DumpLayout(command, g_late_motion.image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal,
			           vk::ImageLayout::eTransferSrcOptimal);
			vk::BufferImageCopy region {};
			region.bufferOffset     = motion_part.offset;
			region.imageSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			region.imageExtent      = vk::Extent3D {motion_part.width, motion_part.height, 1};
			command.copyImageToBuffer(g_late_motion.image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer(g_late_dump.buffer), 1,
			                          &region, Direct());
			DumpLayout(command, g_late_motion.image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferSrcOptimal,
			           vk::ImageLayout::eShaderReadOnlyOptimal);
		}
		if (g_late_depth.image != nullptr && g_late_depth.extent.width == depth_part.width) {
			DumpLayout(command, g_late_depth.image, vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eShaderReadOnlyOptimal,
			           vk::ImageLayout::eTransferSrcOptimal);
			vk::BufferImageCopy region {};
			region.bufferOffset     = depth_part.offset;
			region.imageSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eDepth, 0, 0, 1};
			region.imageExtent      = vk::Extent3D {depth_part.width, depth_part.height, 1};
			command.copyImageToBuffer(g_late_depth.image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer(g_late_dump.buffer), 1,
			                          &region, Direct());
			DumpLayout(command, g_late_depth.image, vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eTransferSrcOptimal,
			           vk::ImageLayout::eShaderReadOnlyOptimal);
		}
		if (scene.extent.width == part.width && scene.extent.height == part.height && scene.layout != vk::ImageLayout::eUndefined) {
			DumpLayout(command, scene.image, vk::ImageAspectFlagBits::eColor, scene.layout, vk::ImageLayout::eTransferSrcOptimal);
			vk::BufferImageCopy region {};
			region.bufferOffset     = part.offset;
			region.imageSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
			region.imageExtent      = vk::Extent3D {part.width, part.height, 1};
			command.copyImageToBuffer(scene.image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer(g_late_dump.buffer), 1,
			                          &region, Direct());
			DumpLayout(command, scene.image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferSrcOptimal, scene.layout);
			std::printf("Late DLSS dump: next frame's input recorded, jitter %.4f %.4f px\n",
			            static_cast<double>(constants.jitterOffset.x), static_cast<double>(constants.jitterOffset.y));
			std::fflush(stdout);
		}
	} else if (LateDumpWanted()) {
		LateDumpRecord(graphics, command, scene, constants.jitterOffset.x, constants.jitterOffset.y);
	}
	g_late_guest_frame = g_guest_frame;
	g_late_unresolved  = 0;
	g_late_failures    = 0;
	if (++g_late_evaluations == 1 || g_late_evaluations == 600) {
		std::printf("Late DLSS: evaluated %llu time(s)\n", static_cast<unsigned long long>(g_late_evaluations));
		std::fflush(stdout);
	}
	return g_late_view;
}
#endif

// ---- KYTY_LIVE_FILE `taaprobe`: the game's TAA result and DLSS's, pixel by pixel (debugging) ----

#ifdef KYTY_FRAMEGEN_STREAMLINE
namespace {

constexpr uint32_t kProbeCols = 16, kProbeRows = 9, kProbePoints = kProbeCols * kProbeRows, kProbeImages = 3;
std::atomic<int>   g_probe_requests {0};
struct Probe {
	VkBuffer      buffer     = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	VmaAllocator  allocator  = nullptr;
	void*         mapped     = nullptr;
	bool          pending    = false;
	bool          dlss       = false;
	uint64_t      ready_at   = 0;
	vk::Extent2D  extent {};
	uint32_t      points[kProbePoints][2] {};
};
Probe g_probe;

float HalfToFloat(uint16_t h) {
	const uint32_t sign = (h >> 15) & 1u, exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu;
	float          value = 0.0f;
	if (exponent == 0) {
		value = std::ldexp(static_cast<float>(mantissa), -24);
	} else if (exponent == 31) {
		value = mantissa == 0 ? INFINITY : NAN;
	} else {
		value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
	}
	return sign != 0 ? -value : value;
}

void ProbeLayout(vk::CommandBuffer command, vk::Image image, vk::ImageLayout from, vk::ImageLayout to) {
	vk::ImageMemoryBarrier2 barrier {};
	barrier.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.srcAccessMask       = vk::AccessFlagBits2::eMemoryWrite;
	barrier.dstStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	barrier.dstAccessMask       = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dependency {};
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers    = &barrier;
	command.pipelineBarrier2(dependency, Direct());
}

// The probe points of `image` (RGBA16F) into the buffer's section `section`.
void ProbeCopy(vk::CommandBuffer command, const SrImage& image, uint32_t section) {
	const bool general = image.layout == vk::ImageLayout::eGeneral;
	if (!general) ProbeLayout(command, image.image, image.layout, vk::ImageLayout::eGeneral);
	std::array<vk::BufferImageCopy, kProbePoints> regions {};
	for (uint32_t i = 0; i < kProbePoints; i++) {
		regions[i].bufferOffset      = (static_cast<vk::DeviceSize>(section) * kProbePoints + i) * 8u;
		regions[i].imageSubresource  = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		regions[i].imageOffset       = vk::Offset3D {static_cast<int32_t>(g_probe.points[i][0]),
                                               static_cast<int32_t>(g_probe.points[i][1]), 0};
		regions[i].imageExtent       = vk::Extent3D {1, 1, 1};
	}
	command.copyImageToBuffer(image.image, vk::ImageLayout::eGeneral, vk::Buffer(g_probe.buffer),
	                          static_cast<uint32_t>(regions.size()), regions.data(), Direct());
	if (!general) ProbeLayout(command, image.image, vk::ImageLayout::eGeneral, image.layout);
}

} // namespace

// Renderer thread, after a DLSS evaluation: the probe's pixels once its frame is done on the GPU.
void ProbeReport() {
	if (!g_probe.pending || g_dlss_evaluations < g_probe.ready_at) return;
	g_probe.pending = false;
	vmaInvalidateAllocation(g_probe.allocator, g_probe.allocation, 0, VK_WHOLE_SIZE);
	const auto* texels = static_cast<const uint16_t*>(g_probe.mapped);
	const auto  pixel  = [&](uint32_t section, uint32_t i, uint32_t c) {
        return HalfToFloat(texels[(section * kProbePoints + i) * 4u + c]);
	};
	const char* names[kProbeImages] {"input", "game", "dlss"};
	double      sum[kProbeImages][5] {};
	float       amin[kProbeImages], amax[kProbeImages];
	uint32_t    bad[kProbeImages] {};
	for (uint32_t s = 0; s < kProbeImages; s++) {
		amin[s] = INFINITY;
		amax[s] = -INFINITY;
		for (uint32_t i = 0; i < kProbePoints; i++) {
			float v[4];
			for (uint32_t c = 0; c < 4; c++) v[c] = pixel(s, i, c);
			if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || !std::isfinite(v[3])) {
				bad[s]++;
				continue;
			}
			for (uint32_t c = 0; c < 4; c++) sum[s][c] += v[c];
			sum[s][4] += 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
			amin[s] = std::min(amin[s], v[3]);
			amax[s] = std::max(amax[s], v[3]);
		}
	}
	std::printf("TAAPROBE %ux%u, %u points, dlss %s\n", g_probe.extent.width, g_probe.extent.height, kProbePoints,
	            g_probe.dlss ? "ran" : "did not run");
	for (uint32_t s = 0; s < kProbeImages; s++) {
		const double n = std::max(1u, kProbePoints - bad[s]);
		std::printf("TAAPROBE mean %-5s r %.4f g %.4f b %.4f a %.4f luma %.4f | alpha %.4f..%.4f | nan/inf %u\n", names[s],
		            sum[s][0] / n, sum[s][1] / n, sum[s][2] / n, sum[s][3] / n, sum[s][4] / n, static_cast<double>(amin[s]),
		            static_cast<double>(amax[s]), bad[s]);
	}
	if (sum[1][4] > 0.0) {
		std::printf("TAAPROBE luma dlss/game %.3f, input/game %.3f\n", sum[2][4] / sum[1][4], sum[0][4] / sum[1][4]);
	}
	for (uint32_t i = 0; i < kProbePoints; i++) {
		std::printf("TAAPROBE p %4u %4u", g_probe.points[i][0], g_probe.points[i][1]);
		for (uint32_t s = 0; s < kProbeImages; s++) {
			std::printf(" | %s %.4f %.4f %.4f %.4f", names[s], static_cast<double>(pixel(s, i, 0)),
			            static_cast<double>(pixel(s, i, 1)), static_cast<double>(pixel(s, i, 2)),
			            static_cast<double>(pixel(s, i, 3)));
		}
		std::printf("\n");
	}
	std::fflush(stdout);
}
#endif

bool TaaProbeWanted(vk::Extent2D extent) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return g_probe_requests.load(std::memory_order_relaxed) > 0 && !g_probe.pending && DlssEnabled() &&
	       extent == g_dlss_extent;
#else
	(void)extent;
	return false;
#endif
}

void TaaProbe(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& color, const SrImage& depth,
              const SrImage& motion, const SrImage& output, const TaaExposure& exposure) {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	g_probe_requests.fetch_sub(1);
	const auto rgba16f = vk::Format::eR16G16B16A16Sfloat;
	const char* refused = color.format != rgba16f || output.format != rgba16f ? "not RGBA16F"
	                    : !(color.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
	                              !(output.usage & vk::ImageUsageFlagBits::eTransferSrc) ? "no transfer-source usage"
	                    : color.layout == vk::ImageLayout::eUndefined || output.layout == vk::ImageLayout::eUndefined
	                        ? "undefined layout" : nullptr;
	if (refused == nullptr && g_probe.buffer == VK_NULL_HANDLE) {
		VkBufferCreateInfo buffer_info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer_info.size        = static_cast<VkDeviceSize>(kProbeImages) * kProbePoints * 8u;
		buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo allocation_info {};
		allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
		VmaAllocationInfo info {};
		if (vmaCreateBuffer(graphics.allocator, &buffer_info, &allocation_info, &g_probe.buffer, &g_probe.allocation,
		                    &info) != VK_SUCCESS || info.pMappedData == nullptr) {
			g_probe.buffer = VK_NULL_HANDLE;
			refused        = "no readback buffer";
		} else {
			g_probe.allocator = graphics.allocator;
			g_probe.mapped    = info.pMappedData;
		}
	}
	if (refused != nullptr) {
		std::printf("TAAPROBE skipped: %s (color %d, output %d)\n", refused, static_cast<int>(color.format),
		            static_cast<int>(output.format));
		std::fflush(stdout);
		(void)EvaluateDlss(graphics, command, color, depth, motion, output, exposure);
		return;
	}
	g_probe.extent = color.extent;
	for (uint32_t j = 0; j < kProbeRows; j++) {
		for (uint32_t i = 0; i < kProbeCols; i++) {
			g_probe.points[j * kProbeCols + i][0] = (2 * i + 1) * color.extent.width / (2 * kProbeCols);
			g_probe.points[j * kProbeCols + i][1] = (2 * j + 1) * color.extent.height / (2 * kProbeRows);
		}
	}
	DrainRecording();
	DirectFullBarrier(command);
	ProbeCopy(command, color, 0);
	ProbeCopy(command, output, 1);
	DirectFullBarrier(command);
	g_probe.dlss = EvaluateDlss(graphics, command, color, depth, motion, output, exposure);
	DirectFullBarrier(command);
	ProbeCopy(command, output, 2);
	DirectFullBarrier(command);
	g_probe.pending  = true;
	g_probe.ready_at = g_dlss_evaluations + 12;
	std::printf("TAAPROBE recorded (color layout %d, output layout %d)\n", static_cast<int>(color.layout),
	            static_cast<int>(output.layout));
	std::fflush(stdout);
#else
	(void)graphics;
	(void)command;
	(void)color;
	(void)depth;
	(void)motion;
	(void)output;
	(void)exposure;
#endif
}

// ---- In-game switches: F6 DLAA, Shift+F6 NIS, Ctrl+F6 / Ctrl+Shift+F6 NIS sharpness, F4 the 30 fps cap, F12 the FPS panel (window.cpp),
// remembered in KYTY_TOGGLE_FILE ----

namespace {

std::atomic<int>                      g_flip_floor {0};
std::atomic<int>                      g_hud_shown {-1}; // F12: -1 KYTY_FPS_HUD, 0 hidden, 1 shown
int                                   g_flip_floor_on = 1; // the cap's rate when on (KYTY_FLIP_RATE, else 1: 30 fps)
std::mutex                            g_toggle_mutex;
std::string                           g_notice;
std::chrono::steady_clock::time_point g_notice_until {};

bool DlssSwitchOn() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return !g_dlss_off.load();
#else
	return false;
#endif
}
bool NisSwitchOn() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	return !g_nis_off.load();
#else
	return false;
#endif
}

// NIS sharpness in hundredths: the live/Ctrl+F6 value, else KYTY_NIS_SHARPNESS, else 30.
int NisSharpnessX100() {
#ifdef KYTY_FRAMEGEN_STREAMLINE
	if (const int live = g_nis_sharpness_x100.load(); live >= 0) return live;
#endif
	const char* env = std::getenv("KYTY_NIS_SHARPNESS");
	return env != nullptr ? std::clamp(static_cast<int>(std::atof(env) * 100.0 + 0.5), 0, 100) : 30;
}

void LoadToggles() {
	static std::once_flag once;
	std::call_once(once, [] {
		const char* rate = std::getenv("KYTY_FLIP_RATE");
		const int   env  = std::clamp(rate != nullptr ? std::atoi(rate) : 0, 0, 2);
		g_flip_floor_on  = env > 0 ? env : 1;
		g_flip_floor     = env;
		const char* path = std::getenv("KYTY_TOGGLE_FILE");
		if (path == nullptr || *path == '\0') return;
		std::ifstream file(path);
		std::string   line;
		while (std::getline(file, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const auto split = line.find('=');
			if (split == std::string::npos) continue;
			const auto key = line.substr(0, split);
			const bool on  = std::atoi(line.c_str() + split + 1) != 0;
			if (key == "cap") {
				g_flip_floor = on ? g_flip_floor_on : 0;
			} else if (key == "hud") {
				g_hud_shown = on ? 1 : 0;
#ifdef KYTY_FRAMEGEN_STREAMLINE
			} else if (key == "dlss") {
				g_dlss_off = !on;
			} else if (key == "nis") {
				g_nis_off = !on;
			} else if (key == "sharp") {
				g_nis_sharpness_x100 = std::clamp(std::atoi(line.c_str() + split + 1), 0, 100);
#endif
			}
		}
		std::printf("Frame generation: switches from %s: DLSS %s, NIS %s (sharpness %.2f), 30 fps cap %s\n", path,
		            DlssSwitchOn() ? "on" : "off", NisSwitchOn() ? "on" : "off", NisSharpnessX100() / 100.0,
		            g_flip_floor.load() > 0 ? "on" : "off");
		std::fflush(stdout);
	});
}

void SaveToggles() {
	const char* path = std::getenv("KYTY_TOGGLE_FILE");
	if (path == nullptr || *path == '\0') return;
	std::ofstream file(path, std::ios::trunc);
	file << "dlss=" << (DlssSwitchOn() ? 1 : 0) << "\nnis=" << (NisSwitchOn() ? 1 : 0)
	     << "\ncap=" << (g_flip_floor.load() > 0 ? 1 : 0) << "\nsharp=" << NisSharpnessX100() << "\n";
	if (const int hud = g_hud_shown.load(); hud >= 0) file << "hud=" << hud << "\n";
}

} // namespace

void Toggle(Switch which) {
	LoadToggles();
	std::string note;
	switch (which) {
		case Switch::Dlss:
#ifdef KYTY_FRAMEGEN_STREAMLINE
			if (g_dlss_ready) {
				g_dlss_off = !g_dlss_off.load();
				note       = g_dlss_off ? "DLAA off: the game's TAA  [F6]" : "DLAA on  [F6]";
				break;
			}
#endif
			note = "DLSS is not available here (start with KYTY_DLSS / KYTY_NIS / KYTY_LATE_DLSS: the late DLSS launcher)";
			break;
		case Switch::Nis:
#ifdef KYTY_FRAMEGEN_STREAMLINE
			if (g_late_ready) {
				g_nis_off = !g_nis_off.load();
				note      = g_nis_off ? "Late DLSS off: the game's own scaling  [Shift+F6]"
				                      : "Late DLSS on: the finished picture upscaled by DLSS to " + std::to_string(g_late_height) +
				                            (g_late_aa ? ", DLSS anti-aliasing" : "") + "  [Shift+F6]";
				break;
			}
			if (g_nis_ready) {
				g_nis_off = !g_nis_off.load();
				note      = g_nis_off ? "NIS off: the game's own scaling  [Shift+F6]"
				                      : "NIS on: sharper scaling to " + std::to_string(g_nis_height) + "  [Shift+F6]";
				break;
			}
#endif
			note = "NIS is not available here (start with KYTY_DLSS / KYTY_NIS / KYTY_LATE_DLSS: the late DLSS launcher)";
			break;
		case Switch::NisSharper:
		case Switch::NisSofter: {
#ifdef KYTY_FRAMEGEN_STREAMLINE
			if (g_nis_ready) {
				// Steps of 0.15 from 0 to 0.90, then 1.0 (the default 0.30 is one of them); Ctrl+F6 up, Ctrl+Shift+F6 down.
				static constexpr int kSteps[] {0, 15, 30, 45, 60, 75, 90, 100};
				const int now  = NisSharpnessX100();
				int       next = now;
				if (which == Switch::NisSharper) {
					next = 100;
					for (const int step : kSteps) {
						if (step > now) {
							next = step;
							break;
						}
					}
				} else {
					next = 0;
					for (const int step : kSteps) {
						if (step < now) next = step;
					}
				}
				g_nis_sharpness_x100 = next;
				g_nis_options_dirty  = true;
				char text[96];
				std::snprintf(text, sizeof(text), "NIS sharpening %.2f%s  [Ctrl+F6 / Ctrl+Shift+F6]", next / 100.0,
				              g_nis_off ? " (NIS is off: Shift+F6)" : next == 100 ? " (maximum)" : next == 0 ? " (none)" : "");
				note = text;
				break;
			}
#endif
			note = "NIS is not available here (start with KYTY_DLSS / KYTY_NIS / KYTY_LATE_DLSS: the late DLSS launcher)";
			break;
		}
		case Switch::Hud: {
			const char* env   = std::getenv("KYTY_FPS_HUD");
			const bool  shown = g_hud_shown.load() >= 0 ? g_hud_shown.load() != 0
			                                            : env != nullptr && *env != '\0' && std::string_view(env) != "0";
			g_hud_shown = shown ? 0 : 1;
			note        = shown ? "FPS panel hidden  [F12]" : "FPS panel shown  [F12]";
			break;
		}
		case Switch::Cap:
			g_flip_floor = g_flip_floor.load() > 0 ? 0 : g_flip_floor_on;
			note = g_flip_floor.load() > 0
			           ? (Enabled() ? "30 fps cap on: 60 with frame generation  [F4]" : "30 fps cap on  [F4]")
			           : "30 fps cap off: as fast as it runs  [F4]";
			break;
	}
	SaveToggles();
	std::printf("Frame generation: %s\n", note.c_str());
	std::fflush(stdout);
	std::lock_guard lock(g_toggle_mutex);
	g_notice       = note;
	g_notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
}

int FlipRateFloor() {
	LoadToggles();
	return g_flip_floor.load(std::memory_order_relaxed);
}

bool HudShown(bool fallback) {
	LoadToggles();
	const int shown = g_hud_shown.load(std::memory_order_relaxed);
	return shown >= 0 ? shown != 0 : fallback;
}

void ShowNotice(std::string note) {
	std::lock_guard lock(g_toggle_mutex);
	g_notice       = std::move(note);
	g_notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
}

std::string HudNotice() {
	std::lock_guard lock(g_toggle_mutex);
	if (g_notice.empty() || std::chrono::steady_clock::now() >= g_notice_until) return {};
	return g_notice;
}

bool UsesFsr() {
	return g_fsr;
}

bool FsrMarksHud() {
	return g_fsr || g_fsr_native;
}

uint64_t FsrVideoMemoryReserve() {
	// (Measured with a 2560x1440 swapchain: the driver's share of the video memory 1.33-1.38 GiB with FSR, 0.55-0.70
	// without; and the HUD images.) KYTY_FSR_VRAM_RESERVE_MB overrides it.
	static const uint64_t reserve = [] {
		const char* value = std::getenv("KYTY_FSR_VRAM_RESERVE_MB");
		return (value != nullptr ? std::strtoull(value, nullptr, 10) : 1536ull) << 20u;
	}();
	return g_fsr ? reserve : 0;
}

void NoteUiCanvas(uint64_t address) {
#ifdef KYTY_FRAMEGEN_FSR_HUD
	if (g_ui_canvas.exchange(address, std::memory_order_relaxed) != address) {
		std::printf("Frame generation: FSR HUD from the interface canvas at 0x%" PRIx64 "\n", address);
		std::fflush(stdout);
	}
#else
	(void)address;
#endif
}

void RecordHudless(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs, vk::Image backbuffer,
                   vk::Extent2D extent, vk::Rect2D region, vk::Format format) {
#ifdef KYTY_FRAMEGEN_FSR_HUD
	auto& h = g_fsr_hud;
	h.ready = false;
	// KYTY_FSR_HUD=0: no HUD-less image (FSR interpolates the HUD with the scene); KYTY_FSR_HUD_ALPHA: the canvas alpha
	// that counts as HUD (default 0.02); KYTY_FSR_HUD_INVERT=1: the canvas's alpha taken as 1 - coverage.
	static const bool  enabled   = [] {
		const char* value = std::getenv("KYTY_FSR_HUD");
		return value == nullptr || std::atoi(value) != 0;
	}();
	static const float threshold = [] {
		const char* value = std::getenv("KYTY_FSR_HUD_ALPHA");
		return value != nullptr ? static_cast<float>(std::atof(value)) : 0.02f;
	}();
	static const bool  invert    = [] {
		const char* value = std::getenv("KYTY_FSR_HUD_INVERT");
		return value != nullptr && std::atoi(value) != 0;
	}();
	// (The Windows backend's swapchain while it generates, in its format; or FSR 3 built in, in the backbuffer's.)
	bool generating = g_fsr_native && PresenterGenerates() && format != vk::Format::eUndefined;
#ifdef KYTY_FRAMEGEN_FSR
	if (!g_fsr_native) {
		generating = OwnsSwapchain() && g_fsr_state.fg_context != nullptr && g_fsr_state.mode_on;
		format     = g_fsr_state.format;
	}
#endif
	if (!enabled || !generating || inputs == nullptr || !inputs->valid || !inputs->ui_valid || extent.width == 0 ||
	    extent.height == 0) {
		return;
	}
	if (!FsrHudSetup(graphics, format, extent)) return;
	h.index     = (h.index + 1u) % static_cast<uint32_t>(h.images.size());
	auto& image = h.images[h.index];
	// The backbuffer as the game image's blit left it, copied.
	ImageBarrier(command, backbuffer, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
	             vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eTransferRead);
	Transition(command, image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferDstOptimal,
	           vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eTransfer);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.extent         = vk::Extent3D {extent.width, extent.height, 1};
	command.copyImage(backbuffer, vk::ImageLayout::eTransferSrcOptimal, image.image, vk::ImageLayout::eTransferDstOptimal, copy);
	// (And once more as the pass's input: it reads around each HUD pixel.)
	Transition(command, h.source, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferDstOptimal,
	           vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eTransfer);
	command.copyImage(backbuffer, vk::ImageLayout::eTransferSrcOptimal, h.source.image, vk::ImageLayout::eTransferDstOptimal,
	                  copy);
	ImageBarrier(command, backbuffer, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eTransferDstOptimal,
	             vk::AccessFlagBits2::eTransferRead, vk::AccessFlagBits2::eTransferWrite);
	Transition(command, h.source, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal,
	           vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eFragmentShader);
	// The HUD replaced in the copy by the scene around it.
	Transition(command, image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eColorAttachmentOptimal,
	           vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
	           vk::PipelineStageFlagBits2::eColorAttachmentOutput);
	vk::RenderingAttachmentInfo color {};
	color.imageView   = h.views[h.index];
	color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	color.loadOp      = vk::AttachmentLoadOp::eLoad;
	color.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &color;
	command.beginRendering(rendering);
	const vk::Viewport viewport {0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
	const vk::Rect2D   scissor {{0, 0}, extent};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, h.pipeline);
	std::array<vk::DescriptorImageInfo, 2> sampled {};
	sampled[0].imageView   = inputs->ui_view;
	sampled[0].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	sampled[1].imageView   = h.source_view;
	sampled[1].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); ++i) {
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		writes[i].pImageInfo      = &sampled[i];
	}
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, h.layout, 0, static_cast<uint32_t>(writes.size()),
	                             writes.data());
	FsrHudParameters parameters {};
	parameters.region[0] = static_cast<float>(region.offset.x);
	parameters.region[1] = static_cast<float>(region.offset.y);
	parameters.region[2] = static_cast<float>(std::max(1u, region.extent.width));
	parameters.region[3] = static_cast<float>(std::max(1u, region.extent.height));
	parameters.extent[0] = static_cast<float>(extent.width);
	parameters.extent[1] = static_cast<float>(extent.height);
	parameters.threshold = threshold;
	parameters.invert    = invert ? 1.0f : 0.0f;
	command.pushConstants(h.layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(parameters), &parameters);
	command.draw(3, 1, 0, 0);
	command.endRendering();
	// KYTY_FSR_HUD_VIEW=1 (diagnostic): the HUD-less image shown instead of the frame (the HUD darkened where FSR is
	// told it is).
	static const bool view = std::getenv("KYTY_FSR_HUD_VIEW") != nullptr;
	if (view) {
		Transition(command, image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferSrcOptimal,
		           vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eTransfer);
		command.copyImage(image.image, vk::ImageLayout::eTransferSrcOptimal, backbuffer, vk::ImageLayout::eTransferDstOptimal,
		                  copy);
	}
	Transition(command, image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal,
	           vk::AccessFlagBits2::eShaderRead, vk::PipelineStageFlagBits2::eAllCommands);
	static bool reported = false;
	if (!reported) {
		reported = true;
		std::printf("Frame generation: FSR HUD-less image in use (threshold %.3f%s)\n", threshold, invert ? ", inverted" : "");
		std::fflush(stdout);
	}
	h.ready = true;
#else
	(void)graphics, (void)command, (void)inputs, (void)backbuffer, (void)extent, (void)region, (void)format;
#endif
}

void OnFsrQueues(GraphicContext& graphics) {
#ifdef KYTY_FRAMEGEN_FSR
	if (!g_fsr) return;
	if (graphics.fsr_present_queue == nullptr || graphics.fsr_compute_queue == nullptr ||
	    graphics.fsr_acquire_queue == nullptr) {
		LOGF("frame generation: no queues for FSR's swapchain\n");
		std::printf("Frame generation: the GPU lacks the queues FSR's swapchain needs, FSR off\n");
		std::fflush(stdout);
		g_fsr      = false;
		g_generate = 0;
		return;
	}
	g_fsr_state.graphics = &graphics;
	g_ready              = true;
#else
	(void)graphics;
#endif
}

bool OwnsSwapchain() {
	return g_fsr && g_ready;
}

vk::Result CreateSwapchain(GraphicContext& graphics, const vk::SwapchainCreateInfoKHR& info, vk::SwapchainKHR* swapchain) {
#ifdef KYTY_FRAMEGEN_FSR
	auto& s = g_fsr_state;
	ffxCreateContextDescFrameGenerationSwapChainVK create {};
	create.header.type    = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
	create.physicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
	create.device         = static_cast<VkDevice>(graphics.device);
	VkSwapchainKHR handle = VK_NULL_HANDLE;
	create.swapchain      = &handle;
	create.allocator      = nullptr;
	create.createInfo     = static_cast<VkSwapchainCreateInfoKHR>(info);
	// The game queue is the emulator's present queue (presents go through QueuePresent there, under its lock), or without
	// one its graphics queue (under queue_mutex): the swapchain records the generated frame's work on it in the present
	// call.
	const vk::Queue game = graphics.present_queue != nullptr ? graphics.present_queue : graphics.queue;
	create.gameQueue         = {static_cast<VkQueue>(game), graphics.queue_family, nullptr};
	create.asyncComputeQueue = {static_cast<VkQueue>(graphics.fsr_compute_queue), graphics.fsr_compute_family, nullptr};
	create.presentQueue      = {static_cast<VkQueue>(graphics.fsr_present_queue), graphics.fsr_present_family, nullptr};
	create.imageAcquireQueue = {static_cast<VkQueue>(graphics.fsr_acquire_queue), graphics.fsr_compute_family, nullptr};
	if (!FsrOk(s.api.CreateContext(&s.swapchain_context, &create.header, nullptr), "swapchain")) {
		s.swapchain_context = nullptr;
		return vk::Result::eErrorInitializationFailed;
	}
	s.functions             = {};
	s.functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
	if (!FsrOk(s.api.Query(&s.swapchain_context, &s.functions.header), "swapchain functions") ||
	    s.functions.pOutAcquireNextImageKHR == nullptr || s.functions.pOutQueuePresentKHR == nullptr ||
	    s.functions.pOutGetSwapchainImagesKHR == nullptr) {
		(void)s.api.DestroyContext(&s.swapchain_context, nullptr);
		s.swapchain_context = nullptr;
		return vk::Result::eErrorInitializationFailed;
	}
	s.swapchain = handle;
	s.display   = info.imageExtent;
	s.format    = info.imageFormat;
	s.hdr       = info.imageColorSpace == vk::ColorSpaceKHR::eHdr10St2084EXT;
	s.run_with  = 0;
	s.run_without = 0;
	*swapchain  = handle;
	LOGF("frame generation: FSR swapchain %ux%u\n", s.display.width, s.display.height);
	return vk::Result::eSuccess;
#else
	(void)graphics;
	(void)info;
	(void)swapchain;
	return vk::Result::eErrorInitializationFailed;
#endif
}

void DestroySwapchain(GraphicContext& graphics, vk::SwapchainKHR swapchain) {
	(void)graphics;
	(void)swapchain;
#ifdef KYTY_FRAMEGEN_FSR
	auto& s = g_fsr_state;
	// (The frame generation context is made for the display size: again with the next swapchain.)
	FsrDestroyFrameGeneration();
	FsrHudDestroy(graphics);
	if (s.swapchain_context != nullptr) {
		(void)s.api.DestroyContext(&s.swapchain_context, nullptr); // (and its swapchain)
		s.swapchain_context = nullptr;
	}
	s.swapchain = nullptr;
	s.functions = {};
#endif
}

vk::Result GetSwapchainImages(vk::Device device, vk::SwapchainKHR swapchain, uint32_t* count, vk::Image* images) {
#ifdef KYTY_FRAMEGEN_FSR
	return static_cast<vk::Result>(g_fsr_state.functions.pOutGetSwapchainImagesKHR(
	    static_cast<VkDevice>(device), static_cast<VkSwapchainKHR>(swapchain), count, reinterpret_cast<VkImage*>(images)));
#else
	(void)device, (void)swapchain, (void)count, (void)images;
	return vk::Result::eErrorInitializationFailed;
#endif
}

vk::Result AcquireNextImage(vk::Device device, vk::SwapchainKHR swapchain, uint64_t timeout, vk::Semaphore semaphore,
                            uint32_t* index) {
#ifdef KYTY_FRAMEGEN_FSR
	return static_cast<vk::Result>(g_fsr_state.functions.pOutAcquireNextImageKHR(
	    static_cast<VkDevice>(device), static_cast<VkSwapchainKHR>(swapchain), timeout, static_cast<VkSemaphore>(semaphore),
	    VK_NULL_HANDLE, index));
#else
	(void)device, (void)swapchain, (void)timeout, (void)semaphore, (void)index;
	return vk::Result::eErrorInitializationFailed;
#endif
}

vk::Result QueuePresent(vk::Queue queue, const vk::PresentInfoKHR& present) {
#ifdef KYTY_FRAMEGEN_FSR
	const VkPresentInfoKHR& info = present;
	return static_cast<vk::Result>(g_fsr_state.functions.pOutQueuePresentKHR(static_cast<VkQueue>(queue), &info));
#else
	(void)queue, (void)present;
	return vk::Result::eErrorInitializationFailed;
#endif
}

bool SkipsFrame(const FrameInputs* inputs) {
#ifdef KYTY_FRAMEGEN_FSR
	auto& s = g_fsr_state;
	if (!OwnsSwapchain() || !s.mode_on || (inputs != nullptr && inputs->valid)) return false;
	if (s.run_without + 1 >= FsrTurnOffAfter) return false; // (RecordPrepare turns generation off with it)
	++s.run_without;
	s.run_with = 0;
	return true;
#else
	(void)inputs;
	return false;
#endif
}

bool PresenterGenerates() {
#ifdef KYTY_FRAMEGEN_NGX
	return g_ready && g_generate > 0 && !g_fg_failed && (g_dlss != nullptr || g_fsr_native);
#else
	return false;
#endif
}

void RecordPrepare(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs, vk::Rect2D region) {
	(void)graphics;
#ifdef KYTY_FRAMEGEN_FSR
	auto& s = g_fsr_state;
	if (!OwnsSwapchain() || s.swapchain == nullptr) return;
	const bool usable = inputs != nullptr && inputs->valid;
	s.run_with        = usable ? s.run_with + 1 : 0;
	s.run_without     = usable ? 0 : s.run_without + 1;
	bool mode_on      = s.mode_on ? s.run_without < FsrTurnOffAfter : s.run_with >= FsrTurnOnAfter;
	if (mode_on && usable && !FsrEnsureFrameGeneration(inputs->render_extent)) mode_on = false;
	if (s.fg_context == nullptr) return; // (no frame with inputs yet: the swapchain presents as it is)
	if (mode_on != s.mode_on) {
		LOGF("frame generation: FSR %s\n", mode_on ? "on" : "off (no inputs)");
		s.mode_on = mode_on;
		s.reset   = true;
	}
	// (A frame left unshown before this one: its motion is to the game's previous frame, not to the one shown last.)
	if (g_fsr_discontinuity.exchange(false, std::memory_order_relaxed) || (usable && inputs->cut)) s.reset = true;
	const auto now   = std::chrono::steady_clock::now();
	const float delta = s.last_frame.time_since_epoch().count() == 0
	                        ? 16.7f
	                        : std::chrono::duration<float, std::milli>(now - s.last_frame).count();
	s.last_frame     = now;
	const uint64_t id = ++s.frame_id;

	static const uint32_t debug_flags = [] {
		// KYTY_FRAMEGEN_DEBUG=1: tear and reset lines, 2: FSR's debug view.
		const char* value = std::getenv("KYTY_FRAMEGEN_DEBUG");
		const int   level = value != nullptr ? std::atoi(value) : 0;
		return level >= 2 ? uint32_t(FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW)
		       : level == 1 ? uint32_t(FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS)
		                    : 0u;
	}();
	ffxConfigureDescFrameGeneration config {};
	config.header.type                        = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
	config.swapChain                          = static_cast<VkSwapchainKHR>(s.swapchain);
	config.presentCallback                    = nullptr;
	config.frameGenerationCallback            = FsrGenerate;
	config.frameGenerationCallbackUserContext = &s.fg_context;
	config.frameGenerationEnabled             = mode_on;
	config.allowAsyncWorkloads                = false;
	config.flags                              = debug_flags;
	config.onlyPresentGenerated               = false;
	// (The HUD-less image of this frame, RecordHudless: FSR takes the HUD from the game's frame.)
	if (g_fsr_hud.ready) config.HUDLessColor = FsrImage(g_fsr_hud.images[g_fsr_hud.index]);
	g_fsr_hud.ready = false;
	config.generationRect                     = {region.offset.x, region.offset.y, static_cast<int32_t>(region.extent.width),
	                                             static_cast<int32_t>(region.extent.height)};
	config.frameID                            = id;
	if (!FsrOk(s.api.Configure(&s.fg_context, &config.header), "configure") || !mode_on) return;

	const auto camera = FsrDecompose(inputs->camera.view_proj);
	const float width  = static_cast<float>(inputs->render_extent.width);
	const float height = static_cast<float>(inputs->render_extent.height);
	// The game's vectors in UV units along the motion (see BeforePresent): FSR wants pixels towards the previous
	// position.
	static const float mv_sign = [] {
		const char* sign = std::getenv("KYTY_FRAMEGEN_MV_SIGN");
		return sign != nullptr && std::atof(sign) > 0 ? 1.0f : -1.0f;
	}();
	static const bool pixels_mv = [] {
		const char* mv = std::getenv("KYTY_FRAMEGEN_MV");
		return mv != nullptr && std::string(mv) == "px";
	}();
	ffxDispatchDescFrameGenerationPrepareCameraInfo camera_info {};
	camera_info.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
	for (int i = 0; i < 3; ++i) {
		camera_info.cameraPosition[i] = camera.position[i];
		camera_info.cameraUp[i]       = camera.up[i];
		camera_info.cameraRight[i]    = camera.right[i];
		camera_info.cameraForward[i]  = camera.forward[i];
	}
	ffxDispatchDescFrameGenerationPrepare prepare {};
	prepare.header.type       = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
	prepare.header.pNext      = camera.valid ? &camera_info.header : nullptr;
	prepare.frameID           = id;
	prepare.flags             = debug_flags;
	prepare.commandList       = static_cast<VkCommandBuffer>(command);
	prepare.renderSize        = {inputs->render_extent.width, inputs->render_extent.height};
	// (The sample's convention: the negated offset in pixels of a projection jittered by 2 * offset / size.)
	prepare.jitterOffset      = {-inputs->camera.jitter_ndc[0] * width * 0.5f, inputs->camera.jitter_ndc[1] * height * 0.5f};
	prepare.motionVectorScale = pixels_mv ? FfxApiFloatCoords2D {mv_sign, mv_sign}
	                                      : FfxApiFloatCoords2D {mv_sign * width, mv_sign * height};
	prepare.frameTimeDelta    = delta;
	prepare.cameraNear        = camera.valid ? camera.z_near : 0.1f;
	prepare.cameraFar         = FLT_MAX;
	prepare.cameraFovAngleVertical  = camera.valid ? 2.0f * std::atan(1.0f / std::fabs(camera.sy)) : 1.0f;
	prepare.viewSpaceToMetersFactor = 1.0f;
	prepare.depth             = FsrImage(inputs->depth);
	prepare.motionVectors     = FsrImage(inputs->motion);
	(void)FsrOk(s.api.Dispatch(&s.fg_context, &prepare.header), "prepare");
#else
	(void)command, (void)inputs, (void)region;
#endif
}

void NotePresented(uint32_t frames) {
	g_presented_frames.store(std::max(frames, 1u), std::memory_order_relaxed);
}

bool PresenterGeneratesHdr() {
	return PresenterGenerates() && g_fsr_native;
}

uint32_t Generate(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs, const UpscaleImage& backbuffer,
                  vk::Rect2D region, std::span<const UpscaleImage> outputs, bool hdr) {
#ifdef KYTY_FRAMEGEN_NGX
	if (!PresenterGenerates()) return 0;
	const bool discontinuity =
	    g_fg_discontinuity.exchange(false, std::memory_order_relaxed) || (inputs != nullptr && inputs->cut);
	if (inputs == nullptr || !inputs->valid) {
		// (Menus, movies, loading: the frame shown as it is; the next one with inputs starts DLSS-G again.)
		g_fg_history = false;
		return 0;
	}
	if (g_fsr_native) {
		// FSR 3 built in: one frame between the last one and this, from the same inputs as DLSS-G's.
		if (outputs.empty()) return 0;
		const auto image = [](const VulkanImage& source, vk::ImageView view) {
			return UpscaleImage {source.image, view, vk::ImageLayout::eShaderReadOnlyOptimal, source.format,
			                     vk::Extent2D {source.extent.width, source.extent.height}, source.usage, 0, 0};
		};
		FsrNative::InterpolationInputs fsr;
		fsr.backbuffer   = backbuffer;
		fsr.depth        = image(inputs->depth, inputs->depth_view);
		fsr.motion       = image(inputs->motion, inputs->motion_view);
		fsr.region       = region;
		const float width  = static_cast<float>(inputs->render_extent.width);
		const float height = static_cast<float>(inputs->render_extent.height);
		// (FSR's documented convention, DLSS-G's here: the projection's offset in pixels, y down.)
		fsr.jitter_px = {inputs->camera.jitter_ndc[0] * width * 0.5f, -inputs->camera.jitter_ndc[1] * height * 0.5f};
		if (const auto camera = Decompose(inputs->camera.view_proj); camera.valid) {
			fsr.near_plane = camera.z_near;
			fsr.fov_y      = 2.0f * std::atan(1.0f / std::fabs(camera.sy));
		}
		// The game's frame time: the time between the frames given here, a long gap (a load, a pause) left out.
		static auto last  = std::chrono::steady_clock::time_point {};
		static float frame_ms = 16.7f;
		const auto now = std::chrono::steady_clock::now();
		if (last != std::chrono::steady_clock::time_point {}) {
			const float ms = std::chrono::duration<float, std::milli>(now - last).count();
			if (ms > 2.0f && ms < 100.0f) frame_ms = ms;
		}
		last         = now;
		fsr.frame_ms = frame_ms;
		fsr.reset    = !g_fg_history || discontinuity;
		fsr.pq       = hdr;
		// (This frame's HUD-less image, RecordHudless: FSR takes the HUD and the menus from the game's frame.)
		if (auto& h = g_fsr_hud; h.ready) {
			const auto& hud = h.images[h.index];
			fsr.hudless     = {hud.image,       h.views[h.index], vk::ImageLayout::eShaderReadOnlyOptimal, hud.format,
			                   backbuffer.extent, hud.usage,       0,                                      0};
		}
		const bool made = FsrNative::Interpolate(graphics, command, fsr, outputs[0]);
		g_fg_history    = true;
		return made ? 1u : 0u;
	}
	const auto count = static_cast<uint32_t>(std::min<size_t>(outputs.size(), static_cast<size_t>(g_generate)));
	if (count == 0) return 0;
	if (!g_fg_created || g_fg_backbuffer != backbuffer.extent || g_fg_format != backbuffer.format ||
	    g_fg_render != inputs->render_extent) {
		KytyDlssgFeature feature {};
		feature.width         = backbuffer.extent.width;
		feature.height        = backbuffer.extent.height;
		feature.format        = static_cast<VkFormat>(backbuffer.format);
		feature.render_width  = inputs->render_extent.width;
		feature.render_height = inputs->render_extent.height;
		if (g_dlss->FrameGenCreate(static_cast<VkCommandBuffer>(command), &feature) == 0) {
			std::printf("Frame generation: NGX did not make DLSS-G for %ux%u (depth and motion %ux%u): off\n", feature.width,
			            feature.height, feature.render_width, feature.render_height);
			std::fflush(stdout);
			g_fg_failed = true;
			return 0;
		}
		std::printf("Frame generation: DLSS-G %ux%u, depth and motion %ux%u, %ux (NVIDIA NGX)\n", feature.width,
		            feature.height, feature.render_width, feature.render_height, count + 1);
		std::fflush(stdout);
		g_fg_created    = true;
		g_fg_backbuffer = backbuffer.extent;
		g_fg_format     = backbuffer.format;
		g_fg_render     = inputs->render_extent;
		g_fg_history    = false;
	}
	// (No frame before this one with inputs, or one left unshown in between: this one starts DLSS-G's history and its
	// generated frames are not shown.)
	const bool reset = !g_fg_history || discontinuity;

	KytyDlssgEvaluate evaluate {};
	evaluate.backbuffer = BridgeImage(backbuffer, vk::ImageAspectFlagBits::eColor);
	evaluate.depth      = BridgeImage(inputs->depth, inputs->depth_view);
	evaluate.motion     = BridgeImage(inputs->motion, inputs->motion_view);
	if (HudlessOn() && inputs->hudless.has_display) evaluate.hudless = BridgeImage(inputs->hudless.display, inputs->hudless.display_view);
	// The camera as the Streamline way gives it (BeforePresent): defaults for a frame whose matrix does not split.
	auto view_to_clip = Identity(), clip_to_view = Identity(), clip_to_prev = Identity(), prev_to_clip = Identity();
	evaluate.mv_scale_x = evaluate.mv_scale_y = 1.0f;
	evaluate.camera_up[1]    = 1.0f;
	evaluate.camera_right[0] = 1.0f;
	evaluate.camera_fwd[2]   = 1.0f;
	evaluate.camera_near     = 0.1f;
	evaluate.camera_far      = 10000.0f;
	evaluate.camera_fov      = 1.0f;
	evaluate.camera_aspect   = 16.0f / 9.0f;
	if (const auto camera = Decompose(inputs->camera.view_proj); camera.valid) {
		// Projection in view space: x' = sx*x + cx*z, y' = sy*y + cy*z, z' = near, w' = z.
		RowMatrix projection {};
		projection[0]  = camera.sx;
		projection[5]  = camera.sy;
		projection[8]  = camera.cx;
		projection[9]  = camera.cy;
		projection[11] = 1.0f;
		projection[14] = camera.z_near;
		view_to_clip   = projection;
		(void)Inverted(projection, clip_to_view);
		const auto current  = Transposed(inputs->camera.view_proj);
		const auto previous = Transposed(inputs->camera.prev_view_proj);
		if (RowMatrix clip_to_world {}; Inverted(current, clip_to_world)) {
			clip_to_prev = Multiply(clip_to_world, previous);
			(void)Inverted(clip_to_prev, prev_to_clip);
		}
		const float width  = static_cast<float>(inputs->render_extent.width);
		const float height = static_cast<float>(inputs->render_extent.height);
		evaluate.jitter_x  = inputs->camera.jitter_ndc[0] * width * 0.5f;
		evaluate.jitter_y  = -inputs->camera.jitter_ndc[1] * height * 0.5f;
		// The game's vectors point along the motion, in UV units (its TAA reads the history at uv - mv); DLSS-G wants
		// them towards the previous position, so they are negated (the Streamline way's KYTY_FRAMEGEN_MV_SIGN and
		// KYTY_FRAMEGEN_MV=px, the same tests).
		static const bool pixels_mv = [] {
			const char* mv = std::getenv("KYTY_FRAMEGEN_MV");
			return mv != nullptr && std::string(mv) == "px";
		}();
		static const float mv_sign = [] {
			const char* sign = std::getenv("KYTY_FRAMEGEN_MV_SIGN");
			return sign != nullptr && std::atof(sign) > 0 ? 1.0f : -1.0f;
		}();
		evaluate.mv_scale_x = pixels_mv ? mv_sign / width : mv_sign;
		evaluate.mv_scale_y = pixels_mv ? mv_sign / height : mv_sign;
		for (int i = 0; i < 3; ++i) {
			evaluate.camera_pos[i]   = camera.position[i];
			evaluate.camera_up[i]    = camera.up[i];
			evaluate.camera_right[i] = camera.right[i];
			evaluate.camera_fwd[i]   = camera.forward[i];
		}
		evaluate.camera_near   = camera.z_near;
		evaluate.camera_far    = 100000.0f;
		evaluate.camera_fov    = 2.0f * std::atan(1.0f / std::fabs(camera.sy));
		evaluate.camera_aspect = std::fabs(camera.sy / camera.sx);
	}
	Copy16(evaluate.camera_view_to_clip, view_to_clip);
	Copy16(evaluate.clip_to_camera_view, clip_to_view);
	Copy16(evaluate.clip_to_prev_clip, clip_to_prev);
	Copy16(evaluate.prev_clip_to_clip, prev_to_clip);
	evaluate.depth_inverted = 1;
	evaluate.reset          = reset ? 1 : 0;
	// The backbuffer's part with the game image (KYTY_PRESENT_ASPECT, KYTY_RENDER_ASPECT: black around it).
	if (region.offset.x != 0 || region.offset.y != 0 || region.extent.width != backbuffer.extent.width ||
	    region.extent.height != backbuffer.extent.height) {
		evaluate.backbuffer_x      = static_cast<uint32_t>(region.offset.x);
		evaluate.backbuffer_y      = static_cast<uint32_t>(region.offset.y);
		evaluate.backbuffer_width  = region.extent.width;
		evaluate.backbuffer_height = region.extent.height;
	}
	evaluate.multi_frame_count = count;
	uint32_t made              = 0;
	for (uint32_t i = 0; i < count; ++i) {
		evaluate.output            = BridgeImage(outputs[i], vk::ImageAspectFlagBits::eColor);
		evaluate.multi_frame_index = i + 1;
		if (g_dlss->FrameGenEvaluate(static_cast<VkCommandBuffer>(command), &evaluate) == 0) break;
		++made;
	}
	if (made < count) {
		static uint32_t failures = 0;
		if (++failures >= 3) {
			g_fg_failed = true;
			std::printf("Frame generation: NGX refused DLSS-G %u times: off from here on\n", failures);
		} else {
			std::printf("Frame generation: NGX's DLSS-G evaluation failed (this frame shown alone)\n");
		}
		std::fflush(stdout);
		g_fg_history = false;
		return 0;
	}
	g_fg_history = true;
	return reset ? 0u : made;
#else
	(void)graphics;
	(void)command;
	(void)inputs;
	(void)backbuffer;
	(void)region;
	(void)outputs;
	(void)hdr;
	return 0;
#endif
}

// ---- KYTY_LIVE_FILE switches for DLSS / NIS (tools: _dlss\\live-commands.txt) ----

bool LiveCommand(std::string_view command, std::string_view value) {
#ifndef KYTY_FRAMEGEN_STREAMLINE
	(void)command;
	(void)value;
	return false;
#else
	const bool on = value == "on" || value == "1", off = value == "off" || value == "0";
	if (command == "dlss" && (on || off)) {
		g_dlss_off = off;
	} else if (command == "nis" && (on || off)) {
		g_nis_off = off;
	} else if (command == "dlssexp" && (value == "auto" || off)) {
		g_dlss_auto_exposure = value == "auto" ? 1 : 0;
		g_dlss_options_dirty = true;
	} else if (command == "dlssalpha" && (on || off)) {
		g_dlss_alpha         = on ? 1 : 0;
		g_dlss_options_dirty = true;
	} else if (command == "dlssexposure" && (on || off)) {
		g_exposure_on = on;
	} else if (command == "taaalpha") {
		if (value == "keep") {
			g_taa_alpha_mode = 0;
		} else {
			g_taa_alpha_value = static_cast<float>(std::atof(std::string(value).c_str()));
			g_taa_alpha_mode  = 1;
		}
	} else if (command == "exposurescale") {
		g_exposure_scale = static_cast<float>(std::atof(std::string(value).c_str()));
	} else if (command == "taaprobe") {
		g_probe_requests = 1;
	} else if (command == "dlsshdr" && (on || off)) {
		g_dlss_hdr           = on ? 1 : 0;
		g_dlss_options_dirty = true;
	} else if (command == "nishdr" && (value == "none" || value == "linear" || value == "pq")) {
		g_nis_hdr = value == "pq" ? static_cast<int>(sl::NISHDR::ePQ)
		          : value == "linear" ? static_cast<int>(sl::NISHDR::eLinear) : static_cast<int>(sl::NISHDR::eNone);
		g_nis_options_dirty = true;
	} else if (command == "nissharp") {
		const double sharpness = std::atof(std::string(value).c_str());
		if (sharpness < 0.0 || sharpness > 1.0) return false;
		g_nis_sharpness_x100 = static_cast<int>(sharpness * 100.0 + 0.5);
		g_nis_options_dirty  = true;
	} else {
		return false;
	}
	std::printf("Frame generation: live %.*s %.*s\n", static_cast<int>(command.size()), command.data(),
	            static_cast<int>(value.size()), value.data());
	std::fflush(stdout);
	return true;
#endif
}

const char* QualityLabel() {
#ifndef KYTY_FRAMEGEN_STREAMLINE
	return "";
#else
	static thread_local std::string label;
	label.clear();
	if (g_dlss_ready) label += g_dlss_off ? "DLAA off" : g_dlss_alpha.load() != 0 ? "DLAA on (alpha)" : "DLAA on";
	if (g_late_ready) label += std::string(label.empty() ? "" : "  |  ") + (g_nis_off ? "late DLSS off" : "late DLSS on");
	if (g_nis_ready && !g_late_ready) {
		char nis[48] = "NIS off";
		if (!g_nis_off) std::snprintf(nis, sizeof(nis), "NIS on (sharp %.2f)", NisSharpnessX100() / 100.0);
		label += std::string(label.empty() ? "" : "  |  ") + nis;
	}
	return label.c_str();
#endif
}


} // namespace Libs::Graphics::FrameGen
