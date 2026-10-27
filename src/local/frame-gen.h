#ifndef EMULATOR_SRC_LOCAL_FRAME_GEN_H_
#define EMULATOR_SRC_LOCAL_FRAME_GEN_H_
// DLSS Frame Generation through NVIDIA Streamline (KYTY_FRAMEGEN=<generated frames, 1..5>), for
// Demon's Souls PPSA01341 01.007.000 and 01.005.000. The inputs come from the game's own passes, found by frame
// capture (docs/RE-DEMONS-SOULS.md): the G-buffer vertex shaders' constants hold the camera
// (world->clip matrix, the previous frame's, and the TAA jitter), the TAA dispatch reads the depth
// and motion-vector images. The renderer thread gathers them per guest frame; at the guest flip
// they are copied next to the presenter's frame; the present thread hands them to Streamline and
// presents through it (sl.interposer.dll is the process' Vulkan loader, see Initialize()).
#include "game-profile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics {
class CommandBuffer;
class TextureCache;
namespace FrameGen {

// The game's TAA compute shader and the vertex shaders holding the camera (flattened SRT words) are the game profile's
// (game-profile.h; Demon's Souls 01.007.000 and 01.005.000 are built in: the same shaders recompiled).
inline bool IsTaaShader(uint64_t hash) {
	return ::GameProfile::Current().IsTaa(hash);
}
inline bool IsGbufferVertexShader(uint64_t hash) {
	return ::GameProfile::Current().IsCameraShader(hash);
}

struct Camera {
	std::array<float, 16> view_proj {};
	std::array<float, 16> prev_view_proj {};
	std::array<float, 2>  jitter_ndc {};
	bool                  valid = false;
};

// KYTY_FG_HUDLESS (frame-gen-hudless.h): a frame's game image without its HUD, its pixels as the frame image's
// (`image`), and that laid out as the backbuffer (`display`, the present's: DLSS-G's HUD-less color).
struct HudlessImages {
	std::unique_ptr<VulkanImage> image;
	VulkanImage                  display;
	vk::ImageView                display_view = nullptr;
	bool                         has_image = false, has_display = false;
};

// One presented frame's inputs, owned by the presenter's frame.
struct FrameInputs {
	VulkanImage   depth, motion;
	vk::ImageView depth_view = nullptr, motion_view = nullptr;
	vk::Extent2D  render_extent {};
	Camera        camera;
	uint64_t      guest_frame = 0;
	bool          motion_attachment = false; // the motion copy is a color attachment too
	HudlessImages hudless;                   // KYTY_FG_HUDLESS
	bool          valid       = false;
	// The game started its upscaler's history again in this frame (a camera cut: NoteCut): no frame is generated across
	// it, which would blend the two views.
	bool          cut         = false;
	// KYTY_FRAMEGEN_BACKEND=fsr: a copy of the game's interface canvas (its alpha: where the HUD is), when known.
	VulkanImage   ui;
	vk::ImageView ui_view  = nullptr;
	bool          ui_valid = false;
};

// Process start, before the window: loads Streamline when KYTY_FRAMEGEN or KYTY_UPSCALER=dlss is set (it then
// becomes SDL's Vulkan loader, and so the proxy of instance, device and swapchain calls).
void Initialize();
// After Initialize: Streamline is loaded (device creation then adds what it relies on but does
// not add itself, VK_KHR_present_id for its VK_NV_low_latency2).
[[nodiscard]] bool Requested();
// Before the device is created: why Streamline says DLSS-G cannot run with this GPU, driver and Windows (e.g.
// hardware-accelerated GPU scheduling turned off), logged; empty when it can, or when frame generation is off. A refused
// device's error repeats it.
[[nodiscard]] std::string SupportProblem(vk::PhysicalDevice physical_device);
// A refused device's advice when this process runs without bottom-up ASLR (issue #45: forcing it on for the emulator in
// Windows' Exploit protection let the driver make the device); empty otherwise.
[[nodiscard]] std::string RefusedDeviceAdvice();
// Linux (KYTY_UPSCALER=dlss through NVIDIA's NGX, the bridge libkyty_dlss.so, kyty-dlss-bridge.h): the Vulkan extensions
// NGX needs, added to `extensions` before the instance and before the device are made (DLSS off when `available` lacks
// one). Elsewhere, and without the bridge, nothing.
void AddInstanceExtensions(const std::vector<vk::ExtensionProperties>& available, std::vector<const char*>& extensions);
void AddDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical_device,
                         const std::vector<vk::ExtensionProperties>& available, std::vector<const char*>& extensions);
