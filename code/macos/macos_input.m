/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2026 the OpenMoHAA Metal port contributors

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// macos_input.m -- input, native: the IN_* functions of sdl/sdl_input.c and sdl/sdl_mouse.c, which it replaced
// (Phase 6; the SDL files are in the last commit that had them, SDL_REF in port/build.sh).
// The event pump, the window's events (focus, minimize, resize, quit), the keyboard and text, the mouse and the
// cursor. No gamepads or joysticks: the user won't use them (docs/METAL_PORT_PLAN.md, 3.1 #12).

#import <Carbon/Carbon.h>	// the keyboard layout (TIS, UCKeyTranslate) and the kVK_* virtual key codes
#import <GameController/GameController.h>	// GCMouse: the mouse's raw movement (in_mouseRaw)
#include <os/lock.h>
#include <time.h>

#include "macos_local.h"
#include "../client/client.h"
#include "../sys/sys_local.h"

static cvar_t *in_keyboardDebug     = NULL;

static NSWindow *in_window;
static NSArray *in_windowObservers;

static int vidRestartTime = 0;

static int in_eventTime = 0;

#define CTRL(a) ((a)-'a'+1)

/*
==============================================================================

KEYBOARD

The keys resolve exactly as they did through SDL 2.32 (its Cocoa keyboard, read from its binary) and sdl_input.c,
so every key gives the same Q3 key and saved binds keep working: the virtual key code (a physical position) gives
SDL's scancode (a USB HID usage), the scancode gives the layout's unshifted character (SDL's key code, "sym"), and
IN_TranslateKey makes the Q3 key from both, as IN_TranslateSDLToQ3Key did.

The key events are read in the pump, before AppKit dispatches them (MacOS_PumpEvents), as SDL read them.
==============================================================================
*/

// SDL's scancodes (USB HID usage IDs), the numbering IN_TranslateKey and K_WORLD_0 + scancode use
enum
{
	SC_A = 4, SC_Z = 29, SC_1 = 30, SC_9 = 38, SC_0 = 39, SC_RETURN = 40, SC_SLASH = 56, SC_GRAVE = 53,
	SC_CAPSLOCK = 57, SC_F1 = 58, SC_F2, SC_F3, SC_F4, SC_F5, SC_F6, SC_F7, SC_F8, SC_F9, SC_F10, SC_F11, SC_F12,
	SC_PRINTSCREEN = 70, SC_SCROLLLOCK, SC_PAUSE, SC_INSERT, SC_HOME, SC_PAGEUP, SC_DELETE, SC_END, SC_PAGEDOWN,
	SC_RIGHT, SC_LEFT, SC_DOWN, SC_UP, SC_NUMLOCKCLEAR, SC_KP_DIVIDE, SC_KP_MULTIPLY, SC_KP_MINUS, SC_KP_PLUS,
	SC_KP_ENTER, SC_KP_1, SC_KP_2, SC_KP_3, SC_KP_4, SC_KP_5, SC_KP_6, SC_KP_7, SC_KP_8, SC_KP_9, SC_KP_0,
	SC_KP_PERIOD, SC_NONUSBACKSLASH, SC_APPLICATION, SC_POWER, SC_F13 = 104, SC_F14, SC_F15, SC_HELP = 117,
	SC_MENU, SC_UNDO = 122, SC_INTERNATIONAL1 = 135, SC_INTERNATIONAL3 = 137, SC_LANG1 = 144, SC_LANG2,
	SC_SYSREQ = 154, SC_LCTRL = 224, SC_LSHIFT, SC_LALT, SC_LGUI, SC_RCTRL, SC_RSHIFT, SC_RALT, SC_RGUI,
	SC_MODE = 257,

	SC_NUM = 256	// the scancodes kept; SDL's macOS table yields none above SC_RGUI
};

// SDL's key code of a key without a character (SDL_SCANCODE_TO_KEYCODE)
#define SYM( scancode )		( ( scancode ) | ( 1 << 30 ) )
#define SYM_IS_SCANCODE( sym )	( ( ( sym ) & ( 1 << 30 ) ) != 0 )

// SDL's modifier state (KMOD_*), for in_keyboardDebug and caps lock
#define KMOD_LSHIFT	0x0001
#define KMOD_RSHIFT	0x0002
#define KMOD_LCTRL	0x0040
#define KMOD_RCTRL	0x0080
#define KMOD_LALT	0x0100
#define KMOD_RALT	0x0200
#define KMOD_LGUI	0x0400
#define KMOD_RGUI	0x0800
#define KMOD_NUM	0x1000
#define KMOD_CAPS	0x2000
#define KMOD_MODE	0x4000
#define KMOD_SCROLL	0x8000

// The device-dependent modifier bits of NSEvent.modifierFlags (NX_DEVICE*KEYMASK, IOKit's IOLLEvent.h): which side
// of a modifier is down
#define NX_DEVICELCTLKEYMASK	0x00000001
#define NX_DEVICELSHIFTKEYMASK	0x00000002
#define NX_DEVICERSHIFTKEYMASK	0x00000004
#define NX_DEVICELCMDKEYMASK	0x00000008
#define NX_DEVICERCMDKEYMASK	0x00000010
#define NX_DEVICELALTKEYMASK	0x00000020
#define NX_DEVICERALTKEYMASK	0x00000040
#define NX_DEVICERCTLKEYMASK	0x00002000

// SDL's darwin_scancode_table (copied from its binary): virtual key code -> scancode, 0 = no key
static const byte in_vkToScancode[ 128 ] =
{
	  4,  22,   7,   9,  11,  10,  29,  27,	// 0x00 A S D F H G Z X
	  6,  25, 100,   5,  20,  26,   8,  21,	// 0x08 C V ISO_Section B Q W E R
	 28,  23,  30,  31,  32,  33,  35,  34,	// 0x10 Y T 1 2 3 4 6 5
	 46,  38,  36,  45,  37,  39,  48,  18,	// 0x18 Equal 9 7 Minus 8 0 RightBracket O
	 24,  47,  12,  19,  40,  15,  13,  52,	// 0x20 U LeftBracket I P Return L J Quote
	 14,  51,  49,  54,  56,  17,  16,  55,	// 0x28 K Semicolon Backslash Comma Slash N M Period
	 43,  44,  53,  42,  88,  41, 231, 227,	// 0x30 Tab Space Grave Delete - Escape RightCommand Command
	225,  57, 226, 224, 229, 230, 228, 231,	// 0x38 Shift CapsLock Option Control RightShift RightOption RightControl Function
	108,  99,   0,  85,   0,  87,   0,  83,	// 0x40 F17 KPDecimal - KPMultiply - KPPlus - KPClear
	128, 129, 127,  84,  88,   0,  86, 109,	// 0x48 VolumeUp VolumeDown Mute KPDivide KPEnter - KPMinus F18
	110, 103,  98,  89,  90,  91,  92,  93,	// 0x50 F19 KPEquals KP0 KP1 KP2 KP3 KP4 KP5
	 94,  95,   0,  96,  97, 137, 135, 133,	// 0x58 KP6 KP7 F20 KP8 KP9 JIS_Yen JIS_Underscore JIS_KeypadComma
	 62,  63,  64,  60,  65,  66, 145,  68,	// 0x60 F5 F6 F7 F3 F8 F9 JIS_Eisu F11
	144,  70, 107,  71,   0,  67, 101,  69,	// 0x68 JIS_Kana F13 F16 F14 - F10 ContextualMenu F12
	  0,  72,  73,  74,  75,  76,  61,  77,	// 0x70 - F15 Help Home PageUp ForwardDelete F4 End
	 59,  78,  58,  80,  79,  81,  82, 102,	// 0x78 F2 PageDown F1 LeftArrow RightArrow DownArrow UpArrow -
};

static byte					in_keyState[ SC_NUM ];	// SDL's key state: 1 = down
static int					in_keymap[ SC_NUM ];	// scancode -> sym, for the current layout
static int					in_modState;			// KMOD_*
static TISInputSourceRef	in_keyLayout;			// the layout in_keymap was built from
static keyNum_t				lastKeyDown = 0;

/*
===============
IN_DefaultSym

SDL's default key code of a scancode: a key with a character on a US keyboard has that character (the layout then
replaces it), every other key its SYM().
===============
*/
static int IN_DefaultSym( int scancode )
{
	static const char returnToSlash[] = { '\r', '\x1B', '\b', '\t', ' ', '-', '=', '[', ']', '\\', '#', ';', '\'',
		'`', ',', '.', '/' };

	if( scancode >= SC_A && scancode <= SC_Z )
		return 'a' + scancode - SC_A;
	if( scancode >= SC_1 && scancode <= SC_9 )
		return '1' + scancode - SC_1;
	if( scancode == SC_0 )
		return '0';
	if( scancode >= SC_RETURN && scancode <= SC_SLASH )
		return returnToSlash[ scancode - SC_RETURN ];
	if( scancode == SC_DELETE )
		return '\x7F';

	// no character until the layout gives one
	if( scancode == 0 || scancode == SC_NONUSBACKSLASH || scancode == SC_INTERNATIONAL1 ||
		scancode == SC_INTERNATIONAL3 || scancode == SC_LANG1 || scancode == SC_LANG2 )
		return 0;

	return SYM( scancode );
}

