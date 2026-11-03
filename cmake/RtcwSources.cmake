# The source lists of iortcw's Makefiles, for the parts this port builds.
# Paths are relative to a game's code/ folder (third_party/iortcw/SP/code, or MP).

set(RTCW_CLIENT_COMMON
	client/cl_cgame.c client/cl_cin.c client/cl_console.c client/cl_input.c client/cl_keys.c
	client/cl_main.c client/cl_net_chan.c client/cl_parse.c client/cl_scrn.c client/cl_ui.c client/cl_avi.c
	client/snd_adpcm.c client/snd_dma.c client/snd_mem.c client/snd_mix.c client/snd_wavelet.c
	client/snd_main.c client/snd_codec.c client/snd_codec_wav.c
	qcommon/cm_load.c qcommon/cm_patch.c qcommon/cm_polylib.c qcommon/cm_test.c qcommon/cm_trace.c
	qcommon/cmd.c qcommon/common.c qcommon/cvar.c qcommon/files.c qcommon/md4.c qcommon/md5.c
	qcommon/msg.c qcommon/net_chan.c qcommon/net_ip.c qcommon/huffman.c
	qcommon/q_math.c qcommon/q_shared.c qcommon/puff.c qcommon/vm.c qcommon/vm_interpreted.c qcommon/vm_none.c
	server/sv_bot.c server/sv_ccmds.c server/sv_client.c server/sv_game.c server/sv_init.c
	server/sv_main.c server/sv_net_chan.c server/sv_snapshot.c server/sv_world.c
	zlib-1.2.11/unzip.c zlib-1.2.11/ioapi.c
	sys/con_log.c sys/sys_main.c
)

set(RTCW_BOTLIB
	botlib/be_aas_bspq3.c botlib/be_aas_cluster.c botlib/be_aas_debug.c botlib/be_aas_entity.c
	botlib/be_aas_file.c botlib/be_aas_main.c botlib/be_aas_move.c botlib/be_aas_optimize.c
	botlib/be_aas_reach.c botlib/be_aas_route.c botlib/be_aas_routealt.c botlib/be_aas_routetable.c
	botlib/be_aas_sample.c botlib/be_ai_char.c botlib/be_ai_chat.c botlib/be_ai_gen.c botlib/be_ai_goal.c
	botlib/be_ai_move.c botlib/be_ai_weap.c botlib/be_ai_weight.c botlib/be_ea.c botlib/be_interface.c
	botlib/l_crc.c botlib/l_libvar.c botlib/l_log.c botlib/l_memory.c botlib/l_precomp.c botlib/l_script.c
	botlib/l_struct.c
)

set(RTCW_SPLINES
	splines/math_angles.cpp splines/math_matrix.cpp splines/math_quaternion.cpp splines/math_vector.cpp
	splines/q_parse.cpp splines/splines.cpp splines/util_str.cpp
)

set(RTCW_RENDERER
	renderer/tr_animation.c renderer/tr_backend.c renderer/tr_bsp.c renderer/tr_cmds.c renderer/tr_cmesh.c
	renderer/tr_curve.c renderer/tr_flares.c renderer/tr_font.c renderer/tr_image.c renderer/tr_image_bmp.c
	renderer/tr_image_jpg.c renderer/tr_image_pcx.c renderer/tr_image_png.c renderer/tr_image_tga.c
	renderer/tr_init.c renderer/tr_light.c renderer/tr_main.c renderer/tr_marks.c renderer/tr_mesh.c
	renderer/tr_model.c renderer/tr_model_iqm.c renderer/tr_noise.c renderer/tr_scene.c renderer/tr_shade.c
	renderer/tr_shade_calc.c renderer/tr_shader.c renderer/tr_shadows.c renderer/tr_sky.c renderer/tr_surface.c
	renderer/tr_world.c renderer/tr_bloom.c
)

set(RTCW_CGAME_COMMON
	cgame/cg_main.c game/bg_animation.c game/bg_misc.c game/bg_pmove.c game/bg_slidemove.c game/bg_lib.c
	cgame/cg_consolecmds.c cgame/cg_draw.c cgame/cg_drawtools.c cgame/cg_effects.c cgame/cg_ents.c cgame/cg_event.c
	cgame/cg_flamethrower.c cgame/cg_info.c cgame/cg_localents.c cgame/cg_marks.c cgame/cg_newdraw.c
	cgame/cg_particles.c cgame/cg_players.c cgame/cg_playerstate.c cgame/cg_predict.c cgame/cg_scoreboard.c
	cgame/cg_servercmds.c cgame/cg_snapshot.c cgame/cg_sound.c cgame/cg_trails.c cgame/cg_view.c cgame/cg_weapons.c
	ui/ui_shared.c qcommon/q_math.c qcommon/q_shared.c cgame/cg_syscalls.c
)

set(RTCW_QAGAME_COMMON
	game/g_main.c game/ai_cast.c game/ai_cast_characters.c game/ai_cast_debug.c game/ai_cast_events.c
	game/ai_cast_fight.c game/ai_cast_func_attack.c game/ai_cast_func_boss1.c game/ai_cast_funcs.c
	game/ai_cast_script_actions.c game/ai_cast_script.c game/ai_cast_script_ents.c game/ai_cast_sight.c
	game/ai_cast_think.c game/ai_chat.c game/ai_cmd.c game/ai_dmnet.c game/ai_dmq3.c game/ai_main.c game/ai_team.c
	game/bg_animation.c game/bg_misc.c game/bg_pmove.c game/bg_slidemove.c game/bg_lib.c
	game/g_active.c game/g_alarm.c game/g_bot.c game/g_client.c game/g_cmds.c game/g_combat.c game/g_items.c
	game/g_mem.c game/g_misc.c game/g_missile.c game/g_mover.c game/g_props.c game/g_script_actions.c game/g_script.c
	game/g_session.c game/g_spawn.c game/g_svcmds.c game/g_target.c game/g_team.c game/g_tramcar.c game/g_trigger.c
	game/g_utils.c game/g_weapon.c qcommon/q_math.c qcommon/q_shared.c game/g_syscalls.c
)

set(RTCW_UI_COMMON
	ui/ui_main.c ui/ui_atoms.c ui/ui_gameinfo.c ui/ui_players.c ui/ui_shared.c game/bg_misc.c game/bg_lib.c
	qcommon/q_math.c qcommon/q_shared.c ui/ui_syscalls.c
)

# What differs between the two games
set(RTCW_SP_CGAME ${RTCW_CGAME_COMMON})
set(RTCW_MP_CGAME ${RTCW_CGAME_COMMON} cgame/cg_spawn.c)
set(RTCW_SP_QAGAME ${RTCW_QAGAME_COMMON} game/g_save.c)
set(RTCW_MP_QAGAME ${RTCW_QAGAME_COMMON} game/g_antilag.c)
set(RTCW_SP_UI ${RTCW_UI_COMMON})
set(RTCW_MP_UI ${RTCW_UI_COMMON})
set(RTCW_SP_ENGINE ${RTCW_CLIENT_COMMON})
set(RTCW_MP_ENGINE ${RTCW_CLIENT_COMMON} qcommon/pbmd5.c)
