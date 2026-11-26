/*
 * mp_session_int.h -- the session's private seam.
 *
 * mp_session.c was one 17k-line translation unit. It is now split by topic
 * (lobby, spawn, sync, cars, mark presentation, round wrappers, CnR, peds and
 * the test levers) and this header is the only thing those files share: the
 * engine surface they all compile against, plus the session state and helpers
 * that genuinely cross a module boundary.
 *
 * It also owns the engine includes for the whole family. Several of those
 * headers (state.h among them) have no include guard, so a translation unit may
 * only pull them once -- splitting the include list across mp_session.c and its
 * modules would redeclare their contents.
 *
 * Everything here is PRIVATE to the multiplayer module. Nothing outside
 * JERICHO/MODS/mp/mp_*.c may include it, and nothing in it is a public API --
 * the public surface stays in mp.h.
 *
 * mp_round.c, mp_mode.c and mp_mark.c must NOT include this header: they are
 * compiled standalone by tools/mode_check/run_mode_check.py and have to stay
 * free of engine code.
 */
#ifndef MP_SESSION_INT_H
#define MP_SESSION_INT_H

#include "jericho.h"
#include "jer_events.h"
#include "jer_notify.h"	/* the timer modes' golden notices */
#include "jer_hud.h"	/* jer_hud_panel: the timer modes' clock readout */
#include "jer_arrow.h"	/* jer_arrow_on_car: the arrow over the fox / the bomb */
#include "jer_music.h"	/* jer_music_force: a mode's pursuit music while live */
#include "jer_pause_menu.h"	/* JER_PAUSE_QUIT_*: the pause menu's codes (mirror MENU_QUIT_*) */
#include "mp.h"
#include "jer_carquery.h"	/* the mp <-> carhacks live-car query contract */
#include "jer_session.h"	/* the session query sandbox asks (mp answers) */
#include "jer_crumple.h"	/* the mp <-> crumple per-car crush multiplier contract */

#include "driver2.h"
#include "mission.h"
#include "glaunch.h"
#include "replays.h"
#include "pad.h"
#include "cars.h"
#include "convert.h"	/* _RotMatrixY: a car's box is built from its matrix */
#include "cosmetic.h"	/* car_cosmetics[slot]: the box a rebuilt car needs */
#include "gamesnd.h"	/* CarHasSiren: whether a model has a siren, and its bank/sample */
#include "sound.h"		/* StartSound / Start3DSoundVolPitch / SetChannelPosition3 */
#include "jer_sound.h"	/* jer_sound_ensure: a voice a module may keep (never starving SFX) */
#include "jer_carexplode.h"	/* bomb-tag wreck: dent, tumble, visual-only blasts */
#include "jer_carglow.h"	/* the fox / bomb-car glow shell */
#include "pres.h"	/* SetTextColour / PrintStringCentred / PrintTimer: the timer HUD */
#include "jer_screen.h"	/* jer_screen_edges: the side timers sit at the screen's edge */
#include "system.h"	/* CITY_COUNT and LevelNames: the city registry MpCarCityName reads */
#include "denting.h"	/* CreateDentableCar: the only writer of the drawn vertex dump */
#include "civ_ai.h"	/* reservedSlots: the array PingInCivCar's free-slot scan reads */
#include "cop_ai.h"	/* requestCopCar: a cop is waiting for a slot (the cop slot hold) */
#include "felony.h"	/* felonyRating / pedestrianFelony: what a restart clears */
#include "overmap.h"	/* gJerMapForceFlash / gJerMapCopCars: the maps' flash and player cop cones */
#include "models.h"	/* JerCarIsPolice / JerCarSlotIsPolice: the engine's police-car answer */
#include "jer_config.h"	/* the role-car picks, kept per mode in mp's config */
#include "players.h"	/* InitPlayer: a late joiner needs a car the same way the engine makes one */
#include "ai.h"		/* TannerCanEnterCar: the gate the enter-left test lever checks */
#include "handling.h"	/* LongQuaternion2Matrix: rebuild a car's matrix from its body */
#include "state.h"
#include "objcoll.h"	/* CellEmpty: the engine's own scenery test (the warp's landing check) */
#include "dr2roads.h"	/* JerRoadAt / FindSurfaceD2: the road network and the ground height */
#include "jer_map.h"	/* jer_map_*: the region maths behind "is the ground there?" */
#include "map.h"	/* regions_across/down, MAP_REGION_SIZE, units_across_halved: map-wide join spawn */
#include "pedest.h"	/* pUsedPeds: our player's own pedestrian, when on foot */
#include "jer_ped_archive.h"	/* interface-3 JMPPED archive contract (pedhacks) */
#include "motion_c.h"	/* SetSkelModelPointers: the character switch */
#include "jer_npc.h"	/* jer_npc_*: the pedestrian we stand in for a REMOTE one */
#include "camera.h"	/* InitCamera: snap the camera with a placed late joiner */
#include "jer_ped_palette.h"	/* jer_ped_palette_reset: scope the suit-colour cache to a session */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>	/* getenv: the MP_TEST_JERICHO dev lever */
#include <math.h>	/* sqrt: the deviation readout below */