// After the device exists: checks that DLSS-G is available on it.
void OnDevice(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device);
// Right after the dispatcher is loaded for the device (before the recording layer copies it):
// the command-buffer calls Streamline intercepts only for its command-list state tracking
// (disabled here) go straight to the driver.
void BypassCommandHooks(vk::Device device);
[[nodiscard]] bool Enabled();

// Present thread, around vkQueuePresentKHR of the image showing `inputs` (null: a frame
// without inputs, e.g. a menu). `backbuffer` is the swapchain extent, `region` the part the
// game image covers.
void BeforePresent(const FrameInputs* inputs, vk::Extent2D backbuffer, vk::Rect2D region,
                   vk::Format format, uint32_t image_count);
void AfterPresent();
// Present thread: the swapchain is made again (DLSS-G off until frames with inputs turn it on again).
void SwapchainChanging();
// Any thread: a frame with inputs was left unshown (the presenter kept a newer one): the next one resets DLSS-G's history.
void NoteDroppedFrame();
// Present thread: the frames DLSS-G showed per presented frame (1 without generation), as last
// queried (about once a second), for the KYTY_FPS_HUD panel.
[[nodiscard]] uint32_t PresentedFrames();
// KYTY_FPS_HUD: the panel's rectangle, normalized to the game image. Its motion vectors are
// zeroed so generated frames keep the panel in place.
void SetStaticRect(float x0, float y0, float x1, float y1);

// KYTY_FRAMEGEN_BACKEND=fsr: AMD FidelityFX FSR 3.1 frame generation instead of Streamline's DLSS-G, any GPU: one
// generated frame per frame (2x), from the same inputs. amd_fidelityfx_vk.dll (AMD's signed runtime, next to the
// emulator) makes the window's swapchain (its frame generation swapchain: it interpolates and paces the presents on
// queues of its own, GraphicContext::fsr_*), so the swapchain's calls go through it.
// After Initialize: FSR asked for and its runtime loaded (device creation then adds its queues).
[[nodiscard]] bool UsesFsr();
// FSR generates frames, by the Windows backend or built in (Linux): the interface canvas is looked for (NoteUiCanvas).
[[nodiscard]] bool FsrMarksHud();
// The video memory FSR takes beside the game's (FidelityFX's allocations, outside the emulator's allocator, and its
// HUD images): what the emulator leaves it when it sizes its use of the budget (small_video_memory).
[[nodiscard]] uint64_t FsrVideoMemoryReserve();
// After the device's queues: FSR is ready when its queues were made.
void OnFsrQueues(GraphicContext& graphics);
// The swapchain is FSR's (Enabled() with the FSR backend).
[[nodiscard]] bool OwnsSwapchain();
// The swapchain calls, FSR's in place of Vulkan's (OwnsSwapchain()). Present thread.
[[nodiscard]] vk::Result CreateSwapchain(GraphicContext& graphics, const vk::SwapchainCreateInfoKHR& info,
                                         vk::SwapchainKHR* swapchain);
void                     DestroySwapchain(GraphicContext& graphics, vk::SwapchainKHR swapchain);
[[nodiscard]] vk::Result GetSwapchainImages(vk::Device device, vk::SwapchainKHR swapchain, uint32_t* count, vk::Image* images);
[[nodiscard]] vk::Result AcquireNextImage(vk::Device device, vk::SwapchainKHR swapchain, uint64_t timeout,
                                          vk::Semaphore semaphore, uint32_t* index);
