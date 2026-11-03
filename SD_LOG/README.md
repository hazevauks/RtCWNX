# SD_LOG

The port writes its logs to `sd:/switch/RtCWNX/SD_LOG/` on the console. Copy them
here to look at them. Nothing here but this file goes into the repository.

| File | What it is |
| --- | --- |
| `rtcw_log.txt` | Everything the game printed during the last run, with stdout and stderr |
| `rtcw_log_previous.txt` | The same, for the run before it |
| `rtcw_crash.txt` | Registers and a backtrace, written when the process faults |
| `rtcw_fatal.txt` | The message of a fatal error |

The multiplayer game writes the same files as `rtcw_mp_*.txt`.

A crash report's offsets map to source lines with the `.elf` of the same build,
which every GitHub Actions run keeps as an artifact:

```bash
aarch64-none-elf-addr2line -f -e build/RtCWNX.elf 0x12345
```