extern int gBootMpLevel;	/* main.c: 1 = the small multiplayer map, 0 = the full city */
extern int gBootMpArena;	/* main.c: which multiplayer map (0..MP_ARENA_MAX) */
extern int gWantNight;		/* glaunch.c: 1 = the night take-a-ride level variant */

/* How long a join/launch reports "busy" before the frontend gives up: shared by
 * the lobby (which raises it) and the launcher (which obeys it). */
#define MP_BUSY_LAUNCH_MS 60000

/* GAME-MODE DECISION POINTS that ask for something this build cannot do yet say so
 * ONCE per kind, then the session carries on with its own behaviour. Mode 0 never asks
 * for anything but that behaviour, so for Take a Ride this never logs. */
enum
{
	MP_MODE_FALLBACK_SPAWN = 0,
	MP_MODE_FALLBACK_KINDS
};

/* ------------------------------------------------------------------------ */
/* Session state and helpers shared across the split modules.               */
/* Everything between the BEGIN/END markers is generated by                 */
/* tmp/mp_split/move.py as each topic moves out of mp_session.c.            */
/* ------------------------------------------------------------------------ */

/* BEGIN SHARED MACROS */
#define MP_INTERP_SNAP_DIST	6000	/* one-sample jump: a teleport, not motion */
#define MP_JOIN_CLEAR	2000
#define MP_JOIN_WIDE_TRIES	8	/* centers probed inside the chosen region */
#define MP_PLACE_SELF_SETTLE	1	/* a placed join settles for one frame */
/* END SHARED MACROS */

/* BEGIN SHARED VARS */
extern char gMpLastRxTag[5];
extern char gMpQuietPad;
extern const char* gHeartbeatStr;
extern int gMp321Count;
extern int gMp321Scale;
extern int gMpChaseFromSet;
extern int gMpChaseFromX, gMpChaseFromZ;
extern int gMpCnrEscapeMs;
extern int gMpCnrHitMarked;
extern int gMpCnrPending;
extern int gMpGoFrame;
extern int gMpLastRxConn;
extern int gMpLaunched;
extern int gMpLocalAppliedPad;
extern int gMpPlaceSelfFrames;
extern int gMpPlaceSelfTry;
extern int gMpRespawnDeferred;
extern int gMpRespawnDelayArmed;
extern int gMpRespawnOut;
extern int gMpSawPreLive;
extern int gMpSpectateCar;
extern int gMpSpectating;
extern int gMpWideRegion;
extern int gMpWideTries;
extern int sMpStandIn;
extern int sMpWantCity;
extern int sMpWantModel;
extern uint8_t gMpPedSeq[MP_MAX_PLAYERS];
extern unsigned char gMpOurCars[MAX_CARS];
extern unsigned char gMpPeerTraffic[MAX_CARS];
extern unsigned long gMpLastRxFrame;
/* END SHARED VARS */

