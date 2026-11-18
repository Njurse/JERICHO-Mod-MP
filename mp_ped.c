/*
 * mp_ped.c -- split out of mp_session.c
 *
 * Pure code motion: the bodies are the session's own, moved verbatim.
 * Shared session state and helpers are declared in mp_session_int.h.
 */
#include "mp_session_int.h"

static int sMpJerPedInstalledLevel = -1;

/* Import Jericho on demand. One refusal per level is remembered so a stored
 * Jericho on a machine without Havana's data does not retry every frame; an
 * explicit re-selection clears the flag. */
static int sMpJerPedImportFailed;

static int sMpJerPedImportLevel = -1;

static int sMpJerPedBakedFailed;	/* the v5 bake itself was tried and refused */

LPPEDESTRIAN MpLocalPed(void);
int MpPedModelCount(void);
int MpPedModelEntry(int model);
const char* MpPedModelName(int model);
int MpPedModelAvailable(int model);
int MpPedModelEffective(int model);
int MpPedModelFromWire(int id);
void MpLogInboundPedModel(const char* source, int playerId, int slot, int wire, int effective);
static int MpPedTypeForModel(int model);
static void MpSelectLocalPalette(void);
int MpEnsureJerichoInstalled(void);
int MpEnsureJerichoImported(void);
int MpPedArchiveEnsure(void);
static LPPEDESTRIAN MpPreferredCharacterPed(void);
static void MpApplyLocalPedModel(void);
void MpSendOwnPedState(void);

/* Called from MpSessionReset: this module's state is per session, so
 * the next match starts from the same values the module loaded with. */
void MpPedReset(void)
{
	/* no session state of its own */
}

/* The pedestrian the engine is driving for OUR player, or NULL while we are in a
 * car. The engine ties the ped to us in player[0].pPed when it puts us on foot,
 * so take it straight from there: deriving it from a padId scan used to misfire,
 * because a spawned stand-in's padId is never set and could inherit whatever the
 * pooled slot last held. */
LPPEDESTRIAN MpLocalPed(void)
{
	if (player[0].playerCarId < 0)
		return player[0].pPed;

	return NULL;
}

int MpPedModelCount(void)
{
	int entries = jer_ped_archive_entry_count();

	if (entries < 1)
		entries = 1;	/* the Jericho id is always offered */
	if (entries > MP_PED_MODEL_MAX - 1)
		entries = MP_PED_MODEL_MAX - 1;

	return 1 + entries;
}

int MpPedModelEntry(int model)
{
	if (model <= MP_PED_MODEL_TANNER || model >= MP_PED_MODEL_MAX)
		return 0;

	return model - 1;
}

const char* MpPedModelName(int model)
{
	const char* name;

	if (model == MP_PED_MODEL_TANNER)
		return "Tanner";

	if (model < 0 || model >= MP_PED_MODEL_MAX)
		return "?";

	name = jer_ped_archive_entry_name(model - 1);

	if (name != NULL && name[0] != '\0')
		return name;

	return (model == MP_PED_MODEL_JERICHO) ? "Jericho" : "?";
}

int MpPedModelAvailable(int model)
{
	int i, entry;

	if (model == MP_PED_MODEL_TANNER)
		return 1;

	if (model < 0 || model >= MP_PED_MODEL_MAX)
		return 0;

	entry = model - 1;

	if (entry == 0)
	{
		for (i = 0; i < 6; i++)
		{
			if (!jer_ped_archive_model_installed(i))
				return 0;
		}

		/* Native Havana models are already resident; imported models are ready
		 * only when their pages and palette are in place. */
		return (GameLevel == 1) ? 1 : jer_ped_archive_ready();
	}

	if (entry >= jer_ped_archive_entry_count())
		return 0;

	for (i = 0; i < 6; i++)
	{
		if (!jer_ped_archive_model_for_entry(entry, i))
			return 0;
	}

	return jer_ped_archive_ready();
}

/* UI-facing: may this character be OFFERED here? Jericho is selectable even
 * before it is resident, because selecting it is what triggers the import. */
