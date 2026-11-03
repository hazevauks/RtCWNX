# Status

What has been checked, and how. Only what a console has shown counts as working.

## Stages

| Stage | State | Checked by |
| --- | --- | --- |
| Source, asset layout, file system | Done | Host check, on the real game files |
| Build with devkitA64, CMake and libnx | Done | Local build and GitHub Actions |
| POSIX layer, memory, threads | Built and run | Host check; libnx's own services not yet |
| Audio and controller mapping | Built and run | Host check; the hardware not yet |
| Boot on the console, logs | Done | Both NROs on a console, with no errors in the logs |
| Native deko3D renderer | Runs; **picture being fixed** | Host check, with `dk_device.c`; a second console test is next |
| Performance and tuning | **Waiting for a console** | Counts from the host check |

## The first test on a console

Both NROs started from the homebrew menu, on a title's memory: all pk3 files mounted, deko3D and sound started, the multiplayer
game opened its socket, and the single player game quit cleanly. The logs have no deko3D error, and there was no crash file. The
picture was distorted.

The cause was in `Flush`, in `dk_device.c`. It read what state to set before opening the frame, and opening the frame is what marks
all of it. So a frame opened by its first clear kept the state of the last frame's pass to the screen: the game's geometry went
through the wrong shaders, with the wrong textures, until something changed them.

The host check could not see that, because `dk_device.c` was the part it replaced. It now runs that file on a stand-in for deko3D.
The stand-in showed the same fault, in the same place, and shows none since the fix. A second test on a console is what confirms it.

## What was checked on a PC

[tools/hostcheck](../tools/hostcheck/README.md) runs the port's own code on the game's real files, with stand-ins for libnx and
deko3D. With it:

- All 19 pk3 files mount: 5 in the single player game and 15 in the multiplayer one, which share `pak0.pk3`. No two names
  differ only by case.
- The title screen, the id Software video and the menus draw right, with text, blending and fades.
- All 32 maps of the campaign load, start and shut down with no error: fog, skies, stained glass, letterboxed
  cutscenes, characters, weapons and the HUD all look right.
- The controller's scripted presses walk, turn, lean, fire, crouch, jump, reload, zoom, and open the menu, the notebook
  and the console.
- The weapon wheel opens, follows the stick and picks, and a tap steps to the next weapon.
- Saving and loading work.
- The mixer's output has sound in both channels, at sane levels, for the whole of a 60 second run.
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

## Misaligned memory

A build that traps on any misaligned access stops as soon as the game loads a map's bot data (`AAS_ParseBSPEntities`): botlib's
allocator hands out memory that is not aligned for 8 byte fields. That is fine on the console's normal memory, whose
CPU allows it, and it is upstream's code as it runs on other arm64 systems. A trap build cannot look past its first hit, so this says
nothing of the code after it. On the console, a misaligned access to memory the GPU sees is the case to watch for, and the renderer's copies
into it are plain ones. `rtcw_crash.txt` would show it as a data abort.

## What only a console can check

- What libnx's services do past starting: audout, HID and the exit handling are stand-ins on a PC.
- `dk_device.c` and the shaders on the real GPU: the host check shows that they ask for the right things, in the right order. It does not
  run the shaders, and it does not show what the GPU makes of image layouts or its caches. The fallback build is there to tell.
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