/*
===============
IN_IsISOKeyboard

On ISO keyboards the key left of 1 and the key right of left shift come with each other's virtual key codes
(SDL swaps them back, and so do we).
===============
*/
static qboolean IN_IsISOKeyboard( void )
{
	return KBGetLayoutType( LMGetKbdType( ) ) == kKeyboardISO;
}

/*
===============
IN_UpdateKeymap

SDL's UpdateKeymap: when the keyboard layout changed (checked at every key press that isn't a repeat; it costs a
pointer compare), each key with a character gets the layout's unshifted character, without dead keys.
===============
*/
static void IN_UpdateKeymap( void )
{
	TISInputSourceRef layout = TISCopyCurrentKeyboardLayoutInputSource( );
	CFDataRef uchr;
	const UCKeyboardLayout *keyboardLayout;
	UInt32 keyboardType;
	int i;

	if( !in_keymap[ SC_A ] )
	{
		// the first call: the default map until a layout replaces it
		for( i = 0; i < SC_NUM; i++ )
			in_keymap[ i ] = IN_DefaultSym( i );
	}

	if( layout == in_keyLayout )
	{
		if( layout )
			CFRelease( layout );
		return;
	}

	if( in_keyLayout )
		CFRelease( in_keyLayout );
	in_keyLayout = layout;

	// a layout without Unicode data leaves the old map (SDL)
	uchr = layout ? TISGetInputSourceProperty( layout, kTISPropertyUnicodeKeyLayoutData ) : NULL;
	keyboardLayout = uchr ? (const UCKeyboardLayout *)CFDataGetBytePtr( uchr ) : NULL;
	if( !keyboardLayout )
		return;

	for( i = 0; i < SC_NUM; i++ )
		in_keymap[ i ] = IN_DefaultSym( i );

	keyboardType = LMGetKbdType( );

	for( i = 0; i < ARRAY_LEN( in_vkToScancode ); i++ )
	{
		int scancode = in_vkToScancode[ i ];
		UniChar chars[ 8 ];
		UniCharCount length;
		UInt32 deadKeyState = 0;

		if( scancode == 0 || SYM_IS_SCANCODE( in_keymap[ scancode ] ) )
			continue;

		if( ( scancode == SC_NONUSBACKSLASH || scancode == SC_GRAVE ) && IN_IsISOKeyboard( ) )
			scancode = SC_NONUSBACKSLASH + SC_GRAVE - scancode;

		if( UCKeyTranslate( keyboardLayout, i, kUCKeyActionDown, 0, keyboardType, kUCKeyTranslateNoDeadKeysMask,
			&deadKeyState, ARRAY_LEN( chars ), &length, chars ) != noErr )
			continue;

		// 0x10 is the "function key" character
		if( length > 0 && chars[ 0 ] != 0x10 )
			in_keymap[ scancode ] = chars[ 0 ];
	}

	// (SDL also gives the number row of AZERTY layouts digits as key codes; IN_TranslateKey takes the number row
	// by scancode, so only in_keyboardDebug would show the difference)
}

/*
===============
IN_PrintKey
===============
*/
static void IN_PrintKey( int virtualKey, int scancode, int sym, keyNum_t key, qboolean down )
{
	if( down )
		Com_Printf( "+ " );
	else
		Com_Printf( "  " );

	Com_Printf( "Scancode: 0x%02x Sym: 0x%02x", scancode, sym );
	if( sym > ' ' && sym < 0x7F )
		Com_Printf( "(%c)", sym );
	if( virtualKey >= 0 )
		Com_Printf( " kVK: 0x%02x", virtualKey );

	if( in_modState & KMOD_LSHIFT )   Com_Printf( " KMOD_LSHIFT" );
	if( in_modState & KMOD_RSHIFT )   Com_Printf( " KMOD_RSHIFT" );
	if( in_modState & KMOD_LCTRL )    Com_Printf( " KMOD_LCTRL" );
	if( in_modState & KMOD_RCTRL )    Com_Printf( " KMOD_RCTRL" );
	if( in_modState & KMOD_LALT )     Com_Printf( " KMOD_LALT" );
	if( in_modState & KMOD_RALT )     Com_Printf( " KMOD_RALT" );
	if( in_modState & KMOD_LGUI )     Com_Printf( " KMOD_LGUI" );
	if( in_modState & KMOD_RGUI )     Com_Printf( " KMOD_RGUI" );
	if( in_modState & KMOD_NUM )      Com_Printf( " KMOD_NUM" );
	if( in_modState & KMOD_CAPS )     Com_Printf( " KMOD_CAPS" );
	if( in_modState & KMOD_MODE )     Com_Printf( " KMOD_MODE" );
	if( in_modState & KMOD_SCROLL )   Com_Printf( " KMOD_SCROLL" );

	Com_Printf( " Q:0x%02x(%s)\n", key, Key_KeynumToString( key ) );
}

#define MAX_CONSOLE_KEYS 16

/*
===============
IN_IsConsoleKey

TODO: If the SDL_Scancode situation improves, use it instead of
      both of these methods
===============
*/
static qboolean IN_IsConsoleKey( keyNum_t key, int character )
{
	typedef struct consoleKey_s
	{
		enum
		{
			QUAKE_KEY,
			CHARACTER
		} type;

		union
		{
			keyNum_t key;
			int character;
		} u;
	} consoleKey_t;

	static consoleKey_t consoleKeys[ MAX_CONSOLE_KEYS ];
	static int numConsoleKeys = 0;
	int i;

	// Only parse the variable when it changes
	if( cl_consoleKeys->modified )
	{
		char *text_p;
		const char *token;

		cl_consoleKeys->modified = qfalse;
		text_p = cl_consoleKeys->string;
		numConsoleKeys = 0;

		while( numConsoleKeys < MAX_CONSOLE_KEYS )
		{
			consoleKey_t *c = &consoleKeys[ numConsoleKeys ];
			int charCode = 0;

			token = COM_Parse( &text_p );
			if( !token[ 0 ] )
				break;

			charCode = Com_HexStrToInt( token );

			if( charCode > 0 )
			{
				c->type = CHARACTER;
				c->u.character = charCode;
			}
			else
			{
				c->type = QUAKE_KEY;
				c->u.key = Key_StringToKeynum( token );

				// 0 isn't a key
				if( c->u.key <= 0 )
					continue;
			}

			numConsoleKeys++;
		}
	}

	// If the character is the same as the key, prefer the character
	if( key == character )
		key = 0;

	for( i = 0; i < numConsoleKeys; i++ )
	{
		consoleKey_t *c = &consoleKeys[ i ];

		switch( c->type )
		{
			case QUAKE_KEY:
				if( key && c->u.key == key )
					return qtrue;
				break;

			case CHARACTER:
				if( c->u.character == character )
					return qtrue;
				break;
		}
	}

	return qfalse;
}

