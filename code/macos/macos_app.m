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
// macos_app.m -- the native application: NSApplication without [NSApp run], its menu bar, quitting,
// the event pump and the clipboard.
//
// The engine owns the loop. Com_Frame pumps the events once per frame from IN_Frame, after the frame limiter's
// sleep and right before the input is sampled, where SDL pumped them (docs/METAL_PORT_PLAN.md 4b). The pump never
// waits.

#include "macos_local.h"
#include "../qcommon/qcommon.h"
#include "../sys/sys_local.h"

@interface MacOSAppDelegate : NSObject <NSApplicationDelegate>
@end

static MacOSAppDelegate	*mac_appDelegate;
static id<NSObject>		mac_displayActivity;
static qboolean			mac_quitRequested;

@implementation MacOSAppDelegate

// Cmd+Q, the close button (macos_window.m) and a quit from the system (logout) end up here. The engine quits
// by itself, from IN_Frame, through Sys_Quit.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
	mac_quitRequested = qtrue;
	return NSTerminateCancel;
}

// ApplePersistenceIgnoreState already keeps macOS from restoring windows; this only silences AppKit's warning
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app
{
	return YES;
}

@end

/*
===============
MacOS_CreateMenu

The application menu: Hide, Hide Others, Show All, Quit.
===============
*/
static void MacOS_CreateMenu( void )
{
	NSString *name = [NSRunningApplication currentApplication].localizedName ?: @CLIENT_WINDOW_TITLE;
	NSMenu *bar = [[NSMenu alloc] init];
	NSMenu *menu = [[NSMenu alloc] initWithTitle:name];
	NSMenuItem *item;

	[menu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
	item = [menu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
	item.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
	[menu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
	[menu addItem:[NSMenuItem separatorItem]];
	[menu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];

	item = [[NSMenuItem alloc] init];
	item.submenu = menu;
	[bar addItem:item];
	NSApp.mainMenu = bar;
}

/*
===============
MacOS_InitApp

Creates the application once, before the first window (MTLimp_Init); vid_restart keeps it.
===============
*/
void MacOS_InitApp( void )
{
	if( mac_appDelegate )
		return;

	@autoreleasepool
	{
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		// the defaults SDL registers: a held key repeats instead of opening the accent popup, no momentum
		// scrolling, no windows restored at the next start
		[[NSUserDefaults standardUserDefaults] registerDefaults:@{
			@"ApplePressAndHoldEnabled": @NO,
			@"AppleMomentumScrollSupported": @NO,
			@"ApplePersistenceIgnoreState": @YES }];

		MacOS_CreateMenu();

		mac_appDelegate = [[MacOSAppDelegate alloc] init];
		NSApp.delegate = mac_appDelegate;
		[NSApp finishLaunching];
		[NSApp activateIgnoringOtherApps:YES];

		// the display stays on while the game runs (SDL's screen saver suspension)
		mac_displayActivity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityIdleDisplaySleepDisabled
			reason:@"game"];
	}
}

/*
===============
MacOS_AppStarted
===============
*/
qboolean MacOS_AppStarted( void )
{
	return mac_appDelegate != nil;
}

/*
===============
MacOS_PumpEvents

Handles every pending event and returns at once (a nil date is distantPast). Running the run loop also serves the
main dispatch queue.

The input layer reads the key events here, before AppKit dispatches them, as SDL did: AppKit drops some of them on
the way to the window (key-ups with Cmd held, Cmd+Tab). sendEvent: still runs for them, so Cmd key equivalents reach
the menu.
The mouse's movement is read here too (the captured mouse's deltas); its buttons, the wheel and the cursor's
position reach the content view through sendEvent:, as SDL read them.
===============
*/
void MacOS_PumpEvents( void )
{
	@autoreleasepool
	{
		NSEvent *event;

		while( ( event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode
			dequeue:YES] ) != nil )
		{
			switch( event.type )
			{
				case NSEventTypeKeyDown:
					MacOS_KeyEvent( event );
					break;

				case NSEventTypeKeyUp:
				case NSEventTypeFlagsChanged:
					MacOS_KeyEvent( event );
					break;

				case NSEventTypeMouseMoved:
				case NSEventTypeLeftMouseDragged:
				case NSEventTypeRightMouseDragged:
				case NSEventTypeOtherMouseDragged:
					MacOS_MouseMotion( event );
					break;

				default:
					break;
			}

			[NSApp sendEvent:event];
		}
	}
}

/*
===============
MacOS_TakeQuitRequest
===============
*/
qboolean MacOS_TakeQuitRequest( void )
{
	qboolean quit = mac_quitRequested;

	mac_quitRequested = qfalse;
	return quit;
}

/*
===============
MacOS_GetClipboardText
===============
*/
char *MacOS_GetClipboardText( void )
{
	@autoreleasepool
	{
		const char *text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString].UTF8String;

		return text ? strdup( text ) : NULL;
	}
}

/*
===============
MacOS_SetClipboardText
===============
*/
void MacOS_SetClipboardText( const char *text )
{
	@autoreleasepool
	{
		NSString *string = [NSString stringWithUTF8String:text];
		NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];

		if( string )
		{
			[pasteboard clearContents];
			[pasteboard setString:string forType:NSPasteboardTypeString];
		}
	}
}
