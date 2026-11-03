/* nx_paths.h -- where the port keeps its files on the SD card. */

#ifndef NX_PATHS_H
#define NX_PATHS_H

#ifndef NX_GAME_DIR	/* tools/hostcheck points this at a folder on the PC */
#define NX_GAME_DIR	"sdmc:/switch/RtCWNX"
#endif

#define NX_SP_NRO	NX_GAME_DIR "/RtCWNX.nro"
#define NX_MP_NRO	NX_GAME_DIR "/mp/RtCWNX_MP.nro"
#define NX_LOG_DIR	NX_GAME_DIR "/SD_LOG"
#define NX_ARGS_FILE	NX_GAME_DIR "/args.txt"

/* The two games start each other, so each keeps its own logs. */
#ifdef RTCW_MP
#define NX_LOG_NAME	"rtcw_mp"
#else
#define NX_LOG_NAME	"rtcw"
#endif
#define NX_LOG_FILE	NX_LOG_DIR "/" NX_LOG_NAME "_log.txt"
#define NX_LOG_FILE_OLD	NX_LOG_DIR "/" NX_LOG_NAME "_log_previous.txt"
#define NX_CRASH_FILE	NX_LOG_DIR "/" NX_LOG_NAME "_crash.txt"
#define NX_FATAL_FILE	NX_LOG_DIR "/" NX_LOG_NAME "_fatal.txt"

#endif	/* NX_PATHS_H */