[[nodiscard]] vk::Result QueuePresent(vk::Queue queue, const vk::PresentInfoKHR& present);
// Present thread, recording the frame's present commands (before its submit): FSR's configuration for the frame
// and its prepare pass on `inputs` (null: no inputs, generation off). `region`: the part of the backbuffer the game
// image covers.
void RecordPrepare(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs, vk::Rect2D region);
// Present thread: a frame without inputs while FSR generates (an idle refresh, a menu's first frames) is not
// presented, up to a few in a row; then generation turns off and they are.
[[nodiscard]] bool SkipsFrame(const FrameInputs* inputs);
// GPU thread (an interface draw, hud-anchor.h): the game's 3840x2160 interface canvas, composited over the 3D into its
// output; FSR is told where the HUD is from its alpha, so that generated frames show the HUD of the game's frame
// instead of moving it with the scene.
void NoteUiCanvas(uint64_t address);
// Present thread, in the frame's present commands right after the game image is blitted into `backbuffer`
// (transfer-destination layout, left so): FSR's HUD-less image of the frame, the backbuffer with the HUD darkened
// (FSR then takes the HUD's pixels from the game's frame). Before the system overlay: that counts as HUD too.
// `format`: the backbuffer's (FSR 3 built in, Linux; the Windows backend uses its swapchain's).
void RecordHudless(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs, vk::Image backbuffer,
                   vk::Extent2D extent, vk::Rect2D region, vk::Format format);

// Renderer thread (Kyty.Gpu).
// The guest frame ran the TAA pass: the game's 3D view (play, or a cutscene in the engine), not a movie, a menu or a
// loading screen; with or without frame generation, by the normal or the table way. The guest flip takes it
// (Presenter::PrepareFrame): above 60 Hz the other frames keep 60 (videoOut.cpp).
void NoteScene();
// The guest frame's upscale (the PS5 Pro path) starts a new history: the game's own reset flag, set at a camera cut (a
// dialogue's end, a cutscene's shots). Its flip's inputs carry it (FrameInputs::cut).
void NoteCut();
[[nodiscard]] bool TakeScene();
// The current guest frame ran the TAA pass so far, or the last one flipped did (HudAnchor: the game queues a frame's
// interface before the flip of the one before).
[[nodiscard]] bool SceneNoted();
// The guest flips taken so far (TakeScene).
[[nodiscard]] uint64_t GuestFlips();
// The current guest frame has no camera yet: its next G-buffer draw goes the normal way (the table way's draws have
// no constants on the CPU), whose hook reads it.
[[nodiscard]] bool WantsCamera();
// The current guest frame has DLSS-G's inputs so far (the camera, the TAA pass's or the upscale's depth and motion),
// which its flip takes (PrepareInputs).
[[nodiscard]] bool InputsGathered();
void OnDraw(uint64_t vs_hash, std::span<const uint32_t> flattened_srt);
void OnDispatch(uint64_t cs_hash, ImageId depth, uint64_t depth_address, ImageId motion,
                uint64_t motion_address);
// The game's super resolution dispatch in place of its TAA (renderSuperResolution.cpp): the same depth and motion.
void OnUpscale(ImageId depth, uint64_t depth_address, ImageId motion, uint64_t motion_address);
// Renderer thread, at a TAA dispatch (KYTY_LATE_DLSS): its depth and motion vectors of `extent`; the largest kept.
void OnLateDispatch(ImageId depth, uint64_t depth_address, ImageId motion, uint64_t motion_address, vk::Extent2D extent);
// At the guest flip, in the frame's command buffer: copy the gathered images into `inputs`.
void PrepareInputs(GraphicContext& graphics, TextureCache& cache, CommandBuffer& command,
                   FrameInputs& inputs);
