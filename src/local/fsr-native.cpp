// This fork has no FSR 3 built from source: the functions of fsr-native.h answer that nothing is made.
#include "fsr-native.h"

namespace Libs::Graphics::FsrNative {

bool UpscalerRequested() {
	return false;
}

bool UpscaleInsteadOfDlss() {
	return false;
}

bool Upscale(GraphicContext& /*graphics*/, vk::CommandBuffer /*command*/, const FrameGen::UpscaleInputs& /*inputs*/) {
	return false;
}

bool InterpolationRequested() {
	return false;
}

bool Interpolate(GraphicContext& /*graphics*/, vk::CommandBuffer /*command*/, const InterpolationInputs& /*inputs*/,
                 const FrameGen::UpscaleImage& /*output*/) {
	return false;
}

} // namespace Libs::Graphics::FsrNative
