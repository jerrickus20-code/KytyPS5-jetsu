Game profiles
=============

A profile tells the emulator what DLSS frame generation, late DLSS and the 720p render size need to know about one game.
One text file per game, named after its title id: PPSA01325.txt (the title id is in sce_sys\param.sfo, and the first line
of _dlss\run.log's "Title ID:" line). The emulator reads profiles\<title id>.txt from the folder it runs in
(KYTY_PROFILE_DIR=<folder> names another). Demon's Souls (PPSA01340, PPSA01341, PPSA01342) is built in.

Writing one for a new game starts with the discovery log:

    run-dlss.cmd -Game <game folder> -Discover

Play to a scene with the 3D view for a minute, quit, and read _dlss\run.log: the PROFILE lines show, once per shader,
  PROFILE compute  - a compute shader and its images (the TAA pass: color in, depth, motion vectors, color out)
  PROFILE vertex   - a vertex shader's first constants (the camera's matrices)
  PROFILE pixel    - a pixel shader drawing into a target much larger than an image it samples (the final composite)
The format of the file is described at the top of src\local\game-profile.h (and docs\DLSS.md, "Game profiles").

Example (Demon's Souls, built in):

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

Render size patches (KYTY_RENDER_HEIGHT, -RenderHeight 720), applied only when every one is found exactly once:
    patch_u32  = <code bytes, ?? for bytes that vary> | <offset from the start of the bytes> | width or height
    patch_pair = <old width> <old height>      (a table entry of two 32-bit numbers)