void DestroyInputs(GraphicContext& graphics, FrameInputs& inputs);

// Frame generation's own images (not the texture cache's; frame-gen-hudless.cpp's too), on the thread that records
// them: `image` (re)made as a sampled copy target like `source`, with its view (with `attachment`, a color attachment
// too where the format allows it, *attachment tells; of `size` when given: a blit target; with `failed`, an image that
// cannot be made sets it instead of exiting); a barrier from `image`'s state to `layout`, `access` at `stage`.
void Configure(GraphicContext& graphics, VulkanImage& image, vk::ImageView& view, const VulkanImage& source,
               bool* attachment = nullptr, vk::Extent2D size = {}, bool* failed = nullptr);
void Transition(vk::CommandBuffer command, VulkanImage& image, vk::ImageAspectFlags aspect, vk::ImageLayout layout,
                vk::AccessFlags2 access, vk::PipelineStageFlags2 stage);

// DLSS super resolution (KYTY_UPSCALER=dlss) in place of Sony's PS5 Pro upscaler the game dispatches
// (graphics/guest_gpu/superResolution.h, renderer/renderSuperResolution.cpp).
struct UpscaleImage {
	vk::Image           image  = nullptr;
	vk::ImageView       view   = nullptr;
	vk::ImageLayout     layout = vk::ImageLayout::eUndefined; // as the command buffer leaves it for the upscaler
	vk::Format          format = vk::Format::eUndefined;
	vk::Extent2D        extent {};
	vk::ImageUsageFlags usage {};
	uint32_t            base_level = 0, base_layer = 0; // the subresource the view shows
	vk::Format          image_format = vk::Format::eUndefined; // the image's own when it differs (a depth image's
	                                                            // D32S8 under an R32 view): for upscalers making views
};
struct UpscaleInputs {
	UpscaleImage color, depth, motion, output; // color, depth and motion at the render size
	float        jitter_x = 0.0f, jitter_y = 0.0f;       // the projection's offset, in render pixels
	float        motion_scale_x = 0.0f, motion_scale_y = 0.0f; // motion vector * scale = render pixels to the previous
	bool         reset = false;                           // no history (a cut, a level load)
	std::array<float, 16> view_to_clip {};                // row vectors (p' = p * M)
	float        near_plane = 0.1f, far_plane = 3000.0f;
};
// Streamline offers DLSS super resolution on this device.
[[nodiscard]] bool UpscalerReady();
// Image quality (KYTY_DLSS, KYTY_NIS), Streamline features next to DLSS-G (they need KYTY_FRAMEGEN):
//   KYTY_DLSS=1        DLSS in place of the game's TAA pass, at the same size (DLAA): the TAA's color, depth and motion
//                      in, its output image out. The rest of the frame (post-processing at render size) is unchanged.
//   KYTY_NIS=<height>  NVIDIA Image Scaling of the finished picture to <height> lines (16:9, 720..2160) before the
//                      game's final composite stretches it to its 4K output (KYTY_NIS_SHARPNESS, 0..1, default 0.3).
// The final composite: the pixel shader that lays the interface over the render-size picture, and that picture's texture
// slot (the game profile's composite_shader and composite_scene_slot).
// HDR runs (--hdr, the HDR launcher): the game's final composite there is another pixel shader (NIS never ran in HDR,
// 10-22). Its scene slot when NIS takes the other composites too: 0x3359df0b33704924 (frame-gen-hudless.cpp's composite of
// a smaller scene, the scene after the HUD layer) and KYTY_NIS_SHADERS=<hash>:<slot>,... (hex hashes); -1: not one.
[[nodiscard]] int NisHdrCompositeSlot(uint64_t pixel_shader);
// The final composite's scene slot that NIS replaces (FinalCompositePixelShader; the HDR composites in HDR runs), or -1.
[[nodiscard]] inline int NisCompositeSlot(uint64_t pixel_shader) {
	const auto& profile = ::GameProfile::Current();
	return profile.composite_shader != 0 && pixel_shader == profile.composite_shader
	           ? static_cast<int>(profile.composite_scene_slot)
	           : NisHdrCompositeSlot(pixel_shader);
}
// The picture DLAA makes (the full-size TAA pass's size; {0, 0} before one), for the HDR runs' composite search log.
[[nodiscard]] vk::Extent2D NisSceneExtent();
// The TAA dispatch's images (KYTY_SHADER_DUMP / frame capture): color, depth, motion in, the output.
inline uint32_t TaaColorSlot() { return ::GameProfile::Current().taa_color; }
inline uint32_t TaaDepthSlot() { return ::GameProfile::Current().taa_depth; }
inline uint32_t TaaMotionSlot() { return ::GameProfile::Current().taa_motion; }
inline uint32_t TaaOutputSlot() { return ::GameProfile::Current().taa_output; }
// The largest texture slot the TAA pass's images use (the dispatch needs that many bound images).
inline uint32_t TaaSlotCount() {
	const auto& p = ::GameProfile::Current();
	return std::max({p.taa_color, p.taa_depth, p.taa_motion, p.taa_output}) + 1u;
}
struct SrImage {
	vk::Image           image  = nullptr;
	vk::ImageView       view   = nullptr;
	vk::Format          format = vk::Format::eUndefined;
	vk::Extent2D        extent {};
	vk::ImageLayout     layout = vk::ImageLayout::eUndefined;
	vk::ImageUsageFlags usage {};
};
// The TAA's exposure: the game's TAA multiplies its color input by the first float of this range (eye adaptation,
// written on the GPU by the pass before it); DLSS's result gets the same factor (null buffer: none).
struct TaaExposure {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;     // aligned, as bound
	vk::DeviceSize range  = 0;
	uint32_t       index  = 0;     // the float's index in the bound range
};
[[nodiscard]] bool DlssEnabled();
[[nodiscard]] bool NisEnabled();
// NIS set up (KYTY_NIS), on or off (Shift+F6): the final composite must take the path with NIS's hook (table-xpr.inc).
[[nodiscard]] bool NisAvailable();
// Renderer thread, at the TAA dispatch once its bindings are committed (outside rendering): DLSS writes `output`
// from the inputs. False: nothing was recorded, the game's TAA runs.
[[nodiscard]] bool EvaluateDlss(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& color,
                                const SrImage& depth, const SrImage& motion, const SrImage& output,
                                const TaaExposure& exposure);
