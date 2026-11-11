/*
 * core/mp_chat.c -- the chat prompt's keyboard wiring.
 *
 * The pad drives the car, so chat opens on a KEYBOARD key. PsyX hands a module
 * a raw scancode through g_dbg_gameDebugKeys -- the only such hook -- and it is a
 * single slot, so we CHAIN whatever was there rather than clobber it.
 *
 * This file owns the module's half of PsyX's single text-input slot too:
 * MpChatInstall() takes the debug-key slot at boot, and MpChatGrabKeyboard()
 * takes/releases the text-input slot exactly while the prompt is up.
 */
#include "jericho.h"		/* jer_input_block */
#include "jer_console.h"	/* jer_console_scroll, JER_CONSOLE_MAX */

#include <string.h>

#include "mp.h"

/* The chat key needs PsyX's debug-key hook, but NOT its header: PsyX_public.h and
 * SDL_scancode.h both pull in math/SDL defines that collide with the game's in
 * this TU (a C4005 'M_PI': macro redefinition). So the one symbol is declared
 * here -- matching GameDebugKeysHandlerFunc, and C-linkage because PsyX_public.h
 * wraps the declaration in extern "C" -- and the two scancodes are named
 * constants (SDL2 USB-HID indices, fixed). */
typedef void (*MpDebugKeysFn)(int nKey, char down);
typedef void (*MpTextInputFn)(const char* buf);

#ifdef __cplusplus
extern "C" {
#endif
extern MpDebugKeysFn g_dbg_gameDebugKeys;
extern MpTextInputFn g_cfg_gameOnTextInput;
#ifdef __cplusplus
}
#endif

/* The scancode names themselves (MP_KEY_CHAT_*) live in mp.h: the frame hook's
 * MP_TEST_CHATKEY lever drives the same keys, so the two share them. This one is
 * chat-only because it is expressed in console rows. */
#define MP_CHAT_PAGE_ROWS	(JER_CONSOLE_MAX - 1)	/* one row of overlap per page */

/* Either Enter sends the chat line.
 *
 * The keypad Enter is a SEPARATE scancode (88) and it reaches this hook like any
 * other key -- PsyX only special-cases the main RETURN, never KP_ENTER. Some
 * remotes and X11 servers deliver KP_ENTER for a key the user thinks of as
 * "Enter", so matching only scancode 40 made "Enter does not send" depend on
 * which Enter the client happened to send. Accept both; the CANCEL key (Esc) has
 * no such twin to worry about. */
static int MpIsChatSendKey(int nKey)
{
	return nKey == MP_KEY_CHAT_SEND || nKey == MP_KEY_CHAT_SEND_KP;
}

static MpDebugKeysFn gMpPrevDebugKeys = NULL;
static MpTextInputFn gMpPrevTextInput = NULL;

/* Take PsyX's single debug-key slot at boot, CHAINING whatever held it. Guarded
 * against a second boot (a reload): re-installing would chain MpOnDebugKey onto
 * itself and recurse forever -- the console and the free camera share this slot. */
void MpChatInstall(void)
{
	if (g_dbg_gameDebugKeys != MpOnDebugKey)
	{
		gMpPrevDebugKeys = g_dbg_gameDebugKeys;
		g_dbg_gameDebugKeys = MpOnDebugKey;
	}
}

/* Take/release PsyX's single text-input slot, CHAINING whoever had it. Idempotent
 * on purpose: the frame hook re-asserts it every frame, so any path that forgot
 * to release self-heals instead of leaving the keyboard grabbed. */
void MpChatGrabKeyboard(int on)
{
	if (on)
	{
		if (g_cfg_gameOnTextInput != MpOnTextInput)
		{
			gMpPrevTextInput = g_cfg_gameOnTextInput;
			g_cfg_gameOnTextInput = MpOnTextInput;
		}
	}
	else if (g_cfg_gameOnTextInput == MpOnTextInput)
	{
		g_cfg_gameOnTextInput = gMpPrevTextInput;
		gMpPrevTextInput = NULL;
	}
}

/* PsyX hands every typed character here while the prompt is up. NULL is the
 * BACKSPACE key (PsyX's convention for it -- there is no character). Everything
 * else appends, up to the buffer, terminator included. */