int MpPedModelSelectable(int model)
{
	if (model == MP_PED_MODEL_TANNER)
		return 1;

	if (model < 0 || model >= MP_PED_MODEL_MAX)
		return 0;

	if (model >= MpPedModelCount())
		return 0;

	if (MpPedModelAvailable(model))
		return 1;

	/* Jericho is offered before its import, because selecting it is what starts
	 * the import. Later archive entries only appear once the count is known. */
	return (model == MP_PED_MODEL_JERICHO) &&
		(jer_ped_archive_baked_available() || jer_ped_archive_probe(1));
}

int MpPedModelEffective(int model)
{
	if (model == MP_PED_MODEL_TANNER)
		return MP_PED_MODEL_TANNER;

	if (model < 0 || model >= MP_PED_MODEL_MAX)
		return MP_PED_MODEL_TANNER;

	if (MpPedModelAvailable(model))
	{
		/* The bank can be resident while the palette pairs/skeleton have not been
		 * installed yet. Do not draw a half-installed character; fall back to the
		 * baseline Jericho set when it is available, else Tanner. */
		if (sMpJerPedInstalledLevel != GameLevel)
			return MpPedModelAvailable(MP_PED_MODEL_JERICHO)
				? MP_PED_MODEL_JERICHO : MP_PED_MODEL_TANNER;

		return model;
	}

	if (model != MP_PED_MODEL_JERICHO && MpPedModelAvailable(MP_PED_MODEL_JERICHO))
		return MP_PED_MODEL_JERICHO;

	return MP_PED_MODEL_TANNER;
}

/* Validate a character id that arrived from the wire. The id space is stable
 * (mp_proto.h), but never trust a peer's byte: an unknown id becomes Tanner.
 *
 * This deliberately keeps the OWNER'S CHOICE (a valid id) rather than folding it
 * to Tanner just because this machine cannot draw it yet. A remote Jericho is
 * resolved at apply time, where MpDriveRemotePed can import the models first;
 * clamping here would erase the choice and the stand-in would never try. */
int MpPedModelFromWire(int id)
{
	if (id < 0 || id >= MP_PED_MODEL_MAX)
		return MP_PED_MODEL_TANNER;

	return id;
}

/* One inbound-character log line, called only when the stored choice changes —
 * never per JPPD frame. `wire` is what the peer asked for; `effective` is what
 * this level can actually draw. */
void MpLogInboundPedModel(const char* source, int playerId, int slot,
				 int wire, int effective)
{
	if (gMpCtx == NULL)
		return;

	gMpCtx->jer_log(gMpCtx,
		"[mp] character: recv %s id %d for player %d slot %d -> %s (id %d)%s\n",
		source, wire, playerId, slot,
		MpPedModelName(effective), effective,
		(wire != effective) ? " (unavailable/invalid; fallback)" : "");
}

int MpPedModelSelected(void)
{
	return gMp.config.pedModel;
}

/* The skeleton a character uses. Only the two skeleton families are selectable
 * today. */
static int MpPedTypeForModel(int model)
{
	return (MpPedModelEffective(model) == MP_PED_MODEL_TANNER) ? TANNER_MODEL : OTHER_MODEL;
}

/* Put the local player's active palette choice back on the selector. The draw
 * (MpOnPedDraw) recomputes it every frame; doing it here means the frame that
 * follows a skeleton swap is already right. */
static void MpSelectLocalPalette(void)
{
	if (!gMp.config.colorOn)
	{
		jer_ped_palette_select(-1);
		return;
	}

	jer_ped_palette_select(
		jer_ped_palette_team_colour(
			jer_colour_make(gMp.config.colorR, gMp.config.colorG, gMp.config.colorB),
			MP_COLOR_STRENGTH));
}

/* Make the resident six models usable for drawing: append their palette pairs,
 * rebuild the Jericho vertex list and rebind the skeleton. One success per
 * level; a failure leaves MpPedModelEffective on Tanner. */
