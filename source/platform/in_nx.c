/* in_nx.c -- Joy-Cons, Pro Controller and touch screen.
 *
 * While playing, the buttons are the keys K_JOY1 to K_JOY16, so all of them can be
 * rebound in the game's own Controls menu, and the sticks are joystick axes. In
 * menus the sticks move the cursor, the touch screen points and clicks, and the
 * buttons act as a keyboard would. The console opens with + and - together.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "client/client.h"
#include "sys/sys_local.h"

#include "nx_platform.h"

#define CLAMP_TO(v, lo, hi)	((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

#define STICK_MAX		32767.0f
#define SCREEN_W		640.0f		/* the UI's virtual screen */
#define SCREEN_H		480.0f
#define PREGAME_ARROW_X		600	/* ui/pregame.menu's button, on that screen */
#define PREGAME_ARROW_Y		450

/* The version of the default layout: raising it sets the bindings anew. */
#define BINDS_VERSION		1

typedef enum
{
	MODE_GAME,
	MODE_MENU,
	MODE_CONSOLE,
	MODE_CHAT
} inputMode_t;

/* pseudo keys, handled here instead of being posted to the engine */
#define KEY_NONE		0
#define KEY_SELECT		-1	/* A in a menu: click at the cursor, or press the focused item */
#define KEY_TEXT		-2	/* the system keyboard */

typedef struct
{
	u64	button;
	int	game;
	int	menu;
	int	console;
} buttonMap_t;

/* JOY1 to JOY16: their names are in cl_keys.c */
static const buttonMap_t button_map[] =
{
	{ HidNpadButton_A,	K_JOY1,		KEY_SELECT,	KEY_TEXT },
	{ HidNpadButton_B,	K_JOY2,		K_ESCAPE,	K_CONSOLE },
	{ HidNpadButton_X,	K_JOY3,		KEY_TEXT,	KEY_NONE },
	{ HidNpadButton_Y,	K_JOY4,		K_BACKSPACE,	K_BACKSPACE },
	{ HidNpadButton_L,	K_JOY5,		K_MWHEELUP,	K_PGUP },
	{ HidNpadButton_R,	K_JOY6,		K_MWHEELDOWN,	K_PGDN },
	{ HidNpadButton_ZL,	K_JOY7,		K_MOUSE2,	KEY_NONE },
	{ HidNpadButton_ZR,	K_JOY8,		K_MOUSE1,	KEY_NONE },
	{ HidNpadButton_Plus,	K_ESCAPE,	K_ESCAPE,	K_CONSOLE },
	{ HidNpadButton_Minus,	K_JOY10,	KEY_NONE,	KEY_NONE },
	{ HidNpadButton_StickL,	K_JOY11,	KEY_NONE,	KEY_NONE },
	{ HidNpadButton_StickR,	K_JOY12,	KEY_NONE,	KEY_NONE },
	{ HidNpadButton_Up,	K_JOY13,	K_UPARROW,	K_UPARROW },
	{ HidNpadButton_Down,	K_JOY14,	K_DOWNARROW,	K_DOWNARROW },
	{ HidNpadButton_Left,	K_JOY15,	K_LEFTARROW,	K_LEFTARROW },
	{ HidNpadButton_Right,	K_JOY16,	K_RIGHTARROW,	K_RIGHTARROW },
};
#define NUM_BUTTONS	ARRAY_LEN(button_map)

typedef struct
{
	int		key;
	const char	*command;
} defaultBind_t;

/* The game's actions on the controller. A player's own changes to a binding stay. */
static const defaultBind_t default_binds[] =
{
	{ K_JOY8,	"+attack" },		/* ZR: fire */
	{ K_JOY7,	"+zoom" },		/* ZL: binoculars */
	{ K_JOY5,	"+leanleft" },		/* L: lean left */
	{ K_JOY6,	"+leanright" },		/* R: lean right */
	{ K_JOY1,	"+moveup" },		/* A: jump */
	{ K_JOY2,	"+movedown" },		/* B: crouch */
	{ K_JOY3,	"+reload" },		/* X: reload */
	{ K_JOY4,	"+activate" },		/* Y: use, open, pick up */
	{ K_JOY11,	"+sprint" },		/* left stick click: sprint */
	{ K_JOY16,	"weapnext" },		/* D-pad right: next weapon */
	{ K_JOY15,	"weapprev" },		/* D-pad left: previous weapon */
	{ K_JOY13,	"weapalt" },		/* D-pad up: scope, silencer, the weapon's other mode */
#ifdef RTCW_MP
	{ K_JOY12,	"+dropweapon" },	/* right stick click: drop the weapon */
	{ K_JOY14,	"mp_QuickMessage" },	/* D-pad down: quick chat */
	{ K_JOY10,	"+scores" },		/* -: scores */
#else
	{ K_JOY12,	"+kick" },		/* right stick click: kick */
	{ K_JOY14,	"+quickgren" },		/* D-pad down: throw a grenade */
	{ K_JOY10,	"notebook" },		/* -: notebook, objectives */
#endif
};

