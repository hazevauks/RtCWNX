# Testing

## On the console

1. Build, then fill `SD_CARD/`:

   ```bash
   ./build.sh
   python tools/package_sd.py "/path/to/your/copy/of/Return to Castle Wolfenstein"
   ```

2. Copy what is inside `SD_CARD/` to the card once. After that only the NROs change.
3. Copy `sd:/switch/RtCWNX/SD_LOG/` back to `SD_LOG/` after each run.

For faster rounds, without taking the card out, start the homebrew menu's NetLoader first (Y):

```bash
nxlink -s -a <console address> build/RtCWNX.nro
```

That prints the log in the terminal as it runs. Started this way, starting the other game looks for it on the card.

## A picture of what the console shows

Open the console (+ and − together), press A for the keyboard and enter `screenshotJPEG`. The picture goes to
`sd:/switch/RtCWNX/main/screenshots/`.

## If the native renderer does not start

The `RtCWNX_fallback_opengl` artifact of each GitHub Actions run is the same game on OpenGL through Mesa. Locally:

```bash
BUILD_DIR=build-gl ./build.sh -DRTCW_RENDERER_BACKEND=opengl
```

If that one works and the native one does not, the trouble is in `source/renderer/dk_device.c` or the shaders. If neither works, it is
in the platform layer.

## Reading a crash

`rtcw_crash.txt` gives addresses in the executable as offsets:

```bash
aarch64-none-elf-addr2line -f -e build/RtCWNX.elf <offset> <offset>
```

The `.elf` has to be from the same build: each GitHub Actions run keeps them as the `RtCWNX_elf` artifact. The build keeps frame pointers, so the
call stack is reliable.

## Measuring

Through `sd:/switch/RtCWNX/args.txt`, or the console (+ and − together):

| Cvar | Use |
| --- | --- |
| `r_dkPerf 1` | fps, draws, vertices and images in the log, once a second |
| `cg_drawFPS 1` | fps on screen |
| `com_speeds 1` | game, front end and back end time, on the console |
| `r_speeds 1` | what the renderer draws, on the console |
| `r_dynamiclight 0`, `r_flares 0` | what each costs |

## What to check before calling a stage done

| Stage | Check |
| --- | --- |
| Boot | the log reaches `finished R_Init`, and the id Software video plays |
| Data | the log lists all the `.pk3` files, and no `Couldn't load` at the main menu |
| Controls | the pointer moves with the sticks, the D-pad and the touch screen; A chooses |
| Level | **Play** reaches the briefing, A starts the level |
| Mechanics | L and R lean, the wheel opens on D-pad right, the notebook opens on − |
| Sound | music and footsteps, in the right ears, with no crackle |
| Save | a save and a load, and sleep and wake in between |
| Multiplayer | **Multiplayer** starts the other game; a local server; a PC server |
| Memory | no `Out of memory` after several map changes; the log's `perf` lines stay steady |
| Speed | `r_dkPerf`, at the same places each time |