int MpEnsureJerichoInstalled(void)
{
	if (!MpPedModelAvailable(MP_PED_MODEL_JERICHO))
		return 0;

	if (sMpJerPedInstalledLevel == GameLevel)
		return 1;

	if (!jer_ped_archive_install_models())
		return 0;

	sMpJerPedInstalledLevel = GameLevel;
	return 1;
}

int MpEnsureJerichoImported(void)
{
	if (MpPedModelAvailable(MP_PED_MODEL_JERICHO))
		return MpEnsureJerichoInstalled();

	if (sMpJerPedImportLevel != GameLevel)
	{
		sMpJerPedImportLevel = GameLevel;
		sMpJerPedImportFailed = 0;
		sMpJerPedInstalledLevel = -1;
	}

	if (sMpJerPedImportFailed)
		return 0;

	/* The baked JMPPED.LEV is self-contained and is the shipping path; the live
	 * Havana import remains as a diagnostic fallback. */
	if (jer_ped_archive_baked_available())
	{
		if (jer_ped_archive_import_baked(GameLevel) && MpEnsureJerichoInstalled())
			return 1;

		jer_ped_archive_reset();
	}

	if (!jer_ped_archive_probe(1) || !jer_ped_archive_import_build(1) || !MpEnsureJerichoInstalled())
	{
		sMpJerPedImportFailed = 1;
		jer_ped_archive_reset();

		if (gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] character: Jericho import failed on level %d - staying on Tanner\n",
				GameLevel);

		return 0;
	}

	return MpPedModelAvailable(MP_PED_MODEL_JERICHO);
}

/* A level (re)start rebuilds the engine's ped banks: InitTanner calls
 * JerPedImportReset and jer_ped_palette_init, which drops the imported models,
 * the stored vertex lists' authority and the palette pairs. Forget what we did
 * for the previous world as well, or the same-GameLevel -1 mismatch checks below
 * would trust a stale install and never re-bind. Called from JER_EVENT_GAME_START. */
void MpPedArchiveReset(void)
{
	sMpJerPedInstalledLevel = -1;
	sMpJerPedImportLevel = -1;
	sMpJerPedImportFailed = 0;
	sMpJerPedBakedFailed = 0;
}

/* Import and install the JMPPED archive for a loaded level. Shared by the
 * session path and by single player: a missing bake still leaves the caller on
 * the Jericho/Tanner fallback, and a refusal is remembered per level.
 *
 * Havana's native JERI_* models satisfy entry 0, so the ordinary import path
 * (MpEnsureJerichoImported) short-circuits there -- but native Havana has no
 * entries past 0. The bake is the only source of those, so read it first when
 * the caller actually needs the entry table. */
int MpPedArchiveEnsure(void)
{
	if (gInFrontend || GameLevel < 0)
		return 0;

	if (!sMpJerPedBakedFailed && jer_ped_archive_baked_available() && !jer_ped_archive_ready())
	{
		if (!jer_ped_archive_import_baked(GameLevel))
		{
			jer_ped_archive_reset();
			sMpJerPedBakedFailed = 1;
		}
	}

	return MpEnsureJerichoImported() && MpEnsureJerichoInstalled();
}

/* The pedestrian the engine is driving for OUR player, or NULL while we are in a
 * car. In a session the roster is the authority; in single player there is no
 * MP_PLAYER row at all, so player[0] is -- the engine puts our ped there and
 * leaves playerCarId at -1 for as long as we are on foot. */
static LPPEDESTRIAN MpPreferredCharacterPed(void)
{
	if (gMp.running)
	{
		MP_PLAYER* me = MpLocalPlayer();

		if (me == NULL || me->carId >= 0)
			return NULL;

		return MpLocalPed();
	}

	if (gInFrontend || player[0].playerCarId >= 0)
		return NULL;

	return player[0].pPed;
}

/* Apply the chosen character to OUR pedestrian, if we have one. While in a car
 * there is no ped; the next on-foot frame calls this again, which is exactly
 * "apply when you next get out". Runs in single player as well as a session. */
