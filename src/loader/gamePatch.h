#ifndef KYTY_LOADER_GAME_PATCH_H_
#define KYTY_LOADER_GAME_PATCH_H_

#include "common/common.h"

#include <filesystem>
#include <vector>

namespace Loader {

struct Program;

namespace GamePatch {

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs);
bool ApplyPending(Program* program);
// Built-in game fixes that need no patch file (KYTY_ASTRO_CB_SCALE). Main program, before it runs.
void ApplyBuiltin(Program* main_program);
// KYTY_LIVE_PATCH=<file>: switch the patch file's mods on/off while the game runs (polled ~2.5 times a second; call it from
// a thread that runs every frame).
void PollLive();
void Clear();

} // namespace GamePatch

} // namespace Loader

#endif // KYTY_LOADER_GAME_PATCH_H_