typedef struct
{
	const char	*name;
	const char	*value;
} defaultCvar_t;

/* The sticks give full travel at +-32767. Forward and side reach the usercmd's 127
 * there, so that the walk is analog; look is in degrees per second per unit. */
static const defaultCvar_t default_cvars[] =
{
	{ "j_forward",	"-0.003876" },
	{ "j_side",	"0.003876" },
	{ "j_yaw",	"-0.0075" },
	{ "j_pitch",	"0.0055" },
	{ "in_nxDeadZone", "0.12" },
	{ "cl_freelook", "1" },
};

static cvar_t	*in_nxDeadZone;
static cvar_t	*in_nxInvertLook;
static cvar_t	*in_nxBindsVersion;
static cvar_t	*sensitivity_cvar;

static bool	input_ready;
static PadState	pad;
static u64	held_last;
static int	pressed_as[NUM_BUTTONS];
static inputMode_t mode = MODE_MENU;
static int	axes_last[4];
static bool	console_combo;
static bool	touch_down;
static bool	cursor_by_pad;	/* the last thing to move the menu's focus was the D-pad */
static float	cursor_x, cursor_y;
static float	cursor_rest_x, cursor_rest_y;

static void PostKey (int key, bool down)
{
	if (key > 0)
		Com_QueueEvent(Sys_Milliseconds(), SE_KEY, key, down, 0, NULL);
}

static void TapKey (int key)
{
	PostKey(key, true);
	PostKey(key, false);
}

/*
===============================================================================

SYSTEM KEYBOARD

===============================================================================
*/

static bool ShowKeyboard (const char *header, char *text, size_t text_size)
{
	SwkbdConfig	kbd;
	Result		rc;

	text[0] = 0;
	if (R_FAILED(swkbdCreate(&kbd, 0)))
		return false;
	swkbdConfigMakePresetDefault(&kbd);
	swkbdConfigSetHeaderText(&kbd, header);
	rc = swkbdShow(&kbd, text, text_size);
	swkbdClose(&kbd);
	return R_SUCCEEDED(rc) && text[0] != 0;
}

static void TypeText (const char *text)
{
	const char	*c;

	for (c = text; *c; c++)
	{
		if (*c >= 32 && *c < 127)
			Com_QueueEvent(Sys_Milliseconds(), SE_CHAR, *c, 0, 0, NULL);
	}
}

static void KeyboardForMode (void)
{
	char	text[256];

	if (ShowKeyboard(mode == MODE_CONSOLE ? "Console" : (mode == MODE_CHAT ? "Message" : "Return to Castle Wolfenstein"),
			text, sizeof(text)))
	{
		TypeText(text);
		if (mode != MODE_MENU)
			TapKey(K_ENTER);
	}
	else if (mode == MODE_CHAT)
	{
		TapKey(K_ESCAPE);
	}
}

/*
===============================================================================

BUTTONS

===============================================================================
*/

static void ReleaseButtons (void)
{
	size_t	i;

	for (i = 0; i < NUM_BUTTONS; i++)
	{
		PostKey(pressed_as[i], false);
		pressed_as[i] = KEY_NONE;
	}
}

static int KeyForMode (const buttonMap_t *map)
{
	switch (mode)
	{
	case MODE_MENU:		return map->menu;
	case MODE_CONSOLE:	return map->console;
	case MODE_CHAT:		return KEY_NONE;
	default:		return map->game;
	}
}

/* The UI only takes relative moves, and clamps them to the screen: a move large enough
 * finds a corner to start from. Directly, because the event queue would add them up. */
static void CursorTo (int x, int y)
{
	const int	now = Sys_Milliseconds();

	CL_MouseEvent(-(int) SCREEN_W * 2, -(int) SCREEN_H * 2, now);
	CL_MouseEvent(x, y, now);
	cursor_x = x;
	cursor_y = y;
}

/* After a level loads, the single player game waits on a screen whose one way out is
 * an arrow in a corner, for the mouse. The buttons that say "go on" press it. */
static bool ClickPregameArrow (void)
{
#ifndef RTCW_MP
	if (uivm && VM_Call(uivm, UI_GET_ACTIVE_MENU) == UIMENU_PREGAME)
	{
		CursorTo(PREGAME_ARROW_X, PREGAME_ARROW_Y);
		/* entering the arrow shows the button that takes the click, and moving on it finds it */
		CL_MouseEvent(1, 1, Sys_Milliseconds());
		TapKey(K_MOUSE1);
		return true;
	}
#endif
	return false;
}

