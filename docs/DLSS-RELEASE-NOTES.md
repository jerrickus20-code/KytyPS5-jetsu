# KytyPS5 U59 + DLSS frame generation (test build)

Jetsku's U59 emulator plus NVIDIA Streamline DLSS frame generation and an F4 30 fps cap, ported from jerrickus20-code/KytyPS5. A test build: see `DLSS-README.md` in the archive for what it needs (an RTX 40-series GPU, NVIDIA's Streamline runtime) and how to start it (`run-dlss.cmd`).

- **Added:** DLSS frame generation, F4 30 fps cap, a DLSS-G line in the F2/F12 performance overlay; Demon's Souls' 720p render size (`-RenderHeight 720`); late DLSS, the finished 720p picture upscaled to 1440p (`run-latedlss-1440.cmd`, Shift+F6 compares).
- **Game profiles:** what frame generation, late DLSS and the render size need to know about a game is now a text file per game (`profiles\<title id>.txt`; Demon's Souls is built in). `run-dlss.cmd -Game <folder> -Discover` writes what a new game's profile needs into `_dlss\run.log` (docs/DLSS.md).
- **Not yet:** the game's own 1440p output mode (the output stays 4K), cheats.
- **Please send:** `_dlss\run.log` and `_kyty.txt` after a test run, with what you saw.