/*
===============
IN_TranslateKey

sdl_input.c's IN_TranslateSDLToQ3Key, case for case.
===============
*/
static keyNum_t IN_TranslateKey( int virtualKey, int scancode, int sym, qboolean down )
{
	keyNum_t key = 0;

	if( scancode >= SC_1 && scancode <= SC_0 )
	{
		// Always map the number keys as such even if they actually map
		// to other characters (eg, "1" is "&" on an AZERTY keyboard).
		if( scancode == SC_0 )
			key = '0';
		else
			key = '1' + scancode - SC_1;
	}
	else if( sym >= ' ' && sym < '\x7F' )
	{
		// These happen to match the ASCII chars
		key = sym;
	}
	else if( scancode == SC_GRAVE )
	{
		// Added in OPM
		// so the console can be toggled properly
		key = '`';
	}
	else
	{
		switch( sym )
		{
			case SYM( SC_PAGEUP ):       key = K_PGUP;          break;
			case SYM( SC_KP_9 ):         key = K_KP_PGUP;       break;
			case SYM( SC_PAGEDOWN ):     key = K_PGDN;          break;
			case SYM( SC_KP_3 ):         key = K_KP_PGDN;       break;
			case SYM( SC_KP_7 ):         key = K_KP_HOME;       break;
			case SYM( SC_HOME ):         key = K_HOME;          break;
			case SYM( SC_KP_1 ):         key = K_KP_END;        break;
			case SYM( SC_END ):          key = K_END;           break;
			case SYM( SC_KP_4 ):         key = K_KP_LEFTARROW;  break;
			case SYM( SC_LEFT ):         key = K_LEFTARROW;     break;
			case SYM( SC_KP_6 ):         key = K_KP_RIGHTARROW; break;
			case SYM( SC_RIGHT ):        key = K_RIGHTARROW;    break;
			case SYM( SC_KP_2 ):         key = K_KP_DOWNARROW;  break;
			case SYM( SC_DOWN ):         key = K_DOWNARROW;     break;
			case SYM( SC_KP_8 ):         key = K_KP_UPARROW;    break;
			case SYM( SC_UP ):           key = K_UPARROW;       break;
			case '\x1B':                 key = K_ESCAPE;        break;
			case SYM( SC_KP_ENTER ):     key = K_KP_ENTER;      break;
			case '\r':                   key = K_ENTER;         break;
			case '\t':                   key = K_TAB;           break;
			case SYM( SC_F1 ):           key = K_F1;            break;
			case SYM( SC_F2 ):           key = K_F2;            break;
			case SYM( SC_F3 ):           key = K_F3;            break;
			case SYM( SC_F4 ):           key = K_F4;            break;
			case SYM( SC_F5 ):           key = K_F5;            break;
			case SYM( SC_F6 ):           key = K_F6;            break;
			case SYM( SC_F7 ):           key = K_F7;            break;
			case SYM( SC_F8 ):           key = K_F8;            break;
			case SYM( SC_F9 ):           key = K_F9;            break;
			case SYM( SC_F10 ):          key = K_F10;           break;
			case SYM( SC_F11 ):          key = K_F11;           break;
			case SYM( SC_F12 ):          key = K_F12;           break;
			case SYM( SC_F13 ):          key = K_F13;           break;
			case SYM( SC_F14 ):          key = K_F14;           break;
			case SYM( SC_F15 ):          key = K_F15;           break;

			case '\b':                   key = K_BACKSPACE;     break;
			case SYM( SC_KP_PERIOD ):    key = K_KP_DEL;        break;
			case '\x7F':                 key = K_DEL;           break;
			case SYM( SC_PAUSE ):        key = K_PAUSE;         break;

			case SYM( SC_LSHIFT ):
			case SYM( SC_RSHIFT ):       key = K_SHIFT;         break;

			case SYM( SC_LCTRL ):
			case SYM( SC_RCTRL ):        key = K_CTRL;          break;

			case SYM( SC_RGUI ):
			case SYM( SC_LGUI ):         key = K_COMMAND;       break;

			case SYM( SC_RALT ):
			case SYM( SC_LALT ):         key = K_ALT;           break;

			case SYM( SC_KP_5 ):         key = K_KP_5;          break;
			case SYM( SC_INSERT ):       key = K_INS;           break;
			case SYM( SC_KP_0 ):         key = K_KP_INS;        break;
			case SYM( SC_KP_MULTIPLY ):  key = K_KP_STAR;       break;
			case SYM( SC_KP_PLUS ):      key = K_KP_PLUS;       break;
			case SYM( SC_KP_MINUS ):     key = K_KP_MINUS;      break;
			case SYM( SC_KP_DIVIDE ):    key = K_KP_SLASH;      break;

			case SYM( SC_MODE ):         key = K_MODE;          break;
			case SYM( SC_HELP ):         key = K_HELP;          break;
			case SYM( SC_PRINTSCREEN ):  key = K_PRINT;         break;
			case SYM( SC_SYSREQ ):       key = K_SYSREQ;        break;
			case SYM( SC_MENU ):         key = K_MENU;          break;
			case SYM( SC_APPLICATION ):  key = K_MENU;          break;
			case SYM( SC_POWER ):        key = K_POWER;         break;
			case SYM( SC_UNDO ):         key = K_UNDO;          break;
			case SYM( SC_SCROLLLOCK ):   key = K_SCROLLOCK;     break;
			case SYM( SC_NUMLOCKCLEAR ): key = K_KP_NUMLOCK;    break;
			case SYM( SC_CAPSLOCK ):     key = K_CAPSLOCK;      break;

			default:
				if( !SYM_IS_SCANCODE( sym ) && scancode <= 95 )
				{
					// Map Unicode characters to 95 world keys using the key's scan code.
					// FIXME: There aren't enough world keys to cover all the scancodes.
					// Maybe create a map of scancode to quake key at start up and on
					// key map change; allocate world key numbers as needed similar
					// to SDL 1.2.
					key = K_WORLD_0 + scancode;
				}
				break;
		}
	}

	if( in_keyboardDebug->integer )
		IN_PrintKey( virtualKey, scancode, sym, key, down );

	if( IN_IsConsoleKey( key, 0 ) )
	{
		// Console keys can't be bound or generate characters
		key = K_CONSOLE;
	}

	return key;
}

/*
===============
IN_UpdateModState

SDL's modifier state after a key event.
===============
*/
static void IN_UpdateModState( int sym, qboolean down )
{
	int modifier;

	switch( sym )
	{
		case SYM( SC_LCTRL ):  modifier = KMOD_LCTRL;  break;
		case SYM( SC_RCTRL ):  modifier = KMOD_RCTRL;  break;
		case SYM( SC_LSHIFT ): modifier = KMOD_LSHIFT; break;
		case SYM( SC_RSHIFT ): modifier = KMOD_RSHIFT; break;
		case SYM( SC_LALT ):   modifier = KMOD_LALT;   break;
		case SYM( SC_RALT ):   modifier = KMOD_RALT;   break;
		case SYM( SC_LGUI ):   modifier = KMOD_LGUI;   break;
		case SYM( SC_RGUI ):   modifier = KMOD_RGUI;   break;
		case SYM( SC_MODE ):   modifier = KMOD_MODE;   break;
		default:               modifier = 0;           break;
	}

	if( !down )
		in_modState &= ~modifier;
	else if( sym == SYM( SC_NUMLOCKCLEAR ) )
		in_modState ^= KMOD_NUM;
	else if( sym == SYM( SC_CAPSLOCK ) )
		in_modState ^= KMOD_CAPS;
	else if( sym == SYM( SC_SCROLLLOCK ) )
		in_modState ^= KMOD_SCROLL;
	else
		in_modState |= modifier;
}

/*
===============
IN_SendKey

A key went down or up: SDL_SendKeyboardKey, then sdl_input.c's SDL_KEYDOWN or SDL_KEYUP. An unknown key sends
nothing; a key that is already down sends a repeat, dropped unless a key catcher is up; a key going up that isn't
down sends nothing.
===============
*/
static void IN_SendKey( int virtualKey, int scancode, qboolean down )
{
	qboolean repeat = qfalse;
	keyNum_t key;
	int sym;

	if( scancode <= 0 || scancode >= SC_NUM )
		return;

	if( down )
		repeat = in_keyState[ scancode ] != 0;
	else if( !in_keyState[ scancode ] )
		return;

	in_keyState[ scancode ] = down;
	sym = in_keymap[ scancode ];
	IN_UpdateModState( sym, down );

	if( down )
	{
		if( repeat && Key_GetCatcher( ) == 0 )
			return;

		if( ( key = IN_TranslateKey( virtualKey, scancode, sym, qtrue ) ) )
			Com_QueueEvent( in_eventTime, SE_KEY, key, qtrue, 0, NULL );

		if( key == K_BACKSPACE )
			Com_QueueEvent( in_eventTime, SE_CHAR, CTRL('h'), 0, 0, NULL );
		else if (key == K_ENTER || key == K_TAB || key == K_ESCAPE) // Added in OPM: mostly used by UINotepad
			Com_QueueEvent( in_eventTime, SE_CHAR, key, 0, 0, NULL);
		else if( keys[K_CTRL].down && key >= 'a' && key <= 'z' )
			Com_QueueEvent( in_eventTime, SE_CHAR, CTRL(key), 0, 0, NULL );

		lastKeyDown = key;
	}
	else
	{
		if( ( key = IN_TranslateKey( virtualKey, scancode, sym, qfalse ) ) )
			Com_QueueEvent( in_eventTime, SE_KEY, key, qfalse, 0, NULL );

		lastKeyDown = 0;
	}
}

/*
===============
IN_IsModifierPressed

SDL's IsModifierKeyPressed: one side of a modifier from the device-dependent bits, trusting the device-independent
bit when the two disagree (events without device bits).
===============
*/
static qboolean IN_IsModifierPressed( unsigned int flags, unsigned int side, unsigned int otherSide,
	unsigned int either )
{
	qboolean sidePressed = ( flags & side ) != 0;
	qboolean otherPressed = ( flags & otherSide ) != 0;
	qboolean eitherPressed = ( flags & either ) != 0;

	if( eitherPressed != ( sidePressed || otherPressed ) )
		return eitherPressed;

	return sidePressed;
}