// KYTY_LATE_DLSS with KYTY_LATE_DLSS_AA=1, late DLSS on (Shift+F6): the game's TAA is not run; DLSS after the
// post-processing does the anti-aliasing and the upscale both, from the jittered picture.
[[nodiscard]] bool LateAaEnabled();
// Renderer thread, at the full-size TAA dispatch (LateAaEnabled): its color input copied into its output, the TAA's
// exposure applied (as after DLAA). False: nothing recorded, the game's TAA runs.
[[nodiscard]] bool PassThroughTaa(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& color,
                                  const SrImage& output, const TaaExposure& exposure);
// Renderer thread, before the final composite's bindings are committed (outside rendering, `scene` shader-readable):
// `scene` scaled by NIS into an image of its own; the view to bind in its place (shader-read-only), or null.
// KYTY_LATE_DLSS (`cache` given): DLSS super resolution of `scene` in NIS's place (frame-gen.cpp EvaluateLateDlss).
[[nodiscard]] vk::ImageView EvaluateNis(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& scene,
                                        TextureCache* cache = nullptr);
// In-game switches (window.cpp: F6 DLAA, Shift+F6 NIS, Ctrl+F6 / Ctrl+Shift+F6 NIS sharper / softer, F4 the 30 fps
// cap, F12 the FPS panel; F1-F3, F5, F10, F11 are the cheats', F7 the mouse camera), remembered in KYTY_TOGGLE_FILE
// (dlss=, nis=, cap=, sharp= (0..100), hud= lines; without the file: DLAA and NIS on, the cap from KYTY_FLIP_RATE). A short note on screen says the new state.
enum class Switch { Dlss, Nis, Cap, NisSharper, NisSofter, Hud };
void Toggle(Switch which);
// The 30 fps cap now: 0 (none) or KYTY_FLIP_RATE's 1..2 (videoOut.cpp, at every flip).
[[nodiscard]] int FlipRateFloor();
// The note of the last switch for a few seconds (empty after).
[[nodiscard]] std::string HudNotice();
// A line shown as HudNotice for 2.5 s (the F11 cheat switch, loader/cheatHotkey.h).
void ShowNotice(std::string note);
// The KYTY_FPS_HUD panel shown (F12; `fallback`: KYTY_FPS_HUD until the switch is used or remembered).
[[nodiscard]] bool HudShown(bool fallback);
// Records DLSS into `command` (outside a render pass; the images in their `layout`s, left so): false when it could not.
// `dispatch` records the commands DLSS needs around it (the recording worker's when it replays).
[[nodiscard]] bool Upscale(vk::CommandBuffer command, const UpscaleInputs& inputs,
                           const vk::detail::DispatchLoaderDynamic& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER);