static void PressButton (size_t i)
{
	int	key = KeyForMode(&button_map[i]);

	if (key == KEY_TEXT)
	{
		KeyboardForMode();
		return;
	}
	if ((key == KEY_SELECT || key == K_MOUSE1 || key == K_ESCAPE) && mode == MODE_MENU && ClickPregameArrow())
		return;
	if (key == KEY_SELECT)
		key = cursor_by_pad ? K_ENTER : K_MOUSE1;
	if (key == K_UPARROW || key == K_DOWNARROW || key == K_LEFTARROW || key == K_RIGHTARROW)
		cursor_by_pad = true;

	PostKey(key, true);
	pressed_as[i] = key;
}

static void UpdateButtons (u64 held)
{
	const u64	combo = HidNpadButton_Plus | HidNpadButton_Minus;
	const u64	changed = held ^ held_last;
	size_t		i;

	/* + and - together open the console from anywhere */
	if ((held & combo) == combo)
	{
		if (!console_combo)
		{
			console_combo = true;
			ReleaseButtons();
			TapKey(K_CONSOLE);
		}
		held_last = held;
		return;
	}
	if (console_combo)
	{
		if (!(held & combo))
			console_combo = false;
		held_last = held;
		return;
	}

	held_last = held;
	for (i = 0; i < NUM_BUTTONS; i++)
	{
		if (!(changed & button_map[i].button))
			continue;
		if (held & button_map[i].button)
		{
			PressButton(i);
		}
		else
		{
			PostKey(pressed_as[i], false);
			pressed_as[i] = KEY_NONE;
		}
	}
}

/*
===============================================================================

STICKS

===============================================================================
*/

/* A round dead zone, and the rest of the travel rescaled to start from nothing. */
static void ShapeStick (s32 raw_x, s32 raw_y, float exponent, float *out_x, float *out_y)
{
	const float	dead = in_nxDeadZone->value;
	const float	x = raw_x / STICK_MAX;
	const float	y = raw_y / STICK_MAX;
	const float	length = sqrtf(x * x + y * y);
	float		scaled;

	if (length <= dead)
	{
		*out_x = *out_y = 0.0f;
		return;
	}
	scaled = ((length < 1.0f ? length : 1.0f) - dead) / (1.0f - dead);
	if (exponent != 1.0f)
		scaled = powf(scaled, exponent);
	*out_x = x / length * scaled;
	*out_y = y / length * scaled;
}

static void SendAxis (int axis, int value)
{
	if (value != axes_last[axis])
	{
		Com_QueueEvent(Sys_Milliseconds(), SE_JOYSTICK_AXIS, axis, value, 0, NULL);
		axes_last[axis] = value;
	}
}

static void ClearAxes (void)
{
	int	i;

	for (i = 0; i < 4; i++)
		SendAxis(i, 0);
}

static void UpdateGameSticks (HidAnalogStickState left, HidAnalogStickState right)
{
	const float	speed = sensitivity_cvar->value / 5.0f;
	float		lx, ly, rx, ry;

	ShapeStick(left.x, left.y, 1.0f, &lx, &ly);
	/* the look's slow end is what aiming needs */
	ShapeStick(right.x, right.y, 2.0f, &rx, &ry);

	SendAxis(0, (int) (lx * STICK_MAX));
	SendAxis(1, (int) (-ly * STICK_MAX));	/* up is forward */
	SendAxis(2, (int) (rx * STICK_MAX * speed));
	SendAxis(3, (int) ((in_nxInvertLook->integer ? ry : -ry) * STICK_MAX * speed));
}

static void MoveCursor (float dx, float dy)
{
	int	move_x, move_y;

	if (dx == 0.0f && dy == 0.0f)
		return;

	cursor_rest_x += dx;
	cursor_rest_y += dy;
	move_x = (int) cursor_rest_x;
	move_y = (int) cursor_rest_y;
	cursor_rest_x -= move_x;
	cursor_rest_y -= move_y;
	if (move_x || move_y)
	{
		cursor_x = CLAMP_TO(cursor_x + move_x, 0, SCREEN_W);
		cursor_y = CLAMP_TO(cursor_y + move_y, 0, SCREEN_H);
		Com_QueueEvent(Sys_Milliseconds(), SE_MOUSE, move_x, move_y, 0, NULL);
		cursor_by_pad = false;
	}
}