/*
===============
IN_SendModifiers

A modifier changed: as SDL, every modifier side is set from the flags in this order (a side that is still down
sends a repeat), then caps lock sends a press and a release when it toggled.
===============
*/
static void IN_SendModifiers( unsigned int flags )
{
	static const struct
	{
		int				scancode;
		unsigned int	side, otherSide, either;
	} modifiers[] =
	{
		{ SC_LSHIFT, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, NSEventModifierFlagShift },
		{ SC_LCTRL,  NX_DEVICELCTLKEYMASK,   NX_DEVICERCTLKEYMASK,   NSEventModifierFlagControl },
		{ SC_LALT,   NX_DEVICELALTKEYMASK,   NX_DEVICERALTKEYMASK,   NSEventModifierFlagOption },
		{ SC_LGUI,   NX_DEVICELCMDKEYMASK,   NX_DEVICERCMDKEYMASK,   NSEventModifierFlagCommand },
		{ SC_RSHIFT, NX_DEVICERSHIFTKEYMASK, NX_DEVICELSHIFTKEYMASK, NSEventModifierFlagShift },
		{ SC_RCTRL,  NX_DEVICERCTLKEYMASK,   NX_DEVICELCTLKEYMASK,   NSEventModifierFlagControl },
		{ SC_RALT,   NX_DEVICERALTKEYMASK,   NX_DEVICELALTKEYMASK,   NSEventModifierFlagOption },
		{ SC_RGUI,   NX_DEVICERCMDKEYMASK,   NX_DEVICELCMDKEYMASK,   NSEventModifierFlagCommand },
	};
	int i;

	for( i = 0; i < ARRAY_LEN( modifiers ); i++ )
	{
		IN_SendKey( -1, modifiers[ i ].scancode, IN_IsModifierPressed( flags, modifiers[ i ].side,
			modifiers[ i ].otherSide, modifiers[ i ].either ) );
	}

	// caps lock is a toggle: AppKit tells its state, not the key's
	if( ( ( flags & NSEventModifierFlagCapsLock ) != 0 ) != ( ( in_modState & KMOD_CAPS ) != 0 ) )
	{
		IN_SendKey( -1, SC_CAPSLOCK, qtrue );
		IN_SendKey( -1, SC_CAPSLOCK, qfalse );
	}
}

/*
===============
IN_ResetKeyboard

Every key that is down goes up (SDL_ResetKeyboard): the keyboard is gone with the focus or the window.
===============
*/
static void IN_ResetKeyboard( void )
{
	int scancode;

	for( scancode = 0; scancode < SC_NUM; scancode++ )
	{
		if( in_keyState[ scancode ] )
			IN_SendKey( -1, scancode, qfalse );
	}
}

/*
===============
IN_SyncCapsLock

The window got the keyboard: caps lock may have changed meanwhile (SDL sets its state without an event).
===============
*/
static void IN_SyncCapsLock( void )
{
	if( [NSEvent modifierFlags] & NSEventModifierFlagCapsLock )
		in_modState |= KMOD_CAPS;
	else
		in_modState &= ~KMOD_CAPS;
}

/*
===============
IN_TextClient

The window's text input client (the content view, macos_window.m).
===============
*/
static NSView<NSTextInputClient> *IN_TextClient( void )
{
	NSView *view = in_window.contentView;

	return [view conformsToProtocol:@protocol(NSTextInputClient)] ? (NSView<NSTextInputClient> *)view : nil;
}

/*
===============
MacOS_KeyEvent

A key or modifier event from the pump, before AppKit dispatches it (SDL's Cocoa_HandleKeyEvent). A key going down
then also goes through the text input client, which turns it into text (MacOS_InsertText).
===============
*/
void MacOS_KeyEvent( NSEvent *event )
{
	int virtualKey = event.keyCode;
	int scancode;

	if( !in_window )
		return;

	if( ( virtualKey == kVK_ISO_Section || virtualKey == kVK_ANSI_Grave ) && IN_IsISOKeyboard( ) )
		virtualKey = kVK_ISO_Section + kVK_ANSI_Grave - virtualKey;

	scancode = virtualKey < ARRAY_LEN( in_vkToScancode ) ? in_vkToScancode[ virtualKey ] : 0;

	switch( event.type )
	{
		case NSEventTypeKeyDown:
			if( !event.ARepeat )
				IN_UpdateKeymap( );

			IN_SendKey( virtualKey, scancode, qtrue );

			[IN_TextClient( ) interpretKeyEvents:@[ event ]];
			break;

		case NSEventTypeKeyUp:
			IN_SendKey( virtualKey, scancode, qfalse );
			break;

		case NSEventTypeFlagsChanged:
			IN_SendModifiers( (unsigned int)event.modifierFlags );
			break;

		default:
			break;
	}
}

/*
===============
MacOS_InsertText

Text from the text input client: sdl_input.c's SDL_TEXTINPUT. A console character opens the console instead, and
the text of the key that toggled the console is dropped.
===============
*/
void MacOS_InsertText( NSString *text )
{
	const char *c = text.UTF8String;

	// no text event for an unprintable character (SDL_SendKeyboardText)
	if( !c || (unsigned char)*c < ' ' || *c == 127 )
		return;

	if( lastKeyDown == K_CONSOLE )
		return;

	// Quick and dirty UTF-8 to UTF-32 conversion
	while( *c )
	{
		int utf32 = 0;

		if( ( *c & 0x80 ) == 0 )
			utf32 = *c++;
		else if( ( *c & 0xE0 ) == 0xC0 ) // 110x xxxx
		{
			utf32 |= ( *c++ & 0x1F ) << 6;
			utf32 |= ( *c++ & 0x3F );
		}
		else if( ( *c & 0xF0 ) == 0xE0 ) // 1110 xxxx
		{
			utf32 |= ( *c++ & 0x0F ) << 12;
			utf32 |= ( *c++ & 0x3F ) << 6;
			utf32 |= ( *c++ & 0x3F );
		}
		else if( ( *c & 0xF8 ) == 0xF0 ) // 1111 0xxx
		{
			utf32 |= ( *c++ & 0x07 ) << 18;
			utf32 |= ( *c++ & 0x3F ) << 12;
			utf32 |= ( *c++ & 0x3F ) << 6;
			utf32 |= ( *c++ & 0x3F );
		}
		else
		{
			Com_DPrintf( "Unrecognised UTF-8 lead byte: 0x%x\n", (unsigned int)*c );
			c++;
		}

		if( utf32 != 0 )
		{
			if( IN_IsConsoleKey( 0, utf32 ) )
			{
				Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qtrue, 0, NULL );
				Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qfalse, 0, NULL );
			}
			else
				Com_QueueEvent( in_eventTime, SE_CHAR, utf32, 0, 0, NULL );
		}
	}
}

/*
==============================================================================

MOUSE

The mouse works as it did through SDL 2.32 (its Cocoa mouse, read from its binary) and sdl_input.c. While the game
has the mouse (IN_Frame's rules), the cursor is captured as by SDL's relative mode: parked at the window's center,
detached from the mouse and hidden. The movement then comes from
  in_mouseRaw 0: each movement event's deltas cut toward zero, as SDL cut them: the system cursor's deltas, with
                 the system's acceleration, the same SE_MOUSE stream as SDL;
  in_mouseRaw 1: GCMouse's raw deltas (the mouse's own counts, no acceleration), added up off the main thread with
                 their fractions and queued once per frame. The default (the user's choice, docs/METAL_PORT_PLAN.md
                 3.1 #14).
Otherwise the system cursor shows over the window, with the UI's color cursor, and its position is the UI's cursor,
in the drawable's pixels (SDL gave window points: on a 2x display the UI's cursor reached only the top left quarter).
A fullscreen UI keeps the mouse captured and draws its own cursor, moved by the deltas, in pixels too.

The captured movement is read in the pump (MacOS_PumpEvents); the buttons, the wheel and the cursor's movement over
the window come through the content view's responder methods (macos_window.m), as SDL read them.
==============================================================================
*/

// SDL's mouse buttons
#define SDL_BUTTON_LEFT		1
#define SDL_BUTTON_MIDDLE	2
#define SDL_BUTTON_RIGHT	3
#define SDL_BUTTON_X1		4
#define SDL_BUTTON_X2		5

static cvar_t *in_mouse             = NULL;
static cvar_t *in_nograb;
static cvar_t *in_mouseRaw;

static qboolean mouseAvailable = qfalse;
static qboolean mouseActive = qfalse;

static qboolean		in_focused;				// the window has the keyboard (SDL_WINDOW_INPUT_FOCUS)
static qboolean		in_captured;			// the cursor is captured (SDL's relative mode)
static qboolean		in_cursorHidden;		// the capture hid the cursor (CGDisplayHideCursor)
static dispatch_queue_t	in_cursorQueue;		// the capture's WindowServer calls, off the main thread
static qboolean		in_skipMotion;			// the next movement event is dropped (SDL's justEnabledRelative)
static qboolean		in_gobbleMotion;		// the pump drops the movement (IN_GobbleMotionEvents)
static qboolean		in_mouseInside;			// the cursor is over the window (SDL's mouse focus)
static int			in_mouseX, in_mouseY;	// the UI's cursor over the window, in pixels (SDL's mouse->x/y)
static CGFloat		in_pixelRestX, in_pixelRestY;	// a scaled fullscreen UI's movement below a pixel, kept for the next
static int			in_lastX, in_lastY;		// the cursor's last position in points, for in_nograb's deltas
static unsigned int	in_mouseButtons;		// SDL's button state: bit n - 1 is button n
static float		in_wheelY;				// SDL's wheel movement not yet sent
static NSCursor		*in_cursor;				// the UI's color cursor, nil = the arrow

