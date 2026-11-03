# Host check

Runs the port on a PC, on the game's real files, with no console and no emulator.

`shim/` stands in for libnx, so that `source/platform/` and the engine compile unchanged for the PC, and `host_device.c` stands in
for `source/renderer/dk_device.c`, with a software rasterizer. The game plays a scenario instead of a player, on a virtual clock: a
frame is one of a 60 Hz console, however fast the PC is, so a run gives the same frames every time. What it draws goes to image files,
what it plays to a raw audio file, and the log to `SD_LOG` in the game's folder, as on the console.

It needs a clone of [iortcw](https://github.com/iortcw/iortcw) for the zlib and libjpeg the console gets from devkitPro,
devkitPro's `GL/` headers, and a GCC whose `long` has 64 bits, like the console's: MSYS2's `gcc` package, or any Linux.

```bash
python tools/package_sd.py "/path/to/your/copy/of/Return to Castle Wolfenstein"
mkdir -p /some/scratch/folder/main
cp SD_CARD/switch/RtCWNX/main/*.pk3 /some/scratch/folder/main/
python tools/hostcheck/build.py SP --game-dir /some/scratch/folder --iortcw /path/to/iortcw \
	--gl-include "$DEVKITPRO/portlibs/switch/include"
mkdir out
build_host/rtcw_sp_host tools/hostcheck/scenarios/sp_smoke.txt out
```

Use a copy of the game's folder: the game writes its settings and saves there.

| Scenario | What it does |
| --- | --- |
| `sp_smoke.txt` | The title, a new game, walking, leaning, the notebook, a save and a load |
| `sp_wheel.txt` | The weapon wheel, and the buttons around it |
| `sp_all_maps.txt` | Every map of the campaign, with what each asks of the console in the log |
| `sp_textures.txt` | The heaviest maps' texture memory, at the quality it is started with |
| `mp_smoke.txt` | The multiplayer game (`build.py MP`): its menus, a local server on two maps, limbo and a spawn |

Only the frames a scenario asks a picture of are rasterized, so the others run at full speed. It shows that what the renderer asks makes the right
picture, and how much it asks for. It cannot show that deko3D does what `dk_device.c` tells it: that takes a console.

The format of a scenario is at the top of `shim/switch_shim.c`.