/* BEGIN SHARED FUNCS */
LPPEDESTRIAN MpLocalPed(void);
const char* MpCarCityName(int city);
const char* MpChangeCarRefusal(int city, int model);
const char* MpModeName(int gamemode);
const char* MpTestCarSelect(void);
int MpAdoptRemoteCar(MP_PLAYER* p, int city, int model);
void MpRemoteCarCheck(MP_PLAYER* p);	/* a right-model car that draws black: ask again, rebuild when it can */
int MpAssignedCarCity(void);
int MpAssignedCarModel(int playerId);
int MpCarChangeWaiting(void);
int MpCarListForCity(int city, int* slots, int* models, int max);
int MpCarQueryCities(int* out, int max);
int MpCarSlotOwnedByPlayer(int slot, int ignoreId);
int MpCarSlotTakenByOther(const MP_PLAYER* leaver, int slot);
int MpChangeCar(int city, int model);
int MpCityFromWire(uint8_t wire);
int MpCnrHitLineVeto(int victimCarId);
int MpColorLen(int count);
int MpCrushScaleFromHalf(uint8_t half);
int MpEnsureJerichoImported(void);
int MpEnsureJerichoInstalled(void);
int MpFindEnteredParkedBody(const MP_PLAYER* pl, const MP_CARSTATE_ENTRY* e);
int MpFindRoadSpotNear(int cx, int cy, int cz, int seed, int* outX, int* outZ, int* outDir, int* outRing);
int MpFindRoadSpotWide(int carId, int* outX, int* outZ, int* outDir);
int MpHostCnrTick(int ms);
int MpJoinSpotClearOfPlayers(int x, int z, int ignoreId, int ignoreSlot);
int MpLocalPad(void);
int MpPedModelEffective(int model);
int MpPedModelFromWire(int id);
int MpPlaceOnGround(CAR_DATA* cp, int x, int z, int dir);
int MpPlayerCarReady(const MP_PLAYER* p);
int MpRejoinActive(void);
int MpRemoteTargetsLocalCar(int slot, const char* what, int otherId);
int MpResidentSlotForCar(int city, int model);
int MpRoadHeadingAt(int x, int y, int z, int* outDir);
int MpSessionModeMinutes(void);
int MpSetStartCar(int slot, MP_PLAYER* p);
int MpTrafficSlotOwnerRank(int slot);
int MpWarpSpotOk(int x, int z);
int MpWarpStreamTo(int x, int z);
int MpWarpToPlayer(int playerId);
int16_t MpSat16(long v);
long MpClampAngVel(long v);
long long MpDistSq64(int dx, int dz);
uint8_t MpCityToWire(int city);
uint8_t MpCrushHalfFromScale(int scale);
void MpApplyRemoteCarPoses(void);
void MpCarChangeWaitForget(void);
void MpCarFailsafeTick(void);
void MpCarQueryChosen(int city, int model, int changed);
void MpCarsReset(void);
void MpClearDeathState(void);
void MpClearFelony(int notify);
void MpClientRoundCheck(void);
void MpCnrDamageTick(int phase, int marked, int myId);
void MpCnrRadioClear(void);
void MpCnrRadioTick(int on, int marked, int myId);
void MpCnrReset(void);
void MpDriveRemotePed(MP_PLAYER* p);
void MpFailsafeNoteCar(int slot);
void MpFailsafeReset(void);
void MpFollowLocalCar(void);
void MpHandleCar(int connIndex, const unsigned char* p, int len);
void MpHandleCarState(int connIndex, const unsigned char* p, int len);
void MpHostNoteClientMode(int playerId, int mode);	/* mp_round_net.c: a client's carstate names its mode */
void MpHandleChannel(int connIndex, const unsigned char* p, int len);
void MpHandleChat(int connIndex, const unsigned char* p, int len);
void MpHandleColor(int connIndex, const unsigned char* p, int len);
void MpHandleHello(int connIndex, const unsigned char* p, int len);
void MpHandleHit(int connIndex, const unsigned char* p, int len);
void MpHandleInput(int connIndex, const unsigned char* p, int len);
void MpHandleKick(int connIndex, const unsigned char* p, int len);
void MpHandleLeave(int connIndex, const unsigned char* p, int len);
void MpHandleMark(int connIndex, const unsigned char* p, int len);
void MpHandlePedState(int connIndex, const unsigned char* p, int len);
void MpHandleReject(const unsigned char* p, int len);
void MpHandleRoster(const unsigned char* p, int len);
void MpHandleRound(int connIndex, const unsigned char* p, int len);
void MpHandleStart(const unsigned char* p, int len);
void MpHandleStats(const unsigned char* p, int len);
void MpHandleTraffic(int connIndex, const unsigned char* p, int len);
void MpHandleWelcome(const unsigned char* p, int len);
void MpHeartbeatTick(void);
void MpHitFrame(void);
void MpHitSummary(void);
void MpHostBenchOut(int id, const char* how);
void MpHostCarRowRound(int from, const MP_CARSTATE_ENTRY* e);
void MpHostCnrFlush(void);
void MpHostMarkBegin(void);
void MpHostMarkTick(void);
int  MpHostMarkForceEnd(void);
int  MpHostMarkForceRestart(void);
void MpHostRoundBegin(void);
void MpHostRoundTick(int periodic);
void MpHostSendMark(int connIndex);
void MpHostSendRoster(void);
void MpHostSendRound(int connIndex);
void MpHostSendStats(void);
void MpKeepOurCarsFromTrafficAi(void);
void MpKickPlayer(int playerId);
void MpLaunchLocal(void);
void MpLeaveSession(void);
void MpListenRotationTick(void);
void MpLogInboundPedModel(const char* source, int playerId, int slot, int wire, int effective);
void MpMapCopsTick(void);
void MpMarkBusy(int ms);
void MpMarkHostDamage(int from, int damage, int maxDamage, const char* how);
void MpMarkHostReport(int a, int b, int checkDistance, int strength);
void MpMarkPresentClear(void);
void MpMarkPresentReset(void);
void MpMarkPresentTick(void);
void MpMarkRosterNoCarOnFoot(int self);
void MpMaterializeLeftCar(MP_PLAYER* pl, const MP_CARSTATE_ENTRY* e);
void MpMirrorSirenTick(void);
void MpModeFallbackOnce(int kind, const char* what, int asked);
void MpModeNoteAdopted(int gamemode, const char* how);
void MpPedReset(void);
void MpPlaceSelfTick(void);
void MpPlayClear(int id);
void MpPlayResetAll(void);
void MpRecordSelfPose(void);
void MpReleaseOurCarSlot(int slot);
void MpReleaseRemoteCar(MP_PLAYER* p);
void MpResolveTestLevers(void);
void MpRespawnHoldTick(void);
void MpRoleCarApply(void);
void MpRolePicksForget(const char* why);
void MpRoundHostSync(void);
void MpRoundNetReset(void);
void MpSelfClearFelony(void);
void MpSendColors(int whole);
void MpSendInput(int pad);
void MpSendOwnCarState(void);
void MpSendOwnPedState(void);
void MpSendPickedCar(int model, int city);
void MpSendTrafficState(void);
void MpSessionAdoptLiveJoin(void);
void MpSessionReset(void);
void MpSetCarCrushScale(int carId, int scale);
void MpSetMapShape(int mpLevel, int arena);
void MpSpawnLateJoiners(void);
void MpSpawnReset(void);
void MpSyncReset(void);
void MpTestCarChangeTick(void);
void MpTestCarCycleTick(void);
void MpTestCityChangeTick(void);
void MpTestEnterLeftTick(void);
void MpTestFallOffTick(void);
void MpTestFelonyTick(void);
void MpTestKickTick(void);
void MpTestLeaveTick(void);
void MpTestOnFootTick(void);
void MpTestPauseCarTick(void);
void MpTestReloadCarsTick(void);
void MpTestReset(void);
void MpTestRestartTick(void);
void MpTestTrafficHitTick(void);
void MpTrafficBandClear(void);
void MpTrafficBandRefresh(void);
void MpTrafficClaimPass(void);
void MpTrafficMirrorAgeTick(void);
void MpTrafficMirrorsClear(void);
/* END SHARED FUNCS */

#endif /* MP_SESSION_INT_H */