static void MpApplyLocalPedModel(void)
{
	LPPEDESTRIAN ped = MpPreferredCharacterPed();
	int model, type, entry, gen;
	static int sLocalSkelGen = -1;

	if (ped == NULL)
		return;

	/* A stored archive character that was chosen before the level loaded is
	 * imported here, on the first frame the player actually has a pedestrian.
	 * Jericho alone can use Havana's native models; a custom entry needs the
	 * bake's entry table, which native Havana does not have. */
	if (gMp.config.pedModel >= MP_PED_MODEL_CUSTOM_FIRST)
	{
		MpPedArchiveEnsure();
	}
	else if (gMp.config.pedModel >= MP_PED_MODEL_JERICHO)
	{
		MpEnsureJerichoImported();
		MpEnsureJerichoInstalled();
	}

	model = MpPedModelEffective(gMp.config.pedModel);
	type = MpPedTypeForModel(model);
	entry = MpPedModelEntry(model);
	gen = jer_ped_archive_skeleton_generation();

	if (ped->pedType == type && ped->pedEntry == entry && sLocalSkelGen == gen)
		return;

	ped->pedType = (char)type;
	ped->pedEntry = (signed char)entry;

	/* THE SAME PATH InitTanner AND THE MOTION CODE USE. This rebinds Skel[].pModel
	 * and bodyShiftValue together, so the bones, the vertex lists
	 * (cJerichoVNumbers) and the draw code stay one consistent skeleton. */
	SetSkelModelPointersForEntry(type, entry);

	MpSelectLocalPalette();
	sLocalSkelGen = gen;

	/* Drop any motion/frame state inherited from the previous model. Reuse the
	 * engine's fresh on-foot setup, then stand still at its first frame. */
	ped->type = PED_ACTION_WALK;
	SetupPedestrian(ped);
	ped->speed = 0;
	ped->velocity.vx = 0;
	ped->velocity.vy = 0;
	ped->velocity.vz = 0;

	if (gMpCtx != NULL)
	{
		int requested = gMp.config.pedModel;

		if (model != requested)
			gMpCtx->jer_log(gMpCtx,
				"[mp] character: applied %s (id %d, pedType %d); requested %s (id %d) unavailable -> fallback\n",
				MpPedModelName(model), model, type,
				MpPedModelName(requested), requested);
		else
			gMpCtx->jer_log(gMpCtx,
				"[mp] character: applied %s (id %d, pedType %d)\n",
				MpPedModelName(model), model, type);
	}
}

/* Single-player per-frame entry point. A session drives the apply from the pose
 * send and from MpSetPedModel; a solo game has neither. Do not force a TANNER
 * choice here: the engine's own "play as Jericho" cheat and missions that create
 * the player as OTHER_MODEL must keep that ped. An explicit Tanner pick still
 * applies through MpSetPedModel. */
void MpPreferredCharacterTick(void)
{
	if (gMp.running)
		return;

	if (gMp.config.pedModel < MP_PED_MODEL_JERICHO)
		return;

	MpApplyLocalPedModel();
}

/* Owner -> peers: persist the choice, apply it if we are on foot, and tell
 * everyone. A client's JPCL row goes to the host, which republishes the table;
 * the host sends the table itself. JPPD carries the same id on the next pose, so
 * an on-foot swap is visible on the frame that changes it. */