// GCMouse (in_mouseRaw 1): the handlers add up the movement and list the buttons on their own queue, IN_Frame takes
// them
#define MAX_RAW_BUTTONS	64

static dispatch_queue_t	in_rawQueue;
static NSMutableSet		*in_rawMice;				// the mice with handlers
static qboolean			in_rawOn;					// the handlers are installed
static os_unfair_lock	in_rawLock = OS_UNFAIR_LOCK_INIT;
static float			in_rawX, in_rawY;			// under in_rawLock
static long long		in_rawOldest, in_rawNewest;	// under in_rawLock: when the handlers ran, microseconds
static int				in_rawButtons[ MAX_RAW_BUTTONS ];	// under in_rawLock: SDL's button numbers, < 0 = up
static int				in_rawNumButtons;					// under in_rawLock

// cl_latencyStats: the times of the movement that reached the game (Sys_MouseEventMonitor)
static qboolean		in_eventTimes;
static long long	in_eventOldest, in_eventNewest;

static void IN_SendMouseButton( int button, qboolean down );

/*
===============
IN_PixelMap

The game's pixels per window point, per axis, and where its image starts in the view (points from the top left):
the UI's coordinates are pixels. The image is the whole view, but for exclusive fullscreen at another size than the
display's, scaled to the screen (MacOS_GameRect).
===============
*/
static void IN_PixelMap( CGFloat *scaleX, CGFloat *scaleY, CGFloat *left, CGFloat *top )
{
	NSRect rect = MacOS_GameRect( );

	*scaleX = rect.size.width > 0 && cls.glconfig.vidWidth > 0 ? cls.glconfig.vidWidth / rect.size.width : 1;
	*scaleY = rect.size.height > 0 && cls.glconfig.vidHeight > 0 ? cls.glconfig.vidHeight / rect.size.height : 1;
	*left = rect.origin.x;
	*top = in_window.contentView.bounds.size.height - NSMaxY( rect );
}

/*
===============
IN_MotionTime

The OS time of movement that reaches the game, for the latency probe.
===============
*/
static void IN_MotionTime( long long usec )
{
	if( !in_eventTimes || !usec )
		return;

	if( !in_eventOldest || usec < in_eventOldest )
		in_eventOldest = usec;
	if( usec > in_eventNewest )
		in_eventNewest = usec;
}

/*
===============
IN_RawMouseUsed

GCMouse moves the view: captured with in_mouseRaw 1, a mouse found, no UI (a fullscreen UI's cursor moves by the
movement events, like the cursor over a window).
===============
*/
static qboolean IN_RawMouseUsed( void )
{
	return in_rawOn && in_rawMice.count > 0 && !in_guimouse;
}

/*
===============
IN_RawButtonsUsed

GCMouse's buttons are the game's while its handlers are installed (captured, in_mouseRaw 1): they reach the process
~1.8 ms before the button events reach the main thread (session 21, the user's mouse: 0.05 to 3.6 ms over 12 clicks).
The button events then only release (MacOS_MouseButton).
===============
*/
static qboolean IN_RawButtonsUsed( void )
{
	return in_rawOn && in_rawMice.count > 0;
}

/*
===============
IN_WatchRawButton
===============
*/
static void IN_WatchRawButton( GCControllerButtonInput *input, int button, qboolean watch )
{
	if( !watch || button > 32 )
	{
		input.pressedChangedHandler = nil;
		return;
	}

	input.pressedChangedHandler = ^( GCControllerButtonInput *b, float value, BOOL pressed )
	{
		os_unfair_lock_lock( &in_rawLock );
		if( in_rawNumButtons < MAX_RAW_BUTTONS )
			in_rawButtons[ in_rawNumButtons++ ] = pressed ? button : -button;
		os_unfair_lock_unlock( &in_rawLock );
	};
}

/*
===============
IN_WatchRawMouse

Gives a mouse the handlers that add up its movement and list its buttons (left, right, middle, then the auxiliary
ones as SDL's 4, 5, ...), or takes them away. The handlers run on in_rawQueue, a serial queue at the
user-interactive QoS, at the mouse's polling rate.
===============
*/
static void IN_WatchRawMouse( GCMouse *mouse, qboolean watch )
{
	GCMouseInput *input = mouse.mouseInput;
	int i;

	if( !input )
		return;

	IN_WatchRawButton( input.leftButton, SDL_BUTTON_LEFT, watch );
	IN_WatchRawButton( input.rightButton, SDL_BUTTON_RIGHT, watch );
	IN_WatchRawButton( input.middleButton, SDL_BUTTON_MIDDLE, watch );
	for( i = 0; i < (int)input.auxiliaryButtons.count; i++ )
		IN_WatchRawButton( input.auxiliaryButtons[ i ], SDL_BUTTON_X1 + i, watch );

	if( !watch )
	{
		input.mouseMovedHandler = nil;
		[in_rawMice removeObject:mouse];
		return;
	}

	mouse.handlerQueue = in_rawQueue;
	input.mouseMovedHandler = ^( GCMouseInput *m, float deltaX, float deltaY )
	{
		long long now = (long long)( clock_gettime_nsec_np( CLOCK_UPTIME_RAW ) / 1000 );

		os_unfair_lock_lock( &in_rawLock );
		in_rawX += deltaX;
		in_rawY += deltaY;
		if( !in_rawOldest )
			in_rawOldest = now;
		in_rawNewest = now;
		os_unfair_lock_unlock( &in_rawLock );
	};
	[in_rawMice addObject:mouse];
}

/*
===============
IN_DropRawMotion
===============
*/
static void IN_DropRawMotion( void )
{
	os_unfair_lock_lock( &in_rawLock );
	in_rawX = in_rawY = 0;
	in_rawOldest = in_rawNewest = 0;
	os_unfair_lock_unlock( &in_rawLock );
}

/*
===============
IN_InitRawMouse

Starts GameController's mouse discovery once (it finds the mice asynchronously): at the input's start with
in_mouseRaw 1, so the mice are known by the first capture.
===============
*/
static void IN_InitRawMouse( void )
{
	NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
	NSOperationQueue *main = [NSOperationQueue mainQueue];	// run inside the pump

	if( in_rawQueue )
		return;

	in_rawQueue = dispatch_queue_create( "openmohaa.mouse",
		dispatch_queue_attr_make_with_qos_class( DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0 ) );
	in_rawMice = [NSMutableSet set];

	[center addObserverForName:GCMouseDidConnectNotification object:nil queue:main usingBlock:^( NSNotification *n )
	{
		if( in_rawOn )
			IN_WatchRawMouse( n.object, qtrue );
	}];
	[center addObserverForName:GCMouseDidDisconnectNotification object:nil queue:main usingBlock:^( NSNotification *n )
	{
		IN_WatchRawMouse( n.object, qfalse );
	}];

	@autoreleasepool
	{
		(void)GCMouse.mice;
	}
}

/*
===============
IN_RawButtonsFrame

GCMouse's buttons since the last frame, in their order.
===============
*/
static void IN_RawButtonsFrame( void )
{
	int buttons[ MAX_RAW_BUTTONS ];
	int numButtons, i;

	os_unfair_lock_lock( &in_rawLock );
	numButtons = in_rawNumButtons;
	memcpy( buttons, in_rawButtons, numButtons * sizeof( buttons[ 0 ] ) );
	in_rawNumButtons = 0;
	os_unfair_lock_unlock( &in_rawLock );

	if( !IN_RawButtonsUsed( ) )
		return;

	for( i = 0; i < numButtons; i++ )
		IN_SendMouseButton( abs( buttons[ i ] ), buttons[ i ] > 0 );
}

/*
===============
IN_RawMouse

GCMouse's handlers are installed only while its movement may be used: captured, with in_mouseRaw 1.
===============
*/
static void IN_RawMouse( qboolean on )
{
	if( on == in_rawOn )
		return;

	// the buttons pressed or released while captured still count
	if( !on )
		IN_RawButtonsFrame( );

	in_rawOn = on;
	IN_InitRawMouse( );

	@autoreleasepool
	{
		for( GCMouse *mouse in GCMouse.mice )
			IN_WatchRawMouse( mouse, on );
	}

	IN_DropRawMotion( );
}

/*
===============
IN_RawMouseFrame

GCMouse's buttons and movement since the last frame: the buttons in their order, then the movement's whole counts
as one SE_MOUSE (the fractions stay for the next frame). GameController's y points up.
===============
*/
static void IN_RawMouseFrame( void )
{
	qboolean used;
	float x, y;
	int dx = 0, dy = 0;
	long long oldest, newest;

	if( !in_rawOn )
		return;

	IN_RawButtonsFrame( );

	used = IN_RawMouseUsed( );

	os_unfair_lock_lock( &in_rawLock );
	x = in_rawX;
	y = in_rawY;
	oldest = in_rawOldest;
	newest = in_rawNewest;
	if( used )
	{
		dx = (int)x;
		dy = (int)y;
		in_rawX = x - dx;
		in_rawY = y - dy;
	}
	else
		in_rawX = in_rawY = 0;
	in_rawOldest = in_rawNewest = 0;
	os_unfair_lock_unlock( &in_rawLock );

	if( !dx && !dy )
		return;

	IN_MotionTime( oldest );
	IN_MotionTime( newest );
	Com_QueueEvent( in_eventTime, SE_MOUSE, dx, -dy, 0, NULL );
}

