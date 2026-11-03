/* cg_wheel.inc -- the weapon wheel, part of cg_weapons.c in both games.
 *
 * The games pick weapons by number keys, one per bank of weapons, or step through
 * them all. A controller has neither to spare. Holding +weaponwheel opens a ring of the
 * banks the player has something in, the right stick points at one, and letting go
 * takes it. A short tap steps to the next weapon instead, so that the same button
 * does both. Source/platform/in_nx.c hands the right stick over while the ring is open.
 */

#define WHEEL_RADIUS		140.0f
#define WHEEL_ICON_W		72.0f
#define WHEEL_ICON_H		36.0f
#define WHEEL_DEAD_ZONE		0.5f
#define WHEEL_TAP_MSEC		200

static qboolean	wheel_open;
static int	wheel_open_time;
static int	wheel_choice;		/* a weapon, or 0 */

static float Wheel_Axis (const char *cvar)
{
	char	buffer[32];

	trap_Cvar_VariableStringBuffer(cvar, buffer, sizeof(buffer));
	return atof(buffer);
}

static int Wheel_WeaponInBank (int bank)
{
	int	i;

	for (i = 0; i < maxWeapsInBank; i++)
	{
		const int	num = getNextWeapInBank(bank, i);

		if (num && CG_WeaponSelectable(num))
			return num;
	}
	return 0;
}

/* The banks with something to pick. */
static int Wheel_Slots (int *weapons)
{
	int	count = 0;
	int	bank;

	for (bank = 1; bank < maxWeapBanks; bank++)
	{
		const int	num = Wheel_WeaponInBank(bank);

		if (num)
			weapons[count++] = num;
	}
	return count;
}

void CG_WheelDown_f (void)
{
	wheel_open = qtrue;
	wheel_open_time = cg.time;
	wheel_choice = 0;
	trap_Cvar_Set("nx_wheel", "1");
}

void CG_WheelUp_f (void)
{
	const qboolean	was_open = wheel_open;

	wheel_open = qfalse;
	trap_Cvar_Set("nx_wheel", "0");
	if (!was_open || !cg.snap || (cg.snap->ps.pm_flags & PMF_FOLLOW))
		return;

	if (wheel_choice)
	{
		if (cg.snap->ps.weaponstate != WEAPON_RELOADING && wheel_choice != cg.weaponSelect)
		{
			cg.weaponSelectTime = cg.time;
			CG_FinishWeaponChange(cg.weaponSelect, wheel_choice);
		}
	}
	else if (cg.time - wheel_open_time < WHEEL_TAP_MSEC)
	{
		CG_NextWeapon_f();
	}
}

void CG_DrawWeaponWheel (void)
{
	static vec4_t	dim = { 0, 0, 0, 0.5f };
	static vec4_t	lit = { 1.0f, 0.75f, 0.1f, 0.5f };
	int		weapons[MAX_WEAP_BANKS_MP > MAX_WEAP_BANKS ? MAX_WEAP_BANKS_MP : MAX_WEAP_BANKS];
	float		stick_x, stick_y;
	int		count, chosen, i;

	if (!wheel_open || !cg.snap || cg.time - wheel_open_time < WHEEL_TAP_MSEC)
		return;

	count = Wheel_Slots(weapons);
	if (count == 0)
		return;

	stick_x = Wheel_Axis("nx_wheelX");
	stick_y = Wheel_Axis("nx_wheelY");

	chosen = -1;
	if (stick_x * stick_x + stick_y * stick_y > WHEEL_DEAD_ZONE * WHEEL_DEAD_ZONE)
	{
		/* clockwise from the top, as the slots are laid out */
		float	angle = atan2(stick_x, stick_y);

		if (angle < 0)
			angle += 2 * M_PI;
		chosen = (int) (angle / (2 * M_PI) * count + 0.5f) % count;
		wheel_choice = weapons[chosen];
	}

	CG_FillRect(0, 0, 640, 480, dim);

	for (i = 0; i < count; i++)
	{
		const float	angle = (float) i / count * 2 * M_PI;
		const float	x = 320 + sin(angle) * WHEEL_RADIUS;
		const float	y = 240 - cos(angle) * WHEEL_RADIUS;
		const qboolean	selected = weapons[i] == wheel_choice;

		if (selected)
			CG_FillRect(x - WHEEL_ICON_W / 2 - 4, y - WHEEL_ICON_H / 2 - 4, WHEEL_ICON_W + 8, WHEEL_ICON_H + 8, lit);
		CG_RegisterWeapon(weapons[i]);
		CG_DrawPic(x - WHEEL_ICON_W / 2, y - WHEEL_ICON_H / 2, WHEEL_ICON_W, WHEEL_ICON_H,
				cg_weapons[weapons[i]].weaponIcon[selected ? 1 : 0]);
	}

	if (wheel_choice)
	{
		const gitem_t	*item = BG_FindItemForWeapon(wheel_choice);

		if (item && item->pickup_name)
		{
			const int	width = CG_DrawStrlen(item->pickup_name) * BIGCHAR_WIDTH / 2;

			CG_DrawStringExt(320 - width / 2, 232, item->pickup_name, colorWhite, qfalse, qtrue,
					BIGCHAR_WIDTH / 2, BIGCHAR_HEIGHT / 2, 0);
		}
	}
}