int MpSetPedModel(int model)
{
	MP_PLAYER* me;
	if (model < 0 || model >= MP_PED_MODEL_MAX)
		return 0;

	/* An explicit pick is also a retry: a previous import failure must not keep
	 * the archive refused for the rest of the level. */
	sMpJerPedImportLevel = GameLevel;
	sMpJerPedImportFailed = 0;
	sMpJerPedBakedFailed = 0;

	/* Single player only warms the archive on demand, so a stored custom id can
	 * arrive before the entry table exists. Import/install here, then the count
	 * below is the archive's real count instead of the pre-import zero. At the
	 * frontend there is no level to pin pages into, so the choice is only stored
	 * and the first on-foot frame applies it. */
	if (!gInFrontend && GameLevel >= 0 && model >= MP_PED_MODEL_JERICHO)
	{
		int ready;

		/* A custom entry needs the bake's entry table even on Havana, where the
		 * resident JERI_* models make the ordinary Jericho path succeed without
		 * reading it. */
		if (model >= MP_PED_MODEL_CUSTOM_FIRST)
			ready = MpPedArchiveEnsure();
		else
			ready = MpEnsureJerichoImported() && MpEnsureJerichoInstalled();

		if (!ready && gMpCtx != NULL)
			gMpCtx->jer_log(gMpCtx,
				"[mp] character: %s (id %d) import is still pending; keeping the choice\n",
				MpPedModelName(model), model);
	}

	/* The picker only offers entries the archive knows. Do not bounce the choice
	 * back just because its models are not resident yet: this function is the
	 * retry, and the first on-foot frame imports it. */
	if (model >= MpPedModelCount())
		return 0;

	gMp.config.pedModel = model;
	MpConfigSave();

	me = MpLocalPlayer();

	if (me != NULL)
		me->pedModel = model;

	/* Apply on foot in single player as well; in a session this is the immediate
	 * swap the peers then learn about from the colour/pose sends. */
	MpApplyLocalPedModel();

	if (gMp.running)
	{
		MpSendColors(0);	/* identity travels with colour */
		MpSendOwnPedState();	/* and with the pose, when there is one */
	}

	if (gMpCtx != NULL)
	{
		int effective = MpPedModelEffective(model);

		gMpCtx->jer_log(gMpCtx,
			"[mp] character: chose %s (id %d); effective %s (id %d)\n",
			MpPedModelName(model), model,
			MpPedModelName(effective), effective);
	}

	return 1;
}

/* Owner -> peers: where our on-foot player is and how fast they are moving.
 * Sent only while on foot; the car state already says "no car" (model 0xFF), so a
 * peer knows to look for this rather than guessing from a paused car. */
void MpSendOwnPedState(void)
{
	unsigned char buf[sizeof(MP_PEDSTATE) + sizeof(MP_PEDSTATE_ENTRY)];
	MP_PEDSTATE h;
	MP_PEDSTATE_ENTRY e;
	MP_PLAYER* me = MpLocalPlayer();
	LPPEDESTRIAN p;
	int len;

	if (me == NULL || !gMp.running)
		return;

	if (me->carId >= 0)
		return;			/* in a car: the car state is the whole story */

	p = MpLocalPed();

	if (p == NULL)
	{
		/* We are on foot but the engine has no pedestrian we can find for our own
		 * player, so there is nothing to send and the other machines see nobody.
		 * That is the whole reason "I can't see other players' Tanners" can happen
		 * even when everything else is healthy, so say it (throttled) rather than
		 * returning in silence.
		 *
		 * The lookup is by padId: there is no playerPedId, and pedest.c only ties
		 * a ped to a player slot that way. */
		if (gMpCtx != NULL && (gMp.frame % 120) == 0)
			gMpCtx->jer_log(gMpCtx,
				"[mp] ped: on foot but found no pedestrian for our own player "
				"(padid %d, carId %d) -- peers will see nobody\n",
				(int)player[0].padid, me->carId);

		return;
	}

	/* We may have got out of a car since the choice was made (or the level may
	 * have changed under a stored Havana-only pick). This is the "apply the next
	 * time they exit" point. */
	MpApplyLocalPedModel();

	memset(&h, 0, sizeof(h));
	memset(&e, 0, sizeof(e));

	h.count = 1;

	/* ORDER: reserved[0] carries a wrapping 1..255 sequence (0 = an older build,
	 * which receivers accept unordered), so a late datagram cannot walk the
	 * stand-in backwards. */
	{
		static uint8_t sSeq;

		if (++sSeq == 0)
			sSeq = 1;

		h.reserved[0] = sSeq;
	}

	e.playerId = (uint8_t)me->id;
	e.flags = (uint8_t)(p->speed != 0 ? MP_PED_MOVING : 0);
	e.pedModel = (uint8_t)MpPedModelFromWire(gMp.config.pedModel);
	e.x = p->position.vx;
	e.y = p->position.vy;
	e.z = p->position.vz;
	e.heading = p->dir.vy & 0xFFF;
	e.speed = p->speed;

	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), &e, sizeof(e));
	len = (int)(sizeof(h) + sizeof(e));

	if (MpIsHost())
		MpHostBroadcast(MP_TAG_PED, 0, buf, len);
	else
		MpSendToHost(MP_TAG_PED, 0, buf, len);
}