/*
===============
MacOS_MouseMotion

A movement event, from the pump (SDL's Cocoa_HandleMouseEvent, then sdl_input.c's SDL_MOUSEMOTION). Only the
captured mouse moves the game here; the cursor's movement over the window comes through the content view
(MacOS_MouseMoved). SDL's deltas are floats cut toward zero per event, so movement below a point per event is
lost: in_mouseRaw 1 reads GCMouse instead.
===============
*/
void MacOS_MouseMotion( NSEvent *event )
{
	CGFloat scaleX = 1, scaleY = 1, left, top;
	int dx, dy;

	if( !in_window )
		return;

	// the first movement after a capture is dropped
	if( in_skipMotion )
	{
		in_skipMotion = qfalse;
		in_pixelRestX = in_pixelRestY = 0;
		return;
	}

	if( in_gobbleMotion || !in_captured || !mouseActive )
		return;

	// movement over the title bar
	if( event.window && !NSMouseInRect( event.locationInWindow, event.window.contentView.frame, NO ) )
		return;

	if( in_guimouse )
		IN_PixelMap( &scaleX, &scaleY, &left, &top );	// a fullscreen UI's cursor, in pixels
	else if( IN_RawMouseUsed( ) )
		return;

	IN_MotionTime( (long long)( event.timestamp * 1e6 ) );

	if( scaleX != 1 || scaleY != 1 )
	{
		// exclusive fullscreen scaled to the screen (or a 2x display): the UI's cursor moves in the game's pixels,
		// and cut per event, a point's movement at 0.75 pixels a point would be lost: the fractions are kept
		CGFloat x = event.deltaX * scaleX + in_pixelRestX, y = event.deltaY * scaleY + in_pixelRestY;

		dx = (int)x;
		dy = (int)y;
		in_pixelRestX = x - dx;
		in_pixelRestY = y - dy;
	}
	else
	{
		dx = (int)(float)( event.deltaX * scaleX );
		dy = (int)(float)( event.deltaY * scaleY );
	}
	if( !dx && !dy )
		return;

	Com_QueueEvent( in_eventTime, SE_MOUSE, dx, dy, 0, NULL );
}

/*
===============
IN_MouseAt

The cursor is at a point of the window (window coordinates; SDL_SendMouseMotion with absolute coordinates): the
UI's cursor goes there. Leaving the window it stops at the edge, unless a button is down (SDL then captures the
mouse and follows it outside). With in_nograb in a window the game takes the movement as deltas, in points.
===============
*/
static void IN_MouseAt( NSPoint point, long long usec )
{
	NSSize size = in_window.contentView.bounds.size;
	CGFloat scaleX, scaleY, left, top;
	int width = (int)size.width, height = (int)size.height;
	int x = (int)point.x, y = (int)( height - point.y );	// SDL's position: points cut toward zero, y down
	int px, py;
	qboolean followed = in_mouseButtons != 0 && in_focused;
	qboolean clamp = !followed;
	int dx, dy;

	IN_PixelMap( &scaleX, &scaleY, &left, &top );
	px = (int)( ( point.x - left ) * scaleX );
	py = (int)( ( height - point.y - top ) * scaleY );

	if( !followed && ( x < 0 || y < 0 || x >= width || y >= height ) )
	{
		if( !in_mouseInside )
			return;
		in_mouseInside = qfalse;	// the last position, at the edge
	}
	else
		in_mouseInside = qtrue;

	if( clamp )
	{
		px = px < 0 ? 0 : px >= cls.glconfig.vidWidth ? cls.glconfig.vidWidth - 1 : px;
		py = py < 0 ? 0 : py >= cls.glconfig.vidHeight ? cls.glconfig.vidHeight - 1 : py;
	}
	in_mouseX = px;
	in_mouseY = py;

	dx = x - in_lastX;
	dy = y - in_lastY;
	in_lastX = x;
	in_lastY = y;

	if( mouseActive && !in_captured && !in_gobbleMotion && ( dx || dy ) )
	{
		IN_MotionTime( usec );
		Com_QueueEvent( in_eventTime, SE_MOUSE, dx, dy, 0, NULL );
	}
}

/*
===============
MacOS_MouseMoved

The cursor moved over the window, or was dragged (SDL's -[Cocoa_WindowListener mouseMoved:]).
===============
*/
void MacOS_MouseMoved( NSEvent *event )
{
	if( !in_window || in_captured )
		return;

	IN_MouseAt( event.locationInWindow, (long long)( event.timestamp * 1e6 ) );
}

/*
===============
IN_SendMouseButton

SDL_SendMouseButton, then sdl_input.c's SDL_MOUSEBUTTONDOWN/UP: a button that is already down or up sends nothing.
===============
*/
static void IN_SendMouseButton( int button, qboolean down )
{
	unsigned int bit = 1u << ( button - 1 );
	keyNum_t key;

	if( ( ( in_mouseButtons & bit ) != 0 ) == down )
		return;

	in_mouseButtons ^= bit;

	switch( button )
	{
		case SDL_BUTTON_LEFT:   key = K_MOUSE1;     break;
		case SDL_BUTTON_MIDDLE: key = K_MOUSE3;     break;
		case SDL_BUTTON_RIGHT:  key = K_MOUSE2;     break;
		case SDL_BUTTON_X1:     key = K_MOUSE4;     break;
		case SDL_BUTTON_X2:     key = K_MOUSE5;     break;
		default:                key = K_AUX1 + ( button - SDL_BUTTON_X2 + 1 ) % 16; break;
	}
	Com_QueueEvent( in_eventTime, SE_KEY, key, down, 0, NULL );
}

/*
===============
MacOS_MouseButton

A button went down or up over the window (SDL's -[Cocoa_WindowListener mouseDown:] and mouseUp:). AppKit numbers
left, right, middle 0, 1, 2; SDL's buttons are 1 (left), 3 (right), 2 (middle), then AppKit's number + 1.
===============
*/
void MacOS_MouseButton( NSEvent *event, qboolean down )
{
	int button;

	if( !in_window )
		return;

	// a press over the title bar isn't the game's
	if( down && event.window && !NSMouseInRect( event.locationInWindow, event.window.contentView.frame, NO ) )
		return;

	// GCMouse presses the buttons first (a release still goes through: it may be a mouse GCMouse doesn't see)
	if( down && IN_RawButtonsUsed( ) )
		return;

	switch( event.buttonNumber )
	{
		case 0:		button = SDL_BUTTON_LEFT;	break;
		case 1:		button = SDL_BUTTON_RIGHT;	break;
		case 2:		button = SDL_BUTTON_MIDDLE;	break;
		default:	button = (byte)( event.buttonNumber + 1 ); break;
	}

	if( button >= 1 && button <= 32 )
		IN_SendMouseButton( button, down );
}

/*
===============
IN_ResetMouseButtons

Every button that is down goes up: the window goes away. (SDL lost these releases with its event queue at
vid_restart, and then ignored the real releases: the buttons stayed down in the client.)
===============
*/
static void IN_ResetMouseButtons( void )
{
	int button;

	for( button = 1; button <= 32; button++ )
		IN_SendMouseButton( button, qfalse );
}

/*
===============
MacOS_MouseWheel

The wheel (SDL's Cocoa_HandleMouseWheel and SDL_SendMouseWheel, then sdl_input.c's SDL_MOUSEWHEEL). A wheel's
notch is at least a whole step, a trackpad's fine deltas add up to whole steps (a change of direction starts
over), and each event with steps presses and releases K_MWHEELUP or K_MWHEELDOWN once. The direction is the
system's (natural scrolling included), as with SDL.
===============
*/
void MacOS_MouseWheel( NSEvent *event )
{
	CGFloat delta = event.deltaY;
	float y;
	int steps;

	if( !in_window )
		return;

	if( !event.hasPreciseScrollingDeltas )
	{
		if( delta > 0 )
			delta = ceil( delta );
		else if( delta < 0 )
			delta = floor( delta );
	}
	y = (float)delta;

	if( y > 0 )
	{
		if( in_wheelY < 0 )
			in_wheelY = 0;
	}
	else if( y < 0 )
	{
		if( in_wheelY > 0 )
			in_wheelY = 0;
	}
	else
		return;

	in_wheelY += y;
	steps = in_wheelY > 0 ? (int)floor( in_wheelY ) : in_wheelY < 0 ? (int)ceil( in_wheelY ) : 0;
	in_wheelY -= steps;

	if( steps > 0 )
	{
		Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELUP, qtrue, 0, NULL );
		Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELUP, qfalse, 0, NULL );
	}
	else if( steps < 0 )
	{
		Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELDOWN, qtrue, 0, NULL );
		Com_QueueEvent( in_eventTime, SE_KEY, K_MWHEELDOWN, qfalse, 0, NULL );
	}
}

