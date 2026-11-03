# How the port is put together

## Layout

```
third_party/iortcw/  the engine and the game code, from iortcw (upstream 438e7d4)
  SP/ MP/            the single player and the multiplayer game: separate trees, as upstream
source/
  platform/          the Switch: files, time, logs, crash reports, input, audio, game modules
  renderer/          the native renderer: the OpenGL layer and the deko3D device
  game/              the weapon wheel, part of both games' cgame
shaders/             the renderer's GLSL, compiled into the NROs with uam
cmake/               source lists, game modules, shaders
tools/               on the PC: SD card, checks of the repository and the NROs, the host check
SD_CARD/             test build, ready to copy to the card (not in git)
SD_LOG/              logs copied back from the card (not in git)
```

Changes to upstream's files are small and marked `__SWITCH__`. The rest of the port is in `source/`, and is
compiled once for each game, against that game's headers.

## Two games, two NROs

iortcw's two games are separate programs, with different engines, game code and renderers, each with 75 to
110 MB of static data. Linked into one executable, a player of one would pay for the other's. So there are two NROs,
and the games' own menu entries start each other with `envSetNextLoad` (`Sys_StartProcess`). Only the
single player one is in the folder the homebrew menu lists.

## The game modules

Homebrew cannot load a library, but cgame, qagame and ui are written to be one:

- They repeat each other's global names. Each module's objects are linked into one relocatable object, and every
  symbol but its entry points is made local (`cmake/RtcwModule.cmake`).
- Each map or menu change unloads and loads them, which on a PC starts them with fresh globals, and their code
  counts on it. A load here does the same: `.data` comes back from a copy made before the module first ran, and `.bss`
  is cleared (`source/platform/nx_modules.c`).
- A client tells pure servers which pk3 its cgame and ui come from. The modules are built in, but
  `mp_bin.pk3` is there, and `VM_Create` touches what a PC would load from it, which gives the same answer.

## 64 bits and ARM

iortcw already runs on 64-bit systems. What is left for this CPU:

- `char` is unsigned on arm64, and id's parsers, tables and network code assume it is signed: everything is built with
  `-fsigned-char`.
- Unaligned reads are fine on the console's normal memory, which the engine's own structures use. The memory that the GPU
  sees is mapped uncached, and only gets plain copies.

## The renderer

The renderer's own files are iortcw's OpenGL 1 renderer. It does its work on the CPU (surfaces, shader stages, texture
coordinates, lighting, sorting) and asks OpenGL to draw arrays with some state set. Those calls end in two layers:

| File | What it does |
| --- | --- |
| `source/renderer/dkgl.c` | the part of OpenGL the renderer uses: state, matrices, arrays, immediate mode, texture objects |
| `source/renderer/dk_device.c` | deko3D: device, memory, swapchain, state objects, uploads, frame pacing |
| `source/renderer/glimp_dk.c` | what the renderer asks of its system |
| `tools/hostcheck/fake_deko3d.c` | on a PC, deko3D for `dk_device.c` to run on, over a software rasterizer |

There is no OpenGL driver and no Mesa in the build, and each renderer stays the code it is on PC, with its fixes.

### Shaders

`shaders/rtcw.vert` does what is left of the fixed function vertex stage: the transform, the color, the fog factor and
the portal's clip plane. `shaders/rtcw.frag` is built once per texture environment the renderer uses, with and
without the clip plane (`cmake/RtcwShaders.cmake`). Alpha testing is the GPU's own, so that no shader has to discard.

### Vertices

The renderer rewrites its arrays between draws, so they are copied to the frame's memory. Compiled vertex arrays are
what saves the most: a shader's stages share their positions and indices, and draw with their own colors and
coordinates.

### Conventions

Clip space has y up and depth -1 to 1, as on OpenGL, so the matrices and winding work unchanged. The screen's first row is
its top, which differs from OpenGL in viewports, scissors and read backs, all converted in `dk_device.c`.