/* The owner's idea of where their on-foot player is. Anything we DO about it
 * happens in MpDriveRemotePed, on a sim frame, where the ped table is safe to
 * touch. */
void MpHandlePedState(int connIndex, const unsigned char* p, int len)
{
	MP_PEDSTATE h;
	int i, n, forged = 0;

	if (len < (int)sizeof(MP_PEDSTATE))
		return;

	memcpy(&h, p, sizeof(h));

	n = h.count;

	if (n > MP_MAX_PLAYERS)
		n = MP_MAX_PLAYERS;

	if (len < (int)(sizeof(MP_PEDSTATE) + (size_t)n * sizeof(MP_PEDSTATE_ENTRY)))
		return;

	for (i = 0; i < n; i++)
	{
		MP_PEDSTATE_ENTRY e;
		MP_PLAYER* pl;

		memcpy(&e, p + sizeof(MP_PEDSTATE) + i * sizeof(e), sizeof(e));

		/* A pose may only describe its SENDER (the dedi enforces the same rule):
		 * otherwise one client could walk another player's Tanner around. Checked
		 * before the local/unknown skip, as in MpHandleCarState. */
		if (MpIsHost() && connIndex >= 0 && e.playerId != MpConnPlayerId(connIndex))
		{
			forged = 1;
			continue;
		}

		pl = MpGetPlayer(e.playerId);

		if (pl == NULL || pl->isLocal)
			continue;

		/* Drop a datagram older than the last one applied (see MpSendOwnPedState).
		 * Sequence 0 is an older sender: no ordering, as before. A long silence
		 * (> 1 s) resets the window, so a restarted sender is never locked out. */
		if (h.reserved[0] != 0)
		{
			int idx = (int)(pl - gMp.players);
			uint8_t last = gMpPedSeq[idx];

			if (last != 0 && pl->pedLastMs != 0 && (MpNowMs() - pl->pedLastMs) < 1000 &&
				(int8_t)(uint8_t)(h.reserved[0] - last) <= 0)
				continue;

			gMpPedSeq[idx] = h.reserved[0];
		}

		pl->pedX = e.x;
		pl->pedY = e.y;
		pl->pedZ = e.z;
		pl->pedHeading = (int)(e.heading & 0xFFF);
		pl->pedSpeed = e.speed;
		{
			int raw = (int)e.pedModel;
			int model = MpPedModelFromWire(raw);

			if (model != pl->pedModel)
				MpLogInboundPedModel("JPPD", pl->id,
					(int)(pl - gMp.players), raw,
					MpPedModelEffective(model));

			pl->pedModel = model;
		}
		pl->pedLastMs = MpNowMs();
	}

	/* The host is the hub, and this needs relaying exactly as the carstate does:
	 * a client sends its pedestrian to the host alone, so without this only the
	 * host ever sees an on-foot CLIENT -- every other client sees a gap where a
	 * person should be, which reads as "I can see the host on foot but not each
	 * other". The carstate relay (MpHandleCarState) is the same rule. */
	if (MpIsHost() && connIndex >= 0 && !forged)
		MpHostRelay(connIndex, MP_TAG_PED, 0, p, len);
}

/* Keep the pedestrian we are standing in for a REMOTE on-foot player in step,
 * spawning and despawning it as that player gets in and out. Every sim frame,
 * every player: building peds from a network callback would do it mid-poll. */