/*
===============
IN_WindowCenter

The center of the window's content in global display coordinates (points, the main display's top left at 0,0),
from the window's position and size in whole points, as SDL computed it.
===============
*/
static CGPoint IN_WindowCenter( void )
{
	NSRect frame = [in_window convertRectToScreen:in_window.contentView.frame];
	CGFloat top = NSMaxY( NSScreen.screens.firstObject.frame ) - NSMaxY( frame );
	int x = (int)NSMinX( frame ), y = (int)top, width = (int)NSWidth( frame ), height = (int)NSHeight( frame );

	return CGPointMake( (float)( x + width / 2 ), (float)( y + height / 2 ) );
}

/*
===============
IN_UpdateCursor

The UI's cursor over the window changed, or it shows again after a capture without the keyboard.
===============
*/
static void IN_UpdateCursor( void )
{
	if( in_window )
		[in_window invalidateCursorRectsForView:in_window.contentView];
}

/*
===============
IN_WarpCursor

The cursor goes to a point in global display points, unless it is there already: a warp is a synchronous
WindowServer call (0.1-1.8 ms in a window, the look 2 us), and the cursor a capture parked at the window's center
is still there at the release. qfalse = no warp.
===============
*/
static qboolean IN_WarpCursor( CGPoint point )
{
	CGEventRef event = CGEventCreate( NULL );
	qboolean there = event && CGPointEqualToPoint( CGEventGetLocation( event ), point );

	if( event )
		CFRelease( event );
	if( there )
		return qfalse;

	CGWarpMouseCursorPosition( point );
	return qtrue;
}

/*
===============
IN_Capture

The cursor is captured (SDL's relative mode, Cocoa_SetRelativeMouseMode): it goes to the window's center when the
window has the keyboard, then it is detached from the mouse (the call right after the warp also ends the warp's
short freeze of the mouse) and hidden, and the first movement event after it is dropped, as SDL drops it.
Released, the cursor follows the mouse again and shows, as the UI's cursor at once. The capture leaves the cursor
rects alone (SDL also put an invisible cursor there): the hide is enough, and rebuilding them cost 0.6-1.3 ms in a
window, up to 2.5 ms after a load. The detach and the hide wait for the WindowServer (0.3-1 ms in a window), so
they run on in_cursorQueue, in order, and the frame goes on; the cursor is hidden before the next frame starts.
The warp stays here: the first movement after it is the one dropped. The release first waits for that queue, then
sets the UI's cursor while the cursor is still hidden (0.3 ms, ~1 ms once it shows; the rects' rebuild took
1.2-1.5) and shows it (docs/METAL_PORT_PLAN.md, Phase 5 "Session 38 result", "Session 39 result").
===============
*/
static void IN_Capture( qboolean capture )
{
	if( capture == in_captured || !in_window )
		return;

	in_captured = capture;

	@autoreleasepool
	{
		if( capture )
		{
			qboolean hide = in_focused && !in_cursorHidden;

			if( in_focused )
			{
				IN_WarpCursor( IN_WindowCenter( ) );
				in_skipMotion = qtrue;
			}
			if( hide )
				in_cursorHidden = qtrue;

			dispatch_async( in_cursorQueue, ^{
				CGAssociateMouseAndMouseCursorPosition( false );
				if( hide )
					CGDisplayHideCursor( kCGNullDirectDisplay );
			} );
		}
		else
		{
			dispatch_sync( in_cursorQueue, ^{
				CGAssociateMouseAndMouseCursorPosition( true );
			} );

			if( in_focused )
				[MacOS_Cursor( ) set];

			if( in_cursorHidden )
			{
				CGDisplayShowCursor( kCGNullDirectDisplay );
				in_cursorHidden = qfalse;
			}

			// without the keyboard the cursor is another window's; the rects set it when the window gets it back
			if( !in_focused )
				IN_UpdateCursor( );
		}
	}

	IN_RawMouse( capture && in_mouseRaw->integer );
}

/*
===============
IN_WarpToCenter

The released cursor goes to the window's center, which becomes the UI's cursor (sdl_input.c). SDL took the center
from the drawable's pixels as points: on a 2x display the cursor went to the window's bottom right corner.
===============
*/
static void IN_WarpToCenter( void )
{
	NSSize size = in_window.contentView.bounds.size;
	CGFloat scaleX, scaleY, left, top;
	int x = (int)size.width / 2, y = (int)size.height / 2;

	@autoreleasepool
	{
		// the associate call ends the warp's freeze of the mouse
		if( IN_WarpCursor( IN_WindowCenter( ) ) )
			CGAssociateMouseAndMouseCursorPosition( true );
	}

	IN_PixelMap( &scaleX, &scaleY, &left, &top );
	in_mouseX = (int)( ( x - left ) * scaleX );
	in_mouseY = (int)( ( y - top ) * scaleY );
	in_lastX = x;
	in_lastY = y;
	in_mouseInside = qtrue;
}

/*
===============
IN_GobbleMotionEvents

The pending events are handled now, without their movement: a capture change drops it, GCMouse's too.
===============
*/
static void IN_GobbleMotionEvents( void )
{
	in_gobbleMotion = qtrue;
	MacOS_PumpEvents( );
	in_gobbleMotion = qfalse;

	IN_DropRawMotion( );
}

/*
===============
IN_ActivateMouse
===============
*/
static void IN_ActivateMouse( qboolean isFullscreen )
{
	if( !mouseAvailable || !in_window )
		return;

	if( !mouseActive )
	{
		IN_Capture( isFullscreen || !in_nograb->integer );

		IN_GobbleMotionEvents( );
	}

	// in_nograb makes no sense in fullscreen mode
	if( !isFullscreen )
	{
		if( in_nograb->modified || !mouseActive )
		{
			IN_Capture( !in_nograb->integer );

			in_nograb->modified = qfalse;
		}
	}

	mouseActive = qtrue;
}

/*
===============
IN_DeactivateMouse
===============
*/
static void IN_DeactivateMouse( void )
{
	if( !mouseAvailable || !in_window )
		return;

	if( mouseActive )
	{
		IN_GobbleMotionEvents( );

		IN_Capture( qfalse );

		// Don't warp the mouse unless the cursor is within the window
		if( in_focused )
			IN_WarpToCenter( );

		mouseActive = qfalse;
	}
}

/*
===============
MacOS_ReleaseMouse

A dialog is coming (MacOS_DialogBegin): the cursor goes back to the system where it is parked, as on a focus loss,
without a pump (a dialog can come from anywhere, even from inside one). IN_Frame captures it again afterwards.
===============
*/
void MacOS_ReleaseMouse( void )
{
	IN_Capture( qfalse );
	mouseActive = qfalse;
}

/*
===============
MacOS_Cursor

The cursor over the content view (its cursor rectangle): the UI's or the arrow (hidden while captured).
===============
*/
NSCursor *MacOS_Cursor( void )
{
	return in_cursor ?: [NSCursor arrowCursor];
}

/*
==============
Sys_MouseEventMonitor

cl_latencyStats: while on, the input layer records the OS times of the mouse movement that reaches the game: the
movement events' timestamps (NSEvent.timestamp: when the event entered the system, on the mach_absolute_time
clock), or with GCMouse when its handler ran.
==============
*/
void Sys_MouseEventMonitor( qboolean on )
{
	in_eventTimes = on;
	in_eventOldest = in_eventNewest = 0;
}

/*
==============
Sys_TakeMouseEventTimes

The oldest and newest mouse movement that reached the game since the last call (qfalse if none).
==============
*/
qboolean Sys_TakeMouseEventTimes( long long *oldest, long long *newest )
{
	qboolean any = in_eventNewest != 0;

	if( oldest )
		*oldest = in_eventOldest;
	if( newest )
		*newest = in_eventNewest;
	in_eventOldest = in_eventNewest = 0;
	return any;
}

/*
==============================================================================

THE WINDOW

==============================================================================
*/

/*
===============
IN_WindowResized

A window's size or backing scale changed: rebuild at the new size once the user stops dragging. glConfig holds
the drawable's pixels, r_customwidth/height the window's points (as with SDL).
===============
*/
static void IN_WindowResized( void )
{
	NSSize size = in_window.contentView.bounds.size;
	CGFloat scale = in_window.backingScaleFactor;
	int width = (int)size.width, height = (int)size.height;

	// ignore this event on fullscreen
	if( cls.glconfig.isFullscreen )
	{
		return;
	}

	// check if size actually changed
	if( cls.glconfig.vidWidth == (int)lround( width * scale ) && cls.glconfig.vidHeight == (int)lround( height * scale ) )
	{
		return;
	}

	Cvar_SetValue( "r_customwidth", width );
	Cvar_SetValue( "r_customheight", height );
	Cvar_Set( "r_mode", "-1" );

	// Wait until user stops dragging for 1 second, so
	// we aren't constantly recreating the GL context while
	// he tries to drag...
	vidRestartTime = Sys_Milliseconds( ) + 1000;
}