### Gamma

On a PC the brightness setting is the video card's gamma ramp. Here a frame is drawn to an image of its own, and showing
it is one more pass through the game's gamma table (`shaders/present.frag`). So the setting works as it does there, right
away and without touching what blends inside the frame. The default is the 1.3 of the game's own configs.

### Flares

The renderer asks for the depth under each flare with `glReadPixels`, which would stop the GPU each time. Instead, each flare
draws an invisible point, the GPU counts whether it passed the depth test, and the answer comes back some frames later
(`DKGL_FlareVisible`, `DEV_DepthProbe`). Flares fade in and out, so the delay does not show.

### What deko3D 0.5.0 needs

Learned by the ports that run deko3D on hardware, and followed here:

- Its release library ends the process silently on an error. Test builds link the debug library
  (`RTCW_DEKO3D_DEBUG`), whose callback writes the error to the log.
- Images need `FixTileSize`.
- A command can write past what it reserved, so each piece of command memory keeps a gap.
- Clears respect the scissor, as OpenGL's do.
- Coarse depth culling (ZCULL) stays off until tested on the console.

### The fallback

`-DRTCW_RENDERER_BACKEND=opengl` builds the same game on OpenGL through Mesa (`source/platform/glimp_gl.c`), and GitHub Actions
keeps it as an artifact. It tells a problem of the platform from a problem of the native renderer.

## Threads and cores

| Core | Runs |
| --- | --- |
| 0 | the game: its own thread, with an 8 MB stack |
| 2 | sound |
| 3 | the system |

Core 1 is free, for the renderer's back end if measurements ask for it: see [status.md](status.md).

## Memory

From what the host check measured on all the maps:

| What | Size |
| --- | --- |
| Hunk (`com_hunkMegs`) | 128 MB, of which the campaign's maps use 39 to 64 |
| Zone (`com_zoneMegs`) | 32 MB |
| Sound (`com_soundMegs`) | 16 "megs", about 50 MB |
| Static data | 77 MB single player, 113 MB multiplayer |
| Textures, at full size | 70 to 103 MB in the heaviest maps, RGBA8 |
| Frame's vertices | 8 MB a frame, two frames; the campaign's maps stay under 1 MB |

That is about 600 MB, on a console that gives a title override over 3 GB. From the album a process gets about 400 MB, and the game
starts itself with less of each (`source/platform/nx_main.c`).

## Audio

`source/platform/snd_nx.c` is the game's own mixer on the system's `audout` service. It mixes at 24 kHz, about what it does
by default on a PC, and its thread doubles the samples to the 48 kHz that `audout` plays.

## Why not SDL2 or OpenAL

The first plan had SDL2 for input and audio. But the Switch's SDL2 links its video driver, and with it all of Mesa, for
what libnx does in a few hundred lines: Mesa alone, in the fallback build, takes the NRO from 3.4 MB to 8.6. OpenAL Soft on
the Switch goes through SDL2 too. So the port uses libnx directly, as PreyNX does.

## Input

`source/platform/in_nx.c`. While playing, the buttons are the keys `K_JOY1` to `K_JOY16`, so all can be rebound in the game's
own Controls menu, and the sticks are joystick axes with a round dead zone. In menus the sticks move the pointer, the touch
screen points and clicks, and the buttons act as a keyboard would. The layout is set once, on a first start or
when `BINDS_VERSION` goes up, and a player's changes stay.

The weapon wheel is in `source/game/cg_wheel.inc`. The game picks weapons by number keys, one per bank: the wheel shows the
banks the player has something in. A short tap on its button steps to the next weapon instead.

## Files

`Sys_FOpen` gives files a 64 KB buffer. Opening a pk3 walks its central directory, twice, seeking before each of its thousands
of entries, and a seek through stdio throws its buffer away, so `source/platform/nx_zipio.c` keeps its own read-ahead across seeks. A
load runs with the CPU boosted, and play at the usual clocks.
