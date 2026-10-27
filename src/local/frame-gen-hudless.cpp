// The HUD-less capture of frame-gen-hudless.h is not part of this fork: DLSS-G takes the backbuffer with the HUD.
#include "frame-gen-hudless.h"

namespace Libs::Graphics::FrameGen {

bool g_hudless = false;

void InitializeHudless() {}

void DestroyHudlessInputs(GraphicContext& /*graphics*/, FrameInputs& /*inputs*/) {}

} // namespace Libs::Graphics::FrameGen
