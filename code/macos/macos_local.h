/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA Metal port contributors

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// macos_local.h -- between the files of the native AppKit layer, which replaced SDL2 (Phase 6):
// the application and its event pump (macos_app.m), the window (macos_window.m, the renderer's side) and input
// (macos_input.m, the client's side).

#ifndef __MACOS_LOCAL_H__
#define __MACOS_LOCAL_H__

#import <AppKit/AppKit.h>

#include "../qcommon/q_shared.h"

// macos_app.m
void		MacOS_InitApp( void );				// the application, created once (before the first window)
qboolean	MacOS_AppStarted( void );
void		MacOS_PumpEvents( void );			// handles every pending event, never waits (IN_Frame)
qboolean	MacOS_TakeQuitRequest( void );		// Cmd+Q, the close button or a quit from the system since the last call

// macos_input.m
void		MacOS_KeyEvent( NSEvent *event );		// a key or modifier event, from the pump before AppKit dispatches it
void		MacOS_InsertText( NSString *text );	// text from the window's text input client (the content view)
void		MacOS_MouseMotion( NSEvent *event );	// a mouse movement, from the pump before AppKit dispatches it
void		MacOS_MouseMoved( NSEvent *event );	// the cursor moved over the content view (from its responder methods)
void		MacOS_MouseButton( NSEvent *event, qboolean down );	// a button, from the content view
void		MacOS_MouseWheel( NSEvent *event );	// the wheel, from the content view
NSCursor	*MacOS_Cursor( void );				// the cursor over the content view
void		MacOS_ReleaseMouse( void );			// a dialog is coming: the cursor is the system's until IN_Frame

// macos_window.m
NSRect		MacOS_GameRect( void );				// the game's image in the content view, in points (the whole view but for scaled exclusive fullscreen)

#endif