/*
===============
IN_WindowFocused

The window got or lost the keyboard. Losing it releases every key that is down and drops any text being composed
(SDL resets its keyboard and stops text input), and gives the cursor back to the system at once, where it was
parked (SDL's windowDidResignKey); IN_Frame captures it again when the window has the keyboard. Getting it back
reads where the cursor is (SDL's windowDidBecomeKey).
===============
*/
static void IN_WindowFocused( qboolean focused )
{
	in_focused = focused;

	if( focused )
	{
		IN_SyncCapsLock( );

		if( !in_captured )
			IN_MouseAt( in_window.mouseLocationOutsideOfEventStream, 0 );
	}
	else
	{
		NSView<NSTextInputClient> *client = IN_TextClient( );

		IN_ResetKeyboard( );
		[client.inputContext discardMarkedText];
		[client unmarkText];

		IN_Capture( qfalse );
		mouseActive = qfalse;
		in_mouseInside = qfalse;
	}

	Cvar_SetValue( "com_unfocused", !focused );
}

/*
===============
IN_ObserveWindow

The window's events reach the input layer as SDL's window events did: during the pump, on the main thread.
===============
*/
static void IN_ObserveWindow( void )
{
	NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
	NSOperationQueue *queue = nil;	// run on the posting thread, inside the pump

	in_windowObservers = @[
		[center addObserverForName:NSWindowDidBecomeKeyNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { IN_WindowFocused( qtrue ); }],
		[center addObserverForName:NSWindowDidResignKeyNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { IN_WindowFocused( qfalse ); }],
		[center addObserverForName:NSWindowDidMiniaturizeNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { Cvar_SetValue( "com_minimized", 1 ); }],
		[center addObserverForName:NSWindowDidDeminiaturizeNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { Cvar_SetValue( "com_minimized", 0 ); }],
		[center addObserverForName:NSWindowDidResizeNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { IN_WindowResized(); }],
		[center addObserverForName:NSWindowDidChangeBackingPropertiesNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { IN_WindowResized(); }],
	];
}

/*
===============
IN_StopObservingWindow
===============
*/
static void IN_StopObservingWindow( void )
{
	for( id observer in in_windowObservers )
		[[NSNotificationCenter defaultCenter] removeObserver:observer];
	in_windowObservers = nil;
}

/*
===============
IN_ProcessEvents
===============
*/
static void IN_ProcessEvents( void )
{
	if( !MacOS_AppStarted( ) )
		return;

	CL_LatencyPump( );

	MacOS_PumpEvents( );

	IN_RawMouseFrame( );

	if( MacOS_TakeQuitRequest( ) )
		Cbuf_ExecuteText( EXEC_NOW, "quit Closed window\n" );
}

/*
===============
IN_Frame
===============
*/
void IN_Frame( void )
{
	qboolean loading;

	// If not DISCONNECTED (main menu) or ACTIVE (in game), we're loading
	loading = ( clc.state != CA_DISCONNECTED && clc.state != CA_ACTIVE );

	// update isFullscreen since it might of changed since the last vid_restart
	cls.glconfig.isFullscreen = Cvar_VariableIntegerValue( "r_fullscreen" ) != 0;

	if( !cls.glconfig.isFullscreen && ( Key_GetCatcher( ) & (KEYCATCH_CONSOLE|KEYCATCH_UI) ) )
	{
		// Console is down in windowed mode
		IN_DeactivateMouse( );
	}
	else if( !cls.glconfig.isFullscreen && loading )
	{
		// Loading in windowed mode
		IN_DeactivateMouse( );
	}
	else if( !in_focused )
	{
		// Window not got focus
		IN_DeactivateMouse( );
	}
	else
		IN_ActivateMouse( cls.glconfig.isFullscreen );

	// GCMouse while captured with in_mouseRaw 1 (in_mouseRaw may change any time)
	IN_RawMouse( in_captured && in_mouseRaw->integer );

	IN_ProcessEvents( );

	// Set event time for next frame to earliest possible time an event could happen
	in_eventTime = Sys_Milliseconds( );

	// In case we had to delay actual restart of video system
	if( ( vidRestartTime != 0 ) && ( vidRestartTime < Sys_Milliseconds( ) ) )
	{
		vidRestartTime = 0;
		Cbuf_AddText( "vid_restart\n" );
	}
}

/*
===============
IN_Init
===============
*/
void IN_Init( void *windowData )
{
	if( !MacOS_AppStarted( ) )
	{
		Com_Error( ERR_FATAL, "IN_Init called before the window exists" );
		return;
	}

	in_window = (__bridge NSWindow *)windowData;

	Com_DPrintf( "\n------- Input Initialization -------\n" );

	in_keyboardDebug = Cvar_Get( "in_keyboardDebug", "0", CVAR_ARCHIVE );

	// mouse variables
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );
	in_nograb = Cvar_Get( "in_nograb", "0", CVAR_ARCHIVE );
	// 1 = GCMouse's raw deltas (no acceleration; the default, the user's choice), 0 = the system cursor's deltas,
	// as with SDL
	in_mouseRaw = Cvar_Get( "in_mouseRaw", "1", CVAR_ARCHIVE );

	// no gamepads or joysticks (the user's decision, 2026-10-18): in_joystick and joy_threshold are SDL's only

	IN_UpdateKeymap( );
	IN_SyncCapsLock( );

	IN_ObserveWindow( );

	if( !in_cursorQueue )
		in_cursorQueue = dispatch_queue_create( "openmohaa.cursor",
			dispatch_queue_attr_make_with_qos_class( DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0 ) );

	// in_restart releases the mouse (before in_mouse 0 can keep SDL's capture on), IN_Frame captures it again
	in_focused = in_window.keyWindow;
	IN_DeactivateMouse( );
	mouseAvailable = ( in_mouse->value != 0 );

	if( in_mouseRaw->integer )
		IN_InitRawMouse( );

	Cvar_SetValue( "com_unfocused",	!in_window.keyWindow );
	Cvar_SetValue( "com_minimized", in_window.miniaturized );

	Com_DPrintf( "------------------------------------\n" );
}

/*
===============
IN_Shutdown

The window goes away (vid_restart or quit): the mouse is released, and the keys and buttons that are down go up.
SDL queued the same key releases when it destroyed its window, but lost them with its event queue at vid_restart,
which left them down in the client.
===============
*/
void IN_Shutdown( void )
{
	IN_DeactivateMouse( );
	mouseAvailable = qfalse;

	IN_ResetKeyboard( );
	IN_ResetMouseButtons( );

	IN_StopObservingWindow( );

	in_window = nil;
}

/*
===============
IN_Restart

in_restart keeps the window and the keyboard's state; the mouse is released and captured again.
===============
*/
void IN_Restart( void )
{
	IN_StopObservingWindow( );
	IN_Init( (__bridge void *)in_window );
}

/*
===============
IN_GetMousePosition

The UI's cursor over the window, in the drawable's pixels.
===============
*/
void IN_GetMousePosition( int *x, int *y )
{
	*x = in_mouseX;
	*y = in_mouseY;
}

/*
===============
IN_SetCursorFromImage

The UI's color cursor over the window from its RGBA pixels, hotspot at the top left, as SDL made it (Cocoa_CreateImage:
alpha premultiplied, the image's size in points). The pixels are copied, so they are freed here (SDL kept pointing
at them and never freed them).
===============
*/
qboolean IN_SetCursorFromImage( const byte *pic, int width, int height, pCursorFree cursorFreeFn )
{
	NSBitmapImageRep *bitmap;

	IN_FreeCursor( );

	@autoreleasepool
	{
		bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:width pixelsHigh:height
			bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
			bytesPerRow:4 * width bitsPerPixel:32];
		if( bitmap )
		{
			byte *pixels = bitmap.bitmapData;
			NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize( width, height )];
			int i;

			memcpy( pixels, pic, 4 * width * height );
			for( i = 0; i < width * height; i++, pixels += 4 )
			{
				pixels[ 0 ] = pixels[ 0 ] * pixels[ 3 ] / 255;
				pixels[ 1 ] = pixels[ 1 ] * pixels[ 3 ] / 255;
				pixels[ 2 ] = pixels[ 2 ] * pixels[ 3 ] / 255;
			}

			[image addRepresentation:bitmap];
			in_cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
		}
	}

	cursorFreeFn( (byte *)pic );
	IN_UpdateCursor( );

	return bitmap != nil;
}

/*
===============
IN_FreeCursor
===============
*/
void IN_FreeCursor( void )
{
	in_cursor = nil;
	IN_UpdateCursor( );
}

/*
===============
IN_IsCursorActive

The mouse is captured (the UI then draws its own cursor).
===============
*/
qboolean IN_IsCursorActive( void )
{
	return in_captured;
}