static void UpdateMenuCursor (HidAnalogStickState left, HidAnalogStickState right)
{
	/* a little over two seconds from one side of the screen to the other, at 60 Hz */
	const float	speed = 5.0f;
	float		lx, ly, rx, ry;

	ShapeStick(left.x, left.y, 1.6f, &lx, &ly);
	ShapeStick(right.x, right.y, 1.6f, &rx, &ry);
	MoveCursor((lx + rx) * speed, -(ly + ry) * speed);
}

/* A finger is a pointer that jumps. */
static void UpdateTouch (void)
{
	HidTouchScreenState	state;

	if (hidGetTouchScreenStates(&state, 1) && state.count > 0)
	{
		CursorTo((int) (state.touches[0].x * SCREEN_W / 1280.0f), (int) (state.touches[0].y * SCREEN_H / 720.0f));
		if (!touch_down)
		{
			PostKey(K_MOUSE1, true);
			touch_down = true;
		}
		cursor_by_pad = false;
	}
	else if (touch_down)
	{
		PostKey(K_MOUSE1, false);
		touch_down = false;
	}
}

/*
===============================================================================

ENGINE INTERFACE

===============================================================================
*/

static inputMode_t CurrentMode (void)
{
	const int	catcher = Key_GetCatcher();

	if (catcher & KEYCATCH_CONSOLE)
		return MODE_CONSOLE;
	if (catcher & KEYCATCH_MESSAGE)
		return MODE_CHAT;
	if (catcher & KEYCATCH_UI)
		return MODE_MENU;
	if (clc.state != CA_ACTIVE && clc.state != CA_CINEMATIC)
		return MODE_MENU;
	return MODE_GAME;
}

/* The console sleeps inside appletMainLoop: that time is not the game's. */
static void RunAppletLoop (void)
{
	const u64	before = armGetSystemTick();
	const bool	running = appletMainLoop();
	const u64	away_ms = armTicksToNs(armGetSystemTick() - before) / 1000000;

	if (away_ms > 100)
		NX_SkipClock((int) away_ms);
	if (!running)
		Cbuf_ExecuteText(EXEC_APPEND, "quit\n");
}

void IN_Frame (void)
{
	inputMode_t	new_mode;
	u64		held;

	if (!input_ready)
		return;

	RunAppletLoop();
	padUpdate(&pad);

	new_mode = CurrentMode();
	if (new_mode != mode)
	{
		ReleaseButtons();
		ClearAxes();
		if (new_mode == MODE_CHAT)
		{
			mode = new_mode;
			KeyboardForMode();
		}
		mode = new_mode;
	}

	held = padGetButtons(&pad);
	UpdateButtons(held);

	switch (mode)
	{
	case MODE_GAME:
		UpdateGameSticks(padGetStickPos(&pad, 0), padGetStickPos(&pad, 1));
		break;
	case MODE_MENU:
		UpdateMenuCursor(padGetStickPos(&pad, 0), padGetStickPos(&pad, 1));
		UpdateTouch();
		break;
	default:
		break;
	}
}

static void ApplyDefaultLayout (void)
{
	size_t	i;

	if (in_nxBindsVersion->integer >= BINDS_VERSION)
		return;

	for (i = 0; i < ARRAY_LEN(default_binds); i++)
		Key_SetBinding(default_binds[i].key, default_binds[i].command);
	for (i = 0; i < ARRAY_LEN(default_cvars); i++)
		Cvar_Set(default_cvars[i].name, default_cvars[i].value);
	Cvar_Set("in_nxBindsVersion", va("%d", BINDS_VERSION));
}

void IN_Init (void *windowData)
{
	(void) windowData;

	in_nxDeadZone = Cvar_Get("in_nxDeadZone", "0.12", CVAR_ARCHIVE);
	in_nxInvertLook = Cvar_Get("in_nxInvertLook", "0", CVAR_ARCHIVE);
	in_nxBindsVersion = Cvar_Get("in_nxBindsVersion", "0", CVAR_ARCHIVE);
	sensitivity_cvar = Cvar_Get("sensitivity", "5", CVAR_ARCHIVE);

	ApplyDefaultLayout();

	if (!input_ready)
	{
		padConfigureInput(1, HidNpadStyleSet_NpadStandard);
		padInitializeDefault(&pad);
		hidInitializeTouchScreen();
	}
	input_ready = true;
	held_last = 0;
	memset(pressed_as, 0, sizeof(pressed_as));
	memset(axes_last, 0, sizeof(axes_last));
	console_combo = false;
	touch_down = false;
	cursor_by_pad = false;
	cursor_x = SCREEN_W / 2;
	cursor_y = SCREEN_H / 2;
	mode = CurrentMode();
}

void IN_Shutdown (void)
{
	if (input_ready)
	{
		ReleaseButtons();
		ClearAxes();
	}
}

void IN_Restart (void)
{
	IN_Shutdown();
	IN_Init(NULL);
}