void MpOnTextInput(const char* text)
{
	/* The manual address field first: it lives in the FRONTEND, where chat never
	 * runs, and it is the only other claimant of this slot. */
	if (MpUiManualEditing())
	{
		/* Enter can arrive here as text on some backends: treat it as done. */
		if (text != NULL && (text[0] == '\r' || text[0] == '\n'))
			MpUiManualCommit();
		else
			MpUiManualType(text);

		return;
	}

	if (!gMp.chatOpen)
	{
		/* not ours: pass it on untouched. Drop any swallow left over, so the next
		 * prompt cannot eat a real keystroke. */
		gMp.chatSwallowText = 0;

		if (gMpPrevTextInput != NULL)
			gMpPrevTextInput(text);
		return;
	}

	/* The key that OPENED the prompt is delivered as TEXT as well on this pump: a
	 * keydown and its text event are separate SDL events, in that order, so the
	 * line used to begin with the character that opened it (every message started
	 * with "t"). Drop exactly that one event -- not a keystroke meant for the line. */
	if (gMp.chatSwallowText)
	{
		gMp.chatSwallowText = 0;
		return;
	}

	if (text == NULL)		/* backspace: drop the last character */
	{
		size_t n = strlen(gMp.chatBuf);

		if (n > 0)
			gMp.chatBuf[n - 1] = '\0';
		return;
	}

	/* Enter may arrive here as text on some backends; treat it as SEND. */
	if (text[0] == '\r' || text[0] == '\n')
	{
		MpChatSendText(gMp.chatBuf);
		MpChatGrabKeyboard(0);
		return;
	}

	{
		size_t have = strlen(gMp.chatBuf);
		size_t room = sizeof(gMp.chatBuf) - 1 - have;
		size_t i;

		for (i = 0; text[i] != '\0' && i < room; i++)
			gMp.chatBuf[have + i] = text[i];

		gMp.chatBuf[have + i] = '\0';
	}
}

void MpOnDebugKey(int nKey, char down)
{
	/* Chain to whoever held this hook BEFORE us -- the engine's own key handlers and
	 * any earlier module -- EXCEPT while the chat prompt is up. The keyboard then
	 * belongs to the line being typed, and every letter in it is also a key the
	 * engine would act on: "wasd" drives the free camera, "`" opens the console,
	 * 1/2/3/0 hit the game debug keys. Our own prompt keys (Enter/Escape) are
	 * consumed below and must not be passed on either. */
	if (!gMp.chatOpen && gMpPrevDebugKeys != NULL)
		gMpPrevDebugKeys(nKey, down);

	if (!down)
		return;

	if (nKey == MP_KEY_CHAT_CANCEL && MpRejoinActive())
		MpRejoinCancel("Esc");

	/* The manual address field is in the FRONTEND, so this sits BEFORE the in-game
	 * gate below: while it is editing it owns Enter and Escape. */
	if (MpUiManualEditing())
	{
		if (MpIsChatSendKey(nKey))
			MpUiManualCommit();
		else if (nKey == MP_KEY_CHAT_CANCEL)
			MpUiManualCancel();

		return;
	}

	/* In a live match only: while the frontend is up the engine's own text fields
	 * (and every other module) want the keyboard. */
	if (!MpIsActive() || !gMp.running)
		return;

	if (nKey == MP_KEY_CHAT_OPEN && !gMp.chatOpen)
	{
		MpChatOpen();
		MpChatGrabKeyboard(1);	/* the prompt owns the keyboard while it is up */
		jer_input_block(1);	/* from this frame: the chat key itself must not leak */
	}
	else if (MpIsChatSendKey(nKey) && gMp.chatOpen)
	{
		/* Enter sends: the owner echoes the line and the host fans it out. */
		MpChatSendText(gMp.chatBuf);
		MpChatGrabKeyboard(0);
	}
	else if (nKey == MP_KEY_CHAT_PGUP && gMp.chatOpen)
	{
		/* Only while typing: the prompt already owns the keyboard then (nothing is
		 * chained), so PgUp/PgDn cannot collide with an engine or module key. */
		jer_console_scroll(MP_CHAT_PAGE_ROWS);
	}
	else if (nKey == MP_KEY_CHAT_PGDN && gMp.chatOpen)
	{
		jer_console_scroll(-MP_CHAT_PAGE_ROWS);
	}
	else if (nKey == MP_KEY_CHAT_CANCEL && gMp.chatOpen)
	{
		gMp.chatOpen = 0;
		gMp.chatBuf[0] = '\0';
		MpChatGrabKeyboard(0);
	}
}
