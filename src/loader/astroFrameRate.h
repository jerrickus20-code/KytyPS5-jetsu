#ifndef KYTY_LOADER_ASTRO_FRAME_RATE_H_
#define KYTY_LOADER_ASTRO_FRAME_RATE_H_

namespace Loader {

struct Program;

// Astro's Playroom (PPSA01325 01.905.000) moves its game by its engine's frame time (1/60 s as shipped) every frame: with
// the emulator's 30 fps cap (F4) it ran in slow motion. While the cap is on, the engine's frame rate is set to 30 (the
// same three values its own frame-rate change writes: 30.0 fps, 33333 us, 1/30 s), so the game moves at its normal
// speed; when the cap is off, back to 60.
namespace AstroFrameRate {

void Install(Program* main_program);
bool Installed();
// Called at every guest frame: fps 30 (cap on) or 60 (cap off). Only writes when the value differs from the game's.
void Apply(int fps);

} // namespace AstroFrameRate
} // namespace Loader

#endif // KYTY_LOADER_ASTRO_FRAME_RATE_H_