// Linux (NVIDIA NGX, kyty-dlss-bridge.h, or FSR 3 built in, fsr-native.h): Streamline makes and shows the generated
// frames inside vkQueuePresentKHR on Windows; here the presenter shows them itself (swapchain.cpp). True once DLSS-G or
// FSR 3 is ready that way.
[[nodiscard]] bool PresenterGenerates();
// Present thread (PresenterGenerates): `backbuffer` is the frame finished at the swapchain's size, its UI included
// (general layout), `region` where the game image is in it. Records into `command` the frames between the last frame
// given and this one, into `outputs` (general layout, storage; in order), and returns how many to show before this
// frame: none for a frame without inputs (a menu, a movie), or the first one after such frames (no frame to start from).
// `hdr`: the backbuffer is HDR10 (PQ; FSR 3 built in only, PresenterGeneratesHdr).
[[nodiscard]] uint32_t Generate(GraphicContext& graphics, vk::CommandBuffer command, const FrameInputs* inputs,
                                const UpscaleImage& backbuffer, vk::Rect2D region, std::span<const UpscaleImage> outputs,
                                bool hdr = false);
// Whether the presenter's generation takes HDR10 frames too (FSR 3 built in; NGX's DLSS-G here only SDR ones).
[[nodiscard]] bool PresenterGeneratesHdr();
// Present thread: the frames the last rendered frame was shown as (1 + the generated ones), for PresentedFrames.
void NotePresented(uint32_t frames);
// KYTY_LIVE_FILE `taaprobe` (debugging): the next full-size TAA pass runs as the game's, then DLSS on the same inputs;
// pixels of its color input, the game's result and DLSS's result are logged (TAAPROBE lines) a few frames later.
[[nodiscard]] bool TaaProbeWanted(vk::Extent2D extent);
void TaaProbe(GraphicContext& graphics, vk::CommandBuffer command, const SrImage& color, const SrImage& depth,
              const SrImage& motion, const SrImage& output, const TaaExposure& exposure);
// KYTY_LIVE_FILE commands, while the game runs: `dlss on|off`, `nis on|off`, `dlssexp auto|off`, `dlsshdr on|off`, `dlssalpha on|off`, `taaprobe`, `dlssexposure on|off`, `taaalpha keep|<value>`,
// `nishdr none|linear|pq`, `nissharp <0..1>`. False: not one of these, or a bad value.
bool LiveCommand(std::string_view command, std::string_view value);
// For the KYTY_FPS_HUD panel: "DLAA on  |  NIS on" (empty without KYTY_DLSS / KYTY_NIS).
[[nodiscard]] const char* QualityLabel();

} // namespace FrameGen
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_LOCAL_FRAME_GEN_H_
