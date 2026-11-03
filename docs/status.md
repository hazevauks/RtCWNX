# Status

What has been checked, and how. Nothing here counts as working until a console has shown it.

## Stages

| Stage | State | Checked by |
| --- | --- | --- |
| Source, asset layout, file system | Done | Host check, on the real game files |
| Build with devkitA64, CMake and libnx | Done | Local build and GitHub Actions |
| POSIX layer, memory, threads | Built and run | Host check; libnx's own services not yet |
| Audio and controller mapping | Built and run | Host check; the hardware not yet |
| Boot on the console, logs | **Waiting for a console** | |
| Native deko3D renderer | Built; its logic run | Host check up to the GPU; `dk_device.c` not yet run |
| Performance and tuning | **Waiting for a console** | Counts from the host check |

## What was checked on a PC

[tools/hostcheck](../tools/hostcheck/README.md) runs the port's own code on the game's real files, with a stand-in for libnx
and a software rasterizer in place of deko3D. With it:

- All 19 pk3 files mount: 6384 files in the single player game. No two names differ only by case.
- The title screen, the id Software video and the menus draw right, with text, blending and fades.
- All 27 maps of the campaign that were tried load, start and shut down with no error: fog, skies, stained glass,
  letterboxed cutscenes, characters, weapons and the HUD all look right.
- The controller's scripted presses walk, turn, lean, fire, crouch, jump, reload, zoom, and open the menu, the notebook
  and the console.
- The weapon wheel opens, follows the stick and picks, and a tap steps to the next weapon.
- Saving and loading work.
- The multiplayer game goes through its menus to a local server on two maps, past the pure check, into limbo
  and a spawn, with its HUD.
- Each module starts fresh on every load, through several map changes.
- On a first start, the layout is set and saved under the buttons' names.

The NROs' name, author and icon are read back from the files on every build: Return to Castle Wolfenstein, hazevauks,
a 256x256 JPEG. All 15 shaders compile with `uam`. No OpenGL, Mesa or SDL2 is in the native build's link.

## What a scene asks for

Counted by the host check at 1280x720, in one frame of each map, on a spot just after the map starts:

| | Lowest | Highest |
| --- | --- | --- |
| Draws | 17 (`crypt1`) | 198 (`assault`), 637 at a multiplayer spawn |
| Indices | 2 301 | 81 945 (`dark`, `end`) |
| Frame's vertices | 32 KB | 808 KB |
| Images | 609 | 797 |
| Texture memory, at full size | | 103 MB (`dam`) |
| Hunk in use | 39 MB (`boss1`) | 64 MB (`swf`) |

These are small numbers for this GPU. If the target is missed, the CPU is the first place to look: the game's logic, AI and
the renderer's work on surfaces all share one 1 GHz core.

## What only a console can check

- That it starts at all: libnx's services, the swapchain, audout, HID and the exit handling are stand-ins on a PC.
- `dk_device.c` and the shaders: the host check shows that what the renderer asks makes the right picture, not that deko3D does
  what it is told. The first test's most likely trouble is here, and the fallback build is there to tell.
- Speed on the Switch's CPU, and how long a load takes on a card.
- Sound on the real service, and its timing.
- The real Joy-Cons, the touch screen, the system keyboard, sleep and wake.
- Online play against PC servers. The client gives pure servers the same answer a PC does, but that has only met its own server.
- Starting one game from the other.
- The album's lower settings.

## Known gaps

| Gap | Note |
| --- | --- |
| Co-op campaign | The game has none, and neither does iortcw. The multiplayer game's team and objective modes are all there |
| Docked resolution | Always 1280x720, scaled by the system |
| Button prompts | Show the buttons' names (`BUTTON_A`, `DPAD_UP`), not icons |
| Renderer's back end on its own thread | Not done: core 1 is free for it, if measurements ask |
| Bloom | Left off: it reads the screen back, which the port does not do |
| Voice chat, downloads through HTTP | Left out of the build |