void MpDriveRemotePed(MP_PLAYER* p)
{
	JerNpc* n;
	LPPEDESTRIAN ped;
	int dx, dz, err, tol, gen, entry, applied;

	if (p == NULL || !p->active || p->isLocal)
		return;

	n = (JerNpc*)p->ped;

	/* Not on foot any more -- back in a car, gone, or never heard from: take the
	 * stand-in away. A pedestrian left standing would read as a third player. */
	if (p->carId >= 0 || !p->connected || p->pedLastMs == 0)
	{
		if (n != NULL)
		{
			jer_npc_despawn(n);
			p->ped = NULL;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] ped: player %d is back in a car; stand-in removed\n", p->id);
		}

		return;
	}

	/* A remote owner may have chosen any archive entry. Import the archive here,
	 * on the machine that has to draw the stand-in, before choosing the type.
	 * A failure falls back through MpPedModelEffective and is remembered for the
	 * level, so a peer without the archive does not retry every frame. */
	if (p->pedModel >= MP_PED_MODEL_JERICHO && !MpPedModelAvailable(p->pedModel))
		MpEnsureJerichoImported();

	if (p->pedModel >= MP_PED_MODEL_JERICHO)
		MpEnsureJerichoInstalled();

	applied = MpPedModelEffective(p->pedModel);
	entry = MpPedModelEntry(applied);
	gen = jer_ped_archive_skeleton_generation();

	if (n != NULL)
	{
		int want = MpPedTypeForModel(p->pedModel);

		if (((LPPEDESTRIAN)n)->pedType != want ||
			((LPPEDESTRIAN)n)->pedEntry != entry || p->pedSkelGen != gen)
		{
			/* The owner changed character. Rebuild the stand-in rather than
			 * mutating a ped whose motion/vertex state belongs to the old model:
			 * the spawn path is the same one a fresh player takes, and the
			 * skeleton is rebound by the draw code on its first frame. */
			jer_npc_despawn(n);
			p->ped = NULL;
			n = NULL;

			if (gMpCtx != NULL)
				gMpCtx->jer_log(gMpCtx,
					"[mp] ped: player %d changed character to %s; stand-in rebuilt\n",
					p->id, MpPedModelName(p->pedModel));
		}
	}

	if (n == NULL)
	{
		n = jer_npc_spawn_model(MpPedTypeForModel(p->pedModel), p->pedX, p->pedZ);

		if (n == NULL)
		{
			/* The ped pool can refuse. Say so (throttled): silently having no
			 * character for a player is the confusing half of this feature. */
			if (gMpCtx != NULL && (gMp.frame % 120) == 0)
				gMpCtx->jer_log(gMpCtx,
					"[mp] ped: no free pedestrian for player %d (pool full?)\n", p->id);

			return;
		}

		p->ped = n;
		p->pedSkelGen = gen;

		/* A stand-in is NOT a local player's ped: mark it so a padId-keyed scan (or
		 * anything else that treats padId >= 0 as "someone's character") can never
		 * mistake it for ours -- and so AnimatePed (jer_npc_tick) writes the unused
		 * player slot, never the local player's. jer_npc_spawn now stamps -1 on every
		 * module-spawned ped, so this is belt-and-braces rather than the only guard:
		 * it was the only one when a pooled slot could inherit the local player's
		 * padId (which is how a remote Tanner once took the player's colour). */
		((LPPEDESTRIAN)n)->padId = -1;
		((LPPEDESTRIAN)n)->pedEntry = (signed char)entry;

		if (gMpCtx != NULL)
		{
			gMpCtx->jer_log(gMpCtx,
				"[mp] ped: standing in for player %d slot %d at %d,%d,%d heading %d speed %d; applied %s (id %d, pedType %d)%s\n",
				p->id, (int)(p - gMp.players), p->pedX, p->pedY, p->pedZ,
				p->pedHeading, p->pedSpeed,
				MpPedModelName(applied), applied,
				((LPPEDESTRIAN)n)->pedType,
				(applied != p->pedModel) ? " (unavailable; fallback)" : "");
		}
	}

	ped = (LPPEDESTRIAN)n;

	if (ped == NULL)
	{
		p->ped = NULL;
		return;
	}

	/* Let the ENGINE walk it, and correct only when it has actually drifted.
	 * Pinning it every frame would stop the walk animation from ever showing --
	 * the animation is the whole reason for driving it this way -- while never
	 * correcting it would let it wander. 120 units is about a car length: large
	 * enough that a correction is unmistakable when it happens, small enough that
	 * it is rare. */
	dx = p->pedX - ped->position.vx;
	dz = p->pedZ - ped->position.vz;
	err = (int)sqrt((double)dx * (double)dx + (double)dz * (double)dz);

	/* The sample is already (half a ping + its age) old, and the stand-in keeps
	 * walking at the owner's speed in the meantime, so a moving stand-in LEADS the
	 * last sample by about speed x that many frames. A flat 120 made every runner
	 * on a >100 ms link snap back several times a second (on-foot rubber-banding);
	 * widen the gate by that expected lead, capped so a real drift still corrects. */
	{
		int lagMs = (int)(MpNowMs() - p->pedLastMs) + (p->pingMs > 0 ? p->pingMs / 2 : 0);
		int lagFrames = lagMs / 33;
		int spd = p->pedSpeed < 0 ? -p->pedSpeed : p->pedSpeed;

		if (lagFrames > 10)
			lagFrames = 10;

		tol = 120 + spd * lagFrames;
	}

	if (err > tol)
	{
		jer_npc_set_world(n, p->pedX, p->pedY, p->pedZ, p->pedHeading);

		if (gMpCtx != NULL && MpDebugOn())
			gMpCtx->jer_log(gMpCtx, "[mp] ped: player %d corrected %d units\n", p->id, err);
	}

	/* Mirror the owner's gait directly: their heading, their speed (negative =
	 * backing up), and the idle/walk/run clip that goes with it. The old
	 * move_to aimed at a point 300 units along the owner's heading but measured
	 * from the OWNER, so a lagging stand-in flipped to face the far side and the
	 * clip/reverse flag were never set -- the "stuck walking backwards" Tanner.
	 * A pose older than 400 ms means the owner stopped sending: stand idle
	 * rather than walk on forever. */
	{
		int spd = p->pedSpeed;

		if (MpNowMs() - p->pedLastMs > 400)
			spd = 0;

		jer_npc_set_gait(n, p->pedHeading, spd);
	}

	/* NOW animate + integrate it. Setting the intent is not enough: this ped is
	 * not CIVILIAN, so neither the ambient pedestrian updater (ControlPedestrians)
	 * nor the player-ped loop runs its state function -- and those are the only
	 * callers of AnimatePed. Nothing advanced its animation or moved it, so it
	 * stood still until the err > 120 catch-up above snapped it onto the owner:
	 * that is what "Tanner teleports, no walk animation" was. jer_npc_tick runs
	 * the pass a pedestrian's own state function would, so the stand-in walks with
	 * its legs moving. */
	jer_npc_tick(n);

	/* AnimatePed integrates the stand-in's position from its gait, and that pass
	 * has no wall test. The owner's own ped is collision-clamped by
	 * ProcessTannerPad, but between network corrections the stand-in can cut a
	 * corner and walk into/through geometry. Clamp it with the same collider the
	 * player uses, so a remote character obeys walls too. */
	TannerCollision(ped);

	/* Evidence line: the stand-in's OWN position (what AnimatePed just integrated)
	 * next to the owner's. A walking stand-in TRACKS the owner; a stuck one stays
	 * put and the "corrected" line above fires repeatedly. */
	if (gMpCtx != NULL && MpDebugOn())
	{
		static unsigned long lastPedMs;

		if ((MpNowMs() - lastPedMs) > 2000)
		{
			lastPedMs = MpNowMs();
			gMpCtx->jer_log(gMpCtx,
				"[mp] ped: player %d stand-in at %d,%d,%d (owner %d,%d,%d) speed %d\n",
				p->id, ped->position.vx, ped->position.vy, ped->position.vz,
				p->pedX, p->pedY, p->pedZ, p->pedSpeed);
		}
	}
}

/* The pedestrian the ENGINE is driving for our own player, as a raw pointer,
 * for the draw hook (which cannot include pedest.h). NULL in a car. */
void* MpLocalPedPtr(void)
{
	return (void*)MpLocalPed();
}
