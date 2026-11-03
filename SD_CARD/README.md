# SD_CARD

The test build, laid out as it goes on the SD card: copy what is in this folder to
the card's root. Building puts the NROs here, and `tools/package_sd.py` adds the
game's files from your own copy. Nothing here but this file goes into the repository.

```
switch/RtCWNX/
  RtCWNX.nro         the game: this is what the homebrew menu lists
  mp/RtCWNX_MP.nro   the multiplayer game, started from the game's menu
  main/              your copy of the game's .pk3 files
```
