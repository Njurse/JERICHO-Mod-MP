#ifndef MP_BOT_H
#define MP_BOT_H

/* ------------------------------------------------------------------ */
/* The multiplayer TEST BOT -- a testing component, nothing more.       */
/*                                                                      */
/* It drives a REAL player's car so two instances can be exercised      */
/* without two humans at two machines. It is deliberately its own file: */
/* no part of the session, networking or gameplay path may depend on    */
/* it, and it does NOTHING unless the MP_BOT environment variable asks  */
/* for it. If you are reading the mod to understand the multiplayer,    */
/* you can skip this component entirely.                                */
/*                                                                      */
/*   MP_BOT=random   the canned manoeuvre (MP_TESTDRIVE is its old name) */
/*   MP_BOT=chase    the host flees, the joiner chases                   */
/*   MP_BOT=fight    both charge each other                             */
/* ------------------------------------------------------------------ */

/* 1 when the bot is asked for. OFF by default -- never enabled unless the
 * environment variable above is set. */
int MpBotEnabled(void);

/* The pad to apply to OUR OWN car this frame, or 0 when the bot is off. The
 * caller feeds it through the same input path a human's pad takes. */
int MpBotPadForLocalCar(void);

#endif
