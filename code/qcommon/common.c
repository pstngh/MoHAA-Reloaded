/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

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
// common.c -- misc functions used in client and server

#include "q_shared.h"
#include "qcommon.h"
#include "q_version.h"
#include <setjmp.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/stat.h> // umask
#else
#include <winsock.h>
#endif

// FPS
#ifdef __cplusplus
#  include <chrono>
#endif

#include <time.h>

#include "../corepp/tiki.h"

#ifndef DEDICATED
#  include "../uilib/ui_public.h"
#endif

#include "../gamespy/q_gamespy.h"

qboolean CL_FinishedIntro(void);

#ifdef __cplusplus
extern "C" {
#endif

cvar_t *sv_scriptfiles;
cvar_t *g_scriptcheck;
cvar_t *g_showopcodes;

int demo_protocols[] =
{ 0, 0 }; // the first value of the array will be replaced by com_protocol

#define MAX_NUM_ARGVS	50

int		com_argc;
char	*com_argv[MAX_NUM_ARGVS+1];

jmp_buf abortframe;		// an ERR_DROP occured, exit the entire frame


FILE *debuglogfile;
static fileHandle_t pipefile;
static fileHandle_t logfile;
fileHandle_t	com_journalFile;			// events are written here
fileHandle_t	com_journalDataFile;		// config files are written here

cvar_t	*paused;
cvar_t	*config;
cvar_t	*fps;
cvar_t	*fps_location;
cvar_t	*com_speeds;
cvar_t	*developer;
cvar_t	*com_dedicated;
cvar_t	*com_timescale;
cvar_t	*com_fixedtime;
cvar_t	*com_dropsim;		// 0.0 to 1.0, simulated packet drops
cvar_t	*com_journal;
cvar_t	*com_maxfps;
cvar_t	*com_altivec;
cvar_t	*com_timedemo;
cvar_t	*com_sv_running;
cvar_t	*com_cl_running;
cvar_t	*com_viewlog;
cvar_t	*com_logfile;		// 1 = buffer log, 2 = flush after each print
cvar_t	*com_logfile_timestamps;
cvar_t	*com_pipefile;
cvar_t	*com_showtrace;
cvar_t	*com_shortversion;
cvar_t	*com_version;
cvar_t	*autopaused;
cvar_t	*deathmatch;
cvar_t	*com_blood;
cvar_t	*com_buildScript;	// for automated data building scripts
cvar_t	*com_radar_range;
cvar_t	*console_spam_filter;
cvar_t	*com_introPlayed;
cvar_t  *cl_packetdelay;
cvar_t  *sv_packetdelay;
cvar_t	*com_cameraMode;
cvar_t	*com_ansiColor;
cvar_t	*com_unfocused;
cvar_t	*com_maxfpsUnfocused;
cvar_t	*com_minimized;
cvar_t	*com_maxfpsMinimized;
cvar_t	*com_abnormalExit;
cvar_t	*com_standalone;
cvar_t	*com_gamename;
cvar_t	*com_protocol;
#ifdef LEGACY_PROTOCOL
cvar_t	*com_legacyprotocol;
#endif
cvar_t	*com_basegame;
cvar_t  *com_homepath;
cvar_t	*com_busyWait;
#ifndef DEDICATED
cvar_t  *con_autochat;
#endif

cvar_t	*precache;

cvar_t	*com_target_game;
cvar_t	*com_target_shortversion;
cvar_t	*com_target_version;
cvar_t	*com_target_extension;
cvar_t	*com_target_demo;

int protocol_version_demo;
int protocol_version_full;

// com_speeds times
int		time_game;
int		time_frontend;		// renderer frontend time
int		time_backend;		// renderer backend time

int			com_frameTime;
int			com_frameNumber;
int			com_frameMsec;

qboolean	com_errorEntered = qfalse;
qboolean	com_fullyInitialized = qfalse;
qboolean	com_gameRestarting = qfalse;
qboolean	com_gameClientRestarting = qfalse;
qboolean	com_gotOriginalConfig = qfalse;
qboolean    com_firstConfig = qfalse;

char	com_errorMessage[MAXPRINTMSG];

static const char *target_game_names[] =
{
	"moh",
	"mohta",
	"mohtt"
};

void Com_WriteConfig_f( void );
void CIN_CloseAllVideos(void);

void Com_InitTargetGame();
void Com_InitHunkMemory( void );
void Z_InitMemory( void );

//============================================================================

static char	*rd_buffer;
static int	rd_buffersize;
static void	(*rd_flush)( char *buffer );

float currentfps;	// presents a second (Com_PresentGate counts them)

void Com_BeginRedirect (char *buffer, int buffersize, void (*flush)( char *) )
{
	if (!buffer || !buffersize || !flush)
		return;
	rd_buffer = buffer;
	rd_buffersize = buffersize;
	rd_flush = flush;

	*rd_buffer = 0;
}

void Com_EndRedirect (void)
{
	if ( rd_flush ) {
		rd_flush(rd_buffer);
	}

	rd_buffer = NULL;
	rd_buffersize = 0;
	rd_flush = NULL;
}

#ifndef _COM_NOPRINTF

/*
=============
Com_Printf

Both client and server can use this, and it will output
to the apropriate place.

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
=============
*/
void QDECL Com_Printf( const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	static qboolean opening_qconsole = qfalse;
	static qboolean recursive_count = qfalse;

	if (recursive_count) {
		return;
	}
	recursive_count = qtrue;

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end(argptr);

	if ( rd_buffer ) {
		if ((strlen (msg) + strlen(rd_buffer)) > (rd_buffersize - 1)) {
			rd_flush(rd_buffer);
			*rd_buffer = 0;
		}
		Q_strcat(rd_buffer, rd_buffersize, msg);
    // TTimo nooo .. that would defeat the purpose
		//rd_flush(rd_buffer);
		//*rd_buffer = 0;
		recursive_count--;
		return;
	}

#ifndef DEDICATED
	if (com_dedicated && !com_dedicated->integer) {
		UI_PrintConsole(msg);
	}
#endif

	// echo to dedicated console and early console
	Sys_Print( msg );

	// logfile
	if ( com_logfile && com_logfile->integer ) {
    // TTimo: only open the qconsole.log if the filesystem is in an initialized state
    //   also, avoid recursing in the qconsole.log opening (i.e. if fs_debug is on)
		if ( !logfile && FS_Initialized() && !opening_qconsole) {
			struct tm *newtime;
			time_t aclock;

      opening_qconsole = qtrue;

			time( &aclock );
			newtime = localtime( &aclock );

			logfile = FS_FOpenFileWrite_HomeData( "qconsole.log" );
			
            // Remove recursive count as it won't be able to print relevant info
            recursive_count--;
			if(logfile)
			{
				Com_Printf( "logfile opened on %s\n", asctime( newtime ) );
				Com_Printf( "=> game is version %s\n", PRODUCT_NAME " " PRODUCT_VERSION_FULL " " PLATFORM_STRING " " PRODUCT_VERSION_DATE );
                Com_Printf( "=> targeting game ID %d\n", Cvar_VariableIntegerValue( "com_target_game" ) );

				if ( com_logfile->integer > 1 )
				{
					// force it to not buffer so we get valid
					// data even if we are crashing
					FS_ForceFlush(logfile);
				}
			}
			else
			{
				Com_Printf("Opening qconsole.log failed!\n");
				Cvar_SetValue("logfile", 0);
			}
            recursive_count++;

      opening_qconsole = qfalse;
		}
        if (logfile && FS_Initialized()) {
            // Added in OPM
            //  Write the time before each message
            //================
			size_t msgLen = strlen(msg);

            if (msgLen > 0) {
                if (com_logfile_timestamps->integer) {
                    static qboolean no_newline = qfalse;

                    if (!no_newline) {
                        time_t t;
                        time_t t_gmt;
                        struct tm tms_local;
                        struct tm tms_gm;
                        double tz;
                        const char* tzStr;
                        char buffer[26];

                        t = time(NULL);
    #ifdef WIN32
                        localtime_s(&tms_local, &t);
                        gmtime_s(&tms_gm, &t);
    #else
                        localtime_r(&t, &tms_local);
                        gmtime_r(&t, &tms_gm);
    #endif
                        t_gmt = mktime(&tms_gm);
                        tz = difftime(t, t_gmt) / 60.0 / 60.0;

                        strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", &tms_local);
                        FS_Write("[", 1, logfile);
                        FS_Write(buffer, strlen(buffer), logfile);
                        FS_Write(" ", 1, logfile);

                        if (tz >= 0) {
                            tzStr = va("UTC+%.03f", tz);
                        } else {
                            tzStr = va("UTC%.03f", tz);
                        }

                        FS_Write(tzStr, strlen(tzStr), logfile);
                        FS_Write("] ", 2, logfile);
                    }

                    if (msg[msgLen - 1] != '\n') {
                        // Don't write the time if the previous message has no newline
                        no_newline = qtrue;
                    } else {
                        no_newline = qfalse;
                    }
                }
                //================

                FS_Write(msg, msgLen, logfile);
            }
		}
    }

    recursive_count--;
}


/*
================
Com_DPrintf

A Com_Printf that only shows up if the "developer" cvar is set
================
*/
void QDECL Com_DPrintf( const char *fmt, ...) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if ( !developer || !developer->integer ) {
		return;			// don't confuse non-developers with techie stuff...
	}

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Com_Printf ("%s", msg);
}

/*
================
Com_DPrintf2

A Com_Printf that only shows up if the "developer" cvar is set
================
*/
void QDECL Com_DPrintf2( const char *fmt, ...) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if ( !developer || !developer->integer ) {
		return;			// don't confuse non-developers with techie stuff...
	}

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Com_Printf ("%s", msg);

#ifndef DEDICATED
	if (com_dedicated && !com_dedicated->integer) {
		UI_PrintDeveloperConsole(msg);
	}
#endif
}

/*
================
Com_DebugPrintf

A Com_Printf that only shows up if the "developer" cvar is set
================
*/
void QDECL Com_DebugPrintf( const char *fmt, ...) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	if ( !developer || !developer->integer ) {
		return;			// don't confuse non-developers with techie stuff...
	}

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	Sys_DebugPrint (msg);
}

#endif

/*
=============
Com_Error

Both client and server can use this, and it will
do the apropriate things.
=============
*/
void QDECL Com_Error( int code, const char *fmt, ... ) {
	va_list		argptr;
	static int	lastErrorTime;
	static int	errorCount;
	int			currentTime;

#ifdef COM_ERROR_DROP_ASSERT
	if (code == ERR_DROP)
	{
		*(int*)0 = 0;
		assert(qfalse);
	}
#endif

	// Debug builds should stop on this
	assert(code != ERR_FATAL);

	Cvar_Set( "com_errorCode", va( "%i", code ) );

	// when we are running automated scripts, make sure we
	// know if anything failed
	if ( com_buildScript && com_buildScript->integer ) {
		code = ERR_FATAL;
	}

	// if we are getting a solid stream of ERR_DROP, do an ERR_FATAL
	currentTime = Sys_Milliseconds();
	if ( currentTime - lastErrorTime < 100 ) {
		if ( ++errorCount > 3 ) {
			code = ERR_FATAL;
		}
	} else {
		errorCount = 0;
	}
	lastErrorTime = currentTime;

	if ( com_errorEntered ) {
		Sys_Error( "recursive error after: %s", com_errorMessage );
	}
	com_errorEntered = qtrue;

	va_start (argptr,fmt);
	Q_vsnprintf (com_errorMessage,sizeof(com_errorMessage),fmt,argptr);
	va_end (argptr);

	if (code != ERR_DISCONNECT && code != ERR_NEED_CD)
		Cvar_Set("com_errorMessage", com_errorMessage);

	if (code == ERR_DISCONNECT || code == ERR_SERVERDISCONNECT) {
		if (com_cl_running && com_cl_running->integer) {
			CL_AbnormalDisconnect();
			CL_FlushMemory();
			CL_StartHunkUsers(qfalse);
		} else {
            SV_Shutdown(va("Server disconnected: %s", com_errorMessage));
		}
		// make sure we can get at our local stuff
		FS_PureServerSetLoadedPaks("", "");
		com_errorEntered = qfalse;
		longjmp (abortframe, -1);
	} else if (code == ERR_DROP) {
		Com_Printf ("********************\nERROR: %s\n********************\n", com_errorMessage);
        SV_Shutdown(va("Server crashed: %s", com_errorMessage));
#ifndef DEDICATED
		if (com_cl_running && com_cl_running->integer) {
			CL_AbnormalDisconnect();
			CL_FlushMemory();
			CL_StartHunkUsers(qfalse);
		}
#endif
		FS_PureServerSetLoadedPaks("", "");
		com_errorEntered = qfalse;
		longjmp (abortframe, -1);
	} else if ( code == ERR_NEED_CD ) {
        SV_Shutdown("Server didn't have CD");
        if (com_cl_running && com_cl_running->integer) {
            CL_Disconnect();
            CL_FlushMemory();
            com_errorEntered = qfalse;
            CL_CDDialog();
        } else {
            Com_Printf("Server didn't have CD\n");
        }
		FS_PureServerSetLoadedPaks("", "");
		longjmp (abortframe, -1);
	} else {
		CL_Shutdown (va("Client fatal crashed: %s", com_errorMessage), qtrue, qtrue);
		SV_Shutdown (va("Server fatal crashed: %s", com_errorMessage));
	}

	Com_Shutdown ();

	Sys_Error("%s", com_errorMessage);
}


/*
=============
Com_Quit_f

Both client and server can use this, and it will
do the appropriate things.
=============
*/
void Com_Quit_f( void ) {
	// don't try to shutdown if we are in a recursive error
	char *p = Cmd_Args( );
	if ( !com_errorEntered ) {
		// Some VMs might execute "quit" command directly,
		// which would trigger an unload of active VM error.
		// Sys_Quit will kill this process anyways, so
		// a corrupt call stack makes no difference
		VM_Forced_Unload_Start();
		SV_Shutdown(p[0] ? p : "Server quit");
		CL_Shutdown(p[0] ? p : "Client quit", qtrue, qtrue);
		VM_Forced_Unload_Done();
		Com_Shutdown ();
		FS_Shutdown(qtrue);
	}
	Sys_Quit ();
}



/*
============================================================================

COMMAND LINE FUNCTIONS

+ characters seperate the commandLine string into multiple console
command lines.

All of these are valid:

quake3 +set test blah +map test
quake3 set test blah+map test
quake3 set test blah + map test

============================================================================
*/

#define	MAX_CONSOLE_LINES	32
int		com_numConsoleLines;
char	*com_consoleLines[MAX_CONSOLE_LINES];

/*
==================
Com_ParseCommandLine

Break it up into multiple console lines
==================
*/
void Com_ParseCommandLine( char *commandLine ) {
    int inq = 0;
    com_consoleLines[0] = commandLine;
    com_numConsoleLines = 1;

    while ( *commandLine ) {
        if (*commandLine == '"') {
            inq = !inq;
        }
        // look for a + seperating character
        // if commandLine came from a file, we might have real line seperators
        if ( (*commandLine == '+' && !inq) || *commandLine == '\n'  || *commandLine == '\r' ) {
            if ( com_numConsoleLines == MAX_CONSOLE_LINES ) {
                return;
            }
            com_consoleLines[com_numConsoleLines] = commandLine + 1;
            com_numConsoleLines++;
            *commandLine = 0;
        }
        commandLine++;
    }
}


/*
===================
Com_SafeMode

Check for "safe" on the command line, which will
skip loading of q3config.cfg
===================
*/
qboolean Com_SafeMode( void ) {
	int		i;

	for ( i = 0 ; i < com_numConsoleLines ; i++ ) {
		Cmd_TokenizeString( com_consoleLines[i] );
		if ( !Q_stricmp( Cmd_Argv(0), "safe" )
			|| !Q_stricmp( Cmd_Argv(0), "cvar_restart" ) ) {
			com_consoleLines[i][0] = 0;
			return qtrue;
		}
	}
	return qfalse;
}


/*
===============
Com_StartupVariable

Searches for command line parameters that are set commands.
If match is not NULL, only that cvar will be looked for.
That is necessary because cddir and basedir need to be set
before the filesystem is started, but all other sets should
be after execing the config and default.
===============
*/
void Com_StartupVariable( const char *match ) {
	int		i;
	char	*s;

	for (i=0 ; i < com_numConsoleLines ; i++) {
		Cmd_TokenizeString( com_consoleLines[i] );
		if ( strcmp( Cmd_Argv(0), "set" ) ) {
			continue;
		}

		s = Cmd_Argv(1);
		
		if(!match || !strcmp(s, match))
		{
			if(Cvar_Flags(s) == CVAR_NONEXISTENT)
				Cvar_Get(s, Cmd_ArgsFrom(2), CVAR_USER_CREATED);
			else
				Cvar_Set2(s, Cmd_ArgsFrom(2), qfalse);
		}
	}
}


/*
=================
Com_AddStartupCommands

Adds command line parameters as script statements
Commands are seperated by + signs

Returns qtrue if any late commands were added, which
will keep the demoloop from immediately starting
=================
*/
qboolean Com_AddStartupCommands( void ) {
	int		i;
	qboolean	added;

	added = qfalse;
	// quote every token, so args with semicolons can work
	for (i=0 ; i < com_numConsoleLines ; i++) {
		if ( !com_consoleLines[i] || !com_consoleLines[i][0] ) {
			continue;
		}

		// set commands already added with Com_StartupVariable
		if ( !Q_stricmpn( com_consoleLines[i], "set ", 4 ) ) {
			continue;
		}
		Cbuf_AddText( com_consoleLines[i] );
		Cbuf_AddText( "\n" );
	}

	return added;
}


//============================================================================

void Info_Print( const char *s ) {
	char	key[BIG_INFO_KEY];
	char	value[BIG_INFO_VALUE];
	char	*o;
	int		l;

	if (*s == '\\')
		s++;
	while (*s)
	{
		o = key;
		while (*s && *s != '\\')
			*o++ = *s++;

		l = o - key;
		if (l < 20)
		{
			Com_Memset (o, ' ', 20-l);
			key[20] = 0;
		}
		else
			*o = 0;
		Com_Printf ("%s : ", key);

		if (!*s)
		{
			Com_Printf ("MISSING VALUE\n");
			return;
		}

		o = value;
		s++;
		while (*s && *s != '\\')
			*o++ = *s++;
		*o = 0;

		if (*s)
			s++;
		Com_Printf ("%s\n", value);
	}
}

/*
============
Com_StringContains
============
*/
const char *Com_StringContains(const char *str1, const char *str2, int casesensitive) {
	intptr_t len;
	int i, j;

	len = strlen(str1) - strlen(str2);
	for (i = 0; i <= len; i++, str1++) {
		for (j = 0; str2[j]; j++) {
			if (casesensitive) {
				if (str1[j] != str2[j]) {
					break;
				}
			}
			else {
				if (toupper(str1[j]) != toupper(str2[j])) {
					break;
				}
			}
		}
		if (!str2[j]) {
			return str1;
		}
	}
	return NULL;
}

/*
============
Com_Filter
============
*/
int Com_Filter(const char *filter, const char *name, int casesensitive)
{
	char buf[MAX_TOKEN_CHARS];
	const char *ptr;
	int i, found;

	while(*filter) {
		if (*filter == '*') {
			filter++;
			for (i = 0; *filter; i++) {
				if (*filter == '*' || *filter == '?' || *filter == '[') break;
				buf[i] = *filter;
				filter++;
			}
			buf[i] = '\0';
			if (strlen(buf)) {
				ptr = Com_StringContains(name, buf, casesensitive);
				if (!ptr) return qfalse;
				name = ptr + strlen(buf);
			}
		}
		else if (*filter == '?') {
			filter++;
			name++;
		}
		else if (*filter == '[' && *(filter+1) == '[') {
			filter++;
		}
		else if (*filter == '[') {
			filter++;
			found = qfalse;
			while(*filter && !found) {
				if (*filter == ']' && *(filter+1) != ']') break;
				if (*(filter+1) == '-' && *(filter+2) && (*(filter+2) != ']' || *(filter+3) == ']')) {
					if (casesensitive) {
						if (*name >= *filter && *name <= *(filter+2)) found = qtrue;
					}
					else {
						if (toupper(*name) >= toupper(*filter) &&
							toupper(*name) <= toupper(*(filter+2))) found = qtrue;
					}
					filter += 3;
				}
				else {
					if (casesensitive) {
						if (*filter == *name) found = qtrue;
					}
					else {
						if (toupper(*filter) == toupper(*name)) found = qtrue;
					}
					filter++;
				}
			}
			if (!found) return qfalse;
			while(*filter) {
				if (*filter == ']' && *(filter+1) != ']') break;
				filter++;
			}
			filter++;
			name++;
		}
		else {
			if (casesensitive) {
				if (*filter != *name) return qfalse;
			}
			else {
				if (toupper(*filter) != toupper(*name)) return qfalse;
			}
			filter++;
			name++;
		}
	}
	return qtrue;
}

/*
============
Com_FilterPath
============
*/
int Com_FilterPath(const char *filter, const char *name, int casesensitive)
{
	int i;
	char new_filter[MAX_QPATH];
	char new_name[MAX_QPATH];

	for (i = 0; i < MAX_QPATH-1 && filter[i]; i++) {
		if ( filter[i] == '\\' || filter[i] == ':' ) {
			new_filter[i] = '/';
		}
		else {
			new_filter[i] = filter[i];
		}
	}
	new_filter[i] = '\0';
	for (i = 0; i < MAX_QPATH-1 && name[i]; i++) {
		if ( name[i] == '\\' || name[i] == ':' ) {
			new_name[i] = '/';
		}
		else {
			new_name[i] = name[i];
		}
	}
	new_name[i] = '\0';
	return Com_Filter(new_filter, new_name, casesensitive);
}

/*
================
Com_RealTime
================
*/
int Com_RealTime(qtime_t *qtime) {
	time_t t;
	struct tm *tms;

	t = time(NULL);
	if (!qtime)
		return t;
	tms = localtime(&t);
	if (tms) {
		qtime->tm_sec = tms->tm_sec;
		qtime->tm_min = tms->tm_min;
		qtime->tm_hour = tms->tm_hour;
		qtime->tm_mday = tms->tm_mday;
		qtime->tm_mon = tms->tm_mon;
		qtime->tm_year = tms->tm_year;
		qtime->tm_wday = tms->tm_wday;
		qtime->tm_yday = tms->tm_yday;
		qtime->tm_isdst = tms->tm_isdst;
	}
	return t;
}

/*
========================
CopyString

 NOTE:	never write over the memory CopyString returns because
		memory from a memstatic_t might be returned
========================
*/
char *CopyString( const char *in ) {
	char	*out;

#ifndef ZONE_DEBUG
	if (!in[0]) {
		return ( char * )Z_EmptyStringPointer();
	}
	else if (!in[1]) {
		if (in[0] >= '0' && in[0] <= '9') {
			return ( char * )Z_NumberStringPointer( *in );
		}
	}
#endif

	out = ( char * )Z_Malloc( strlen( in ) + 1 );
	strcpy( out, in );
	return out;
}

/*
====================
B_Malloc
====================
*/
void *B_Malloc( size_t size ) {
	return Z_Malloc( size );
}

/*
====================
B_Free
====================
*/
void B_Free( void *ptr ) {
	return Z_Free( ptr );
}

void CL_ShutdownCGame( void );
void CL_ShutdownUI( void );
void SV_ShutdownGameProgs( void );


/*
===================================================================

EVENTS AND JOURNALING

In addition to these events, .cfg files are also copied to the
journaled file
===================================================================
*/

#define	MAX_PUSHED_EVENTS	            1024
static int com_pushedEventsHead = 0;
static int com_pushedEventsTail = 0;
static sysEvent_t	com_pushedEvents[MAX_PUSHED_EVENTS];

/*
=================
Com_InitJournaling
=================
*/
void Com_InitJournaling( void ) {
	Com_StartupVariable( "journal" );
	com_journal = Cvar_Get ("journal", "0", CVAR_INIT);
	if ( !com_journal->integer ) {
		return;
	}

	if ( com_journal->integer == 1 ) {
		Com_Printf( "Journaling events\n");
		com_journalFile = FS_FOpenFileWrite_HomeState( "journal.dat" );
		com_journalDataFile = FS_FOpenFileWrite_HomeState( "journaldata.dat" );
	} else if ( com_journal->integer == 2 ) {
		Com_Printf( "Replaying journaled events\n");
		FS_FOpenFileRead( "journal.dat", &com_journalFile, qtrue, qtrue );
		FS_FOpenFileRead( "journaldata.dat", &com_journalDataFile, qtrue, qtrue );
	}

	if ( !com_journalFile || !com_journalDataFile ) {
		Cvar_Set( "com_journal", "0" );
		com_journalFile = 0;
		com_journalDataFile = 0;
		Com_Printf( "Couldn't open journal files\n" );
	}
}

/*
========================================================================

EVENT LOOP

========================================================================
*/

#define MAX_QUEUED_EVENTS  256
#define MASK_QUEUED_EVENTS ( MAX_QUEUED_EVENTS - 1 )

static sysEvent_t  eventQueue[ MAX_QUEUED_EVENTS ];
static int         eventHead = 0;
static int         eventTail = 0;

/*
================
Com_QueueEvent

A time of 0 will get the current time
Ptr should either be null, or point to a block of data that can
be freed by the game later.
================
*/
void Com_QueueEvent( int time, sysEventType_t type, int value, int value2, size_t ptrLength, void *ptr )
{
	sysEvent_t  *ev;

	// combine mouse movement with previous mouse event
	if ( type == SE_MOUSE && eventHead != eventTail )
	{
		ev = &eventQueue[ ( eventHead + MAX_QUEUED_EVENTS - 1 ) & MASK_QUEUED_EVENTS ];

		if ( ev->evType == SE_MOUSE )
		{
			ev->evValue += value;
			ev->evValue2 += value2;
			return;
		}
	}

	ev = &eventQueue[ eventHead & MASK_QUEUED_EVENTS ];

	if ( eventHead - eventTail >= MAX_QUEUED_EVENTS )
	{
		Com_Printf("Com_QueueEvent: overflow\n");
		// we are discarding an event, but don't leak memory
		if ( ev->evPtr )
		{
			Z_Free( ev->evPtr );
		}
		eventTail++;
	}

	eventHead++;

	if ( time == 0 )
	{
		time = Sys_Milliseconds();
	}

	ev->evTime = time;
	ev->evType = type;
	ev->evValue = value;
	ev->evValue2 = value2;
	ev->evPtrLength = ptrLength;
	ev->evPtr = ptr;
}

/*
================
Com_GetSystemEvent

================
*/
sysEvent_t Com_GetSystemEvent(void)
{
	sysEvent_t  ev;
	char        *s;

	// return if we have data
	if ( eventHead > eventTail )
	{
		eventTail++;
		return eventQueue[ ( eventTail - 1 ) & MASK_QUEUED_EVENTS ];
	}

	// check for console commands
	s = Sys_ConsoleInput();
	if ( s )
	{
		char  *b;
		int   len;

		len = strlen( s ) + 1;
		b = Z_Malloc( len );
		strcpy( b, s );
		Com_QueueEvent( 0, SE_CONSOLE, 0, 0, len, b );
	}

	// return if we have data
	if ( eventHead > eventTail )
	{
		eventTail++;
		return eventQueue[ ( eventTail - 1 ) & MASK_QUEUED_EVENTS ];
	}

	// create an empty event to return
	memset( &ev, 0, sizeof( ev ) );
	ev.evTime = Sys_Milliseconds();

	return ev;
}

/*
=================
Com_GetRealEvent
=================
*/
sysEvent_t	Com_GetRealEvent( void ) {
	int			r;
	sysEvent_t	ev;

	// either get an event from the system or the journal file
	if ( com_journal->integer == 2 ) {
		r = FS_Read( &ev, sizeof(ev), com_journalFile );
		if ( r != sizeof(ev) ) {
			Com_Error( ERR_FATAL, "Error reading from journal file" );
		}
		if ( ev.evPtrLength ) {
			ev.evPtr = Z_Malloc( ev.evPtrLength );
			r = FS_Read( ev.evPtr, ev.evPtrLength, com_journalFile );
			if ( r != ev.evPtrLength ) {
				Com_Error( ERR_FATAL, "Error reading from journal file" );
			}
		}
	} else {
		ev = Com_GetSystemEvent();

		// write the journal value out if needed
		if ( com_journal->integer == 1 ) {
			r = FS_Write( &ev, sizeof(ev), com_journalFile );
			if ( r != sizeof(ev) ) {
				Com_Error( ERR_FATAL, "Error writing to journal file" );
			}
			if ( ev.evPtrLength ) {
				r = FS_Write( ev.evPtr, ev.evPtrLength, com_journalFile );
				if ( r != ev.evPtrLength ) {
					Com_Error( ERR_FATAL, "Error writing to journal file" );
				}
			}
		}
	}

	return ev;
}


/*
=================
Com_InitPushEvent
=================
*/
void Com_InitPushEvent( void ) {
  // clear the static buffer array
  // this requires SE_NONE to be accepted as a valid but NOP event
  memset( com_pushedEvents, 0, sizeof(com_pushedEvents) );
  // reset counters while we are at it
  // beware: GetEvent might still return an SE_NONE from the buffer
  com_pushedEventsHead = 0;
  com_pushedEventsTail = 0;
}


/*
=================
Com_PushEvent
=================
*/
void Com_PushEvent( sysEvent_t *event ) {
	sysEvent_t		*ev;
	static int printedWarning = 0;

	ev = &com_pushedEvents[ com_pushedEventsHead & (MAX_PUSHED_EVENTS-1) ];

	if ( com_pushedEventsHead - com_pushedEventsTail >= MAX_PUSHED_EVENTS ) {

		// don't print the warning constantly, or it can give time for more...
		if ( !printedWarning ) {
			printedWarning = qtrue;
			Com_Printf( "WARNING: Com_PushEvent overflow\n" );
		}

		if ( ev->evPtr ) {
			Z_Free( ev->evPtr );
		}
		com_pushedEventsTail++;
	} else {
		printedWarning = qfalse;
	}

	*ev = *event;
	com_pushedEventsHead++;
}

/*
=================
Com_GetEvent
=================
*/
sysEvent_t	Com_GetEvent( void ) {
	if ( com_pushedEventsHead > com_pushedEventsTail ) {
		com_pushedEventsTail++;
		return com_pushedEvents[ (com_pushedEventsTail-1) & (MAX_PUSHED_EVENTS-1) ];
	}
	return Com_GetRealEvent();
}

/*
=================
Com_RunAndTimeServerPacket
=================
*/
void Com_RunAndTimeServerPacket( netadr_t *evFrom, msg_t *buf ) {
	int		t1, t2, msec;

	t1 = 0;

	if ( com_speeds->integer ) {
		t1 = Sys_Milliseconds ();
	}

	SV_PacketEvent( *evFrom, buf );

	if ( com_speeds->integer ) {
		t2 = Sys_Milliseconds ();
		msec = t2 - t1;
		if ( com_speeds->integer == 3 ) {
			Com_Printf( "SV_PacketEvent time: %i\n", msec );
		}
	}
}

/*
=================
Com_EventLoop

Returns last event time
=================
*/
int Com_EventLoop( void ) {
	sysEvent_t	ev;
	netadr_t	evFrom;
	byte		bufData[MAX_MSGLEN];
	msg_t		buf;

	MSG_Init( &buf, bufData, sizeof( bufData ) );

	while ( 1 ) {
		ev = Com_GetEvent();

		// if no more events are available
		if ( ev.evType == SE_NONE ) {
			// manually send packet events for the loopback channel
			while ( NET_GetLoopPacket( NS_CLIENT, &evFrom, &buf ) ) {
				CL_PacketEvent( evFrom, &buf );
			}

			while ( NET_GetLoopPacket( NS_SERVER, &evFrom, &buf ) ) {
				// if the server just shut down, flush the events
				if ( com_sv_running->integer ) {
					Com_RunAndTimeServerPacket( &evFrom, &buf );
				}
			}

			return ev.evTime;
		}


		switch(ev.evType)
		{
			case SE_KEY:
				CL_KeyEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
			case SE_CHAR:
				CL_CharEvent( ev.evValue );
			break;
			case SE_MOUSE:
				CL_MouseEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
			case SE_JOYSTICK_AXIS:
				CL_JoystickEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
			case SE_CONSOLE:
				Cbuf_AddText( (char *)ev.evPtr );
				Cbuf_AddText( "\n" );
			break;
			default:
				Com_Error( ERR_FATAL, "Com_EventLoop: bad event type %i", ev.evType );
			break;
		}

		// free any block data
		if ( ev.evPtr ) {
			Z_Free( ev.evPtr );
		}
	}

	return 0;	// never reached
}

/*
================
Com_Milliseconds

Can be used for profiling, but will be journaled accurately
================
*/
int Com_Milliseconds (void) {
	sysEvent_t	ev;

	// get events and push them until we get a null event with the current time
	do {

		ev = Com_GetRealEvent();
		if ( ev.evType != SE_NONE ) {
			Com_PushEvent( &ev );
		}
	} while ( ev.evType != SE_NONE );

	return ev.evTime;
}

//============================================================================

/*
=============
Com_Error_f

Just throw a fatal error to
test error shutdown procedures
=============
*/
static void Q_NO_RETURN Com_Error_f (void) {
	if ( Cmd_Argc() > 1 ) {
		Com_Error( ERR_DROP, "Testing drop error" );
	} else {
		Com_Error( ERR_FATAL, "Testing fatal error" );
	}
}


/*
=============
Com_Freeze_f

Just freeze in place for a given number of seconds to test
error recovery
=============
*/
static void Com_Freeze_f (void) {
	float	s;
	int		start, now;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "freeze <seconds>\n" );
		return;
	}
	s = atof( Cmd_Argv(1) );

	start = Com_Milliseconds();

	while ( 1 ) {
		now = Com_Milliseconds();
		if ( ( now - start ) * 0.001 > s ) {
			break;
		}
	}
}

/*
=================
Com_Crash_f

A way to force a bus error for development reasons
=================
*/
static void Com_Crash_f( void ) {
	* ( volatile int * ) 0 = 0x12345678;
}

/*
==================
Com_Setenv_f

For controlling environment variables
==================
*/
void Com_Setenv_f(void)
{
	int argc = Cmd_Argc();
	char *arg1 = Cmd_Argv(1);

	if(argc > 2)
	{
		char *arg2 = Cmd_ArgsFrom(2);
		
		Sys_SetEnv(arg1, arg2);
	}
	else if(argc == 2)
	{
		char *env = getenv(arg1);
		
		if(env)
			Com_Printf("%s=%s\n", arg1, env);
		else
			Com_Printf("%s undefined\n", arg1);
        }
}

/*
==================
Com_ExecuteCfg

For controlling environment variables
==================
*/

void Com_ExecuteCfg(void)
{
	Cbuf_ExecuteText(EXEC_NOW, "exec default.cfg\n");
	Cbuf_Execute(0); // Always execute after exec to prevent text buffer overflowing

	if(!Com_SafeMode())
	{
		// skip the q3config.cfg and autoexec.cfg if "safe" is on the command line
		Cbuf_ExecuteText(EXEC_NOW, "exec " Q3CONFIG_CFG "\n");
		Cbuf_Execute(0);
		Cbuf_ExecuteText(EXEC_NOW, "exec autoexec.cfg\n");
		Cbuf_Execute(0);
	}
}

/*
==================
Com_GameRestart

Change to a new mod properly with cleaning up cvars before switching.
==================
*/

void Com_GameRestart(int checksumFeed, qboolean disconnect)
{
	// make sure no recursion can be triggered
	if(!com_gameRestarting && com_fullyInitialized)
	{
		com_gameRestarting = qtrue;
		com_gameClientRestarting = com_cl_running->integer;

		// Kill server if we have one
		if(com_sv_running->integer)
			SV_Shutdown("Game directory changed");

		if(com_gameClientRestarting)
		{
			if(disconnect)
				CL_Disconnect();
				
			CL_Shutdown("Game directory changed", disconnect, qfalse);
		}

		FS_Restart(checksumFeed);
	
		// Clean out any user and VM created cvars
		Cvar_Restart(qtrue);
		Com_ExecuteCfg();

		if(disconnect)
		{
			// We don't want to change any network settings if gamedir
			// change was triggered by a connect to server because the
			// new network settings might make the connection fail.
			NET_Restart_f();
		}

		if(com_gameClientRestarting)
		{
			CL_Init();
			CL_StartHunkUsers(qfalse);
		}
		
		com_gameRestarting = qfalse;
		com_gameClientRestarting = qfalse;
	}
}

/*
==================
Com_GameRestart_f

Expose possibility to change current running mod to the user
==================
*/

void Com_GameRestart_f(void)
{
	Cvar_Set("fs_game", Cmd_Argv(1));

	Com_GameRestart(0, qtrue);
}

#ifndef STANDALONE

// TTimo: centralizing the cl_cdkey stuff after I discovered a buffer overflow problem with the dedicated server version
//   not sure it's necessary to have different defaults for regular and dedicated, but I don't want to risk it
//   https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=470
#ifndef DEDICATED
char	cl_cdkey[34] = "                                ";
#else
char	cl_cdkey[34] = "123456789";
#endif

/*
=================
Com_ReadCDKey
=================
*/
qboolean CL_CDKeyValidate( const char *key, const char *checksum );
void Com_ReadCDKey( const char *filename ) {
	fileHandle_t	f;
	char			buffer[33];
	char			fbuffer[MAX_OSPATH];

	Com_sprintf(fbuffer, sizeof(fbuffer), "%s/q3key", filename);

	FS_BaseDir_FOpenFileRead( fbuffer, &f );
	if ( !f ) {
		Com_Memset( cl_cdkey, '\0', 17 );
		return;
	}

	Com_Memset( buffer, 0, sizeof(buffer) );

	FS_Read( buffer, 16, f );
	FS_FCloseFile( f );

	if (CL_CDKeyValidate(buffer, NULL)) {
		Q_strncpyz( cl_cdkey, buffer, 17 );
	} else {
		Com_Memset( cl_cdkey, '\0', 17 );
	}
}

/*
=================
Com_AppendCDKey
=================
*/
void Com_AppendCDKey( const char *filename ) {
	fileHandle_t	f;
	char			buffer[33];
	char			fbuffer[MAX_OSPATH];

	Com_sprintf(fbuffer, sizeof(fbuffer), "%s/q3key", filename);

	FS_BaseDir_FOpenFileRead( fbuffer, &f );
	if (!f) {
		Com_Memset( &cl_cdkey[16], '\0', 17 );
		return;
	}

	Com_Memset( buffer, 0, sizeof(buffer) );

	FS_Read( buffer, 16, f );
	FS_FCloseFile( f );

	if (CL_CDKeyValidate(buffer, NULL)) {
		strcat( &cl_cdkey[16], buffer );
	} else {
		Com_Memset( &cl_cdkey[16], '\0', 17 );
	}
}

#ifndef DEDICATED
/*
=================
Com_WriteCDKey
=================
*/
static void Com_WriteCDKey( const char *filename, const char *ikey ) {
	fileHandle_t	f;
	char			fbuffer[MAX_OSPATH];
	char			key[17];
#ifndef _WIN32
	mode_t			savedumask;
#endif


	Com_sprintf(fbuffer, sizeof(fbuffer), "%s/q3key", filename);


	Q_strncpyz( key, ikey, 17 );

	if(!CL_CDKeyValidate(key, NULL) ) {
		return;
	}

#ifndef _WIN32
	savedumask = umask(0077);
#endif
	f = FS_BaseDir_FOpenFileWrite_HomeState( fbuffer );
	if ( !f ) {
		Com_Printf ("Couldn't write CD key to %s.\n", fbuffer );
		goto out;
	}

	FS_Write( key, 16, f );

	FS_Printf( f, "\n// generated by " PRODUCT_NAME ", do not modify\r\n" );
	FS_Printf( f, "// Do not give this file to ANYONE.\r\n" );
	FS_Printf( f, "// id Software and Activision will NOT ask you to send this file to them.\r\n");

	FS_FCloseFile( f );
out:
#ifndef _WIN32
	umask(savedumask);
#else
	;
#endif
}
#endif

#endif // STANDALONE

static void Com_DetectAltivec(void)
{
	// Only detect if user hasn't forcibly disabled it.
	if (com_altivec->integer) {
		static qboolean altivec = qfalse;
		static qboolean detected = qfalse;
		if (!detected) {
			altivec = !!( Sys_GetProcessorFeatures( ) & CF_ALTIVEC );
			detected = qtrue;
		}

		if (!altivec) {
			Cvar_Set( "com_altivec", "0" );  // we don't have it! Disable support!
		}
	}
}

/*
=================
Com_InitRand
Seed the random number generator, if possible with an OS supplied random seed.
=================
*/
static void Com_InitRand(void)
{
	unsigned int seed;

	if(Sys_RandomBytes((byte *) &seed, sizeof(seed)))
		srand(seed);
	else
		srand(time(NULL));
}

/*
=================
Com_ConfigExists
=================
*/
qboolean Com_ConfigExists(const char* configname) {
	fileHandle_t handle = (fileHandle_t)0;

    if (FS_FOpenFileRead(va("configs/%s", configname), &handle, qfalse, qfalse) == -1) {
		return qfalse;
    }

	FS_FCloseFile(handle);
	return qtrue;
}

/*
=================
Com_Init
=================
*/
void Com_Init( char *commandLine ) {
	int			iStart;
	int			iEnd;
	const char	*s;
	char		configname[ 128 ];
	int			qport;
	qboolean	configExists;

	Com_Printf( "--- Common Initialization ---\n" );

	iStart = Sys_Milliseconds();

	Com_Printf( "%s %s %s %s\n", PRODUCT_NAME, PRODUCT_VERSION_FULL, PLATFORM_STRING, PRODUCT_VERSION_DATE );

	if ( setjmp (abortframe) ) {
		Sys_Error("Error during initialization");
	}

	// Clear queues
	Com_Memset( &eventQueue[ 0 ], 0, MAX_QUEUED_EVENTS * sizeof( sysEvent_t ) );

	// initialize the weak pseudo-random number generator for use later.
	Com_InitRand();

	// do this before anything else decides to push events
	Com_InitPushEvent();

	// prepare enough of the subsystems to handle
	// cvar and command buffer management
	Com_ParseCommandLine( commandLine );

	Swap_Init();
	Cbuf_Init();
	Z_InitMemory();

	Cmd_Init();
	Cvar_Init();

	// override anything from the config files with command line args
	Com_StartupVariable( NULL );

	// get the developer cvar set as early as possible
	Com_StartupVariable( "developer" );

	developer = Cvar_Get( "developer", "0", CVAR_ARCHIVE );

	// done early so bind command exists
	CL_InitKeyCommands();

	Com_InitTargetGame();

	com_standalone = Cvar_Get("com_standalone", "0", CVAR_ROM);
	com_basegame = Cvar_Get("com_basegame", BASEGAME, CVAR_INIT);
    if (com_target_demo->integer) {
		// As full and demo are different products,
		// it's better for them to have separate location for configs and saves
	    com_homepath = Cvar_Get("com_homepath", va("%s-demo", HOMEPATH_NAME), CVAR_INIT|CVAR_PROTECTED);
    } else {
	    com_homepath = Cvar_Get("com_homepath", "", CVAR_INIT|CVAR_PROTECTED);
    }

	FS_InitFilesystem ();

	Com_InitJournaling();

	TIKI_Begin();

	Cbuf_Execute( 0 );

	Cbuf_AddText( "exec default.cfg\n" );

	Cbuf_AddText( "exec menu.cfg\n" );

	// Removed in OPM
	//  Not relevant as there is no auto-configurator
	//Cbuf_AddText ("exec newconfig.cfg\n");

	cvar_global_force = qtrue;
	Cbuf_Execute( 0 );
	cvar_global_force = qfalse;

	strncpy( configname, Q3CONFIG_CFG, sizeof( configname ) );

	// override anything from the config files with command line args
	Com_StartupVariable( "config" );

	config = Cvar_Get( "config", configname, 0 );

	if( strlen( config->string ) > 1 )
	{
		size_t len = strlen( config->string );

		Q_strncpyz( configname, config->string, len >= sizeof( configname ) ? sizeof( configname ) : len );
		COM_StripExtension( configname, configname, sizeof( configname ) );
		Q_strcat( configname, sizeof( configname ), ".cfg" );
		SaveRegistryInfo( 1, "config", configname, sizeof( configname ) );
	}
	else
	{
		long size = sizeof( configname );
		LoadRegistryInfo(1, "config", configname, &size );
		COM_DefaultExtension( configname, sizeof( configname ), ".cfg" );
	}

	SaveRegistryInfo( 1, "config", configname, sizeof( configname ) );

	Cvar_Set( "config", configname );
	Com_Printf( "Config: %s\n", configname );

    // Look if a config file exists whether in main or expansion
	configExists = Com_ConfigExists(configname);

	if ( configExists ) {
		// Look if the config specific to the game exists
		com_firstConfig = !FS_FileExists_HomeConfig(va("configs/%s", configname));

		if ( com_firstConfig ) {
			//
			// Create an empty file to prevent expansions from inheriting the base file
			// and inheriting from variables such as cg_crosshair_friend
			//FS_WriteFile(va("configs/%s", configname), "", 0);
		}

		Cbuf_AddText( va( "exec configs/%s\n", configname ) );
	} else if ( Com_ConfigExists( "unnamedsoldier.cfg" ) ) {
		Com_Printf( "The config file '%s' doesn't exist, using unnamedsoldier.cfg as a template\n", configname );
		// Grab the config file from the original game
		Cbuf_AddText( "exec configs/unnamedsoldier.cfg\n" );
		com_gotOriginalConfig = qtrue;
	} else {
        // Neither original or home config exist
		com_firstConfig = qtrue;
    }

	if( Com_SafeMode() )
	{
		Cbuf_AddText( "exec safemode.cfg\n" );
	}
	else if( IsFirstRun() || IsNewConfig() )
	{
		Cbuf_AddText( "exec newconfig.cfg\n" );
		cvar_modifiedFlags |= CVAR_ARCHIVE;
	}

	Cbuf_AddText( "exec localized.cfg\n" );
	Cbuf_AddText( "exec autoexec.cfg\n" );
	Cbuf_Execute( 0 );
	Com_StartupVariable( NULL );
	Cvar_Set( "config", configname );

	// if any archived cvars are modified after this, we will trigger a writing
	// of the config file
	cvar_modifiedFlags &= ~CVAR_ARCHIVE;

	if( developer && developer->integer ) {
		Cmd_AddCommand( "error", Com_Error_f );
		Cmd_AddCommand( "crash", Com_Crash_f );
		Cmd_AddCommand( "freeze", Com_Freeze_f );
	}
	Cmd_AddCommand("quit", Com_Quit_f);
	Cmd_AddCommand("changeVectors", MSG_ReportChangeVectors_f );
	Cmd_AddCommand("writeconfig", Com_WriteConfig_f );
	Cmd_SetCommandCompletionFunc( "writeconfig", Cmd_CompleteCfgName );
	Cmd_AddCommand("pause", Com_Pause_f);
	Cmd_AddCommand("game_restart", Com_GameRestart_f);

	// override anything from the config files with command line args
	Com_StartupVariable( NULL );

  // get dedicated here for proper hunk megs initialization
#ifdef DEDICATED
	com_dedicated = Cvar_Get ("dedicated", "1", CVAR_INIT);
	Cvar_CheckRange( com_dedicated, 1, 2, qtrue );
#else
	com_dedicated = Cvar_Get ("dedicated", "0", CVAR_LATCH);
	Cvar_CheckRange( com_dedicated, 0, 2, qtrue );
#endif
	// allocate the stack based hunk allocator
	Com_InitHunkMemory();

	// if any archived cvars are modified after this, we will trigger a writing
	// of the config file
	cvar_modifiedFlags &= ~CVAR_ARCHIVE;

	//
	// init commands and vars
	//
	console_spam_filter = Cvar_Get("spam", "1", 0); // why is this in Com_Printf in mohaas/mohaab??
	dumploadedanims = Cvar_Get( "dumploadedanims", "0", 0 );
	low_anim_memory = Cvar_Get( "low_anim_memory", "0", 0 );
	showLoad = Cvar_Get( "showLoad", "0", 0 );
	convertAnims = Cvar_Get( "convertAnim", "0", 0 );
	com_altivec = Cvar_Get ("com_altivec", "1", CVAR_ARCHIVE);
	// MoHAA Reloaded's default (upstream's is 85), as on the metal branch; movement doesn't depend on it (cl_movefps)
	com_maxfps = Cvar_Get( "com_maxfps", "125", CVAR_ARCHIVE );
	deathmatch = Cvar_Get( "deathmatch", "0", 0 );
	paused = Cvar_Get( "paused", "0", 64 );
	autopaused = Cvar_Get( "autopaused", "1", CVAR_ARCHIVE );
	fps = Cvar_Get( "fps", "0", CVAR_ARCHIVE );
	fps_location = Cvar_Get( "fps_location", "0", CVAR_ARCHIVE );
	com_timescale = Cvar_Get( "timescale", "1", CVAR_CHEAT | CVAR_SYSTEMINFO );
	com_fixedtime = Cvar_Get( "fixedtime", "0", CVAR_CHEAT );
	com_showtrace = Cvar_Get( "com_showtrace", "0", CVAR_CHEAT );
	com_dropsim = Cvar_Get( "com_dropsim", "0", CVAR_CHEAT );
	com_viewlog = Cvar_Get( "viewlog", "0", CVAR_CHEAT );
	com_logfile = Cvar_Get("logfile", "0", CVAR_TEMP);
	com_logfile_timestamps = Cvar_Get("logfile_timestamps", "1", CVAR_TEMP);
	com_speeds = Cvar_Get( "com_speeds", "0", 0 );
	com_timedemo = Cvar_Get( "timedemo", "0", CVAR_CHEAT );
	com_dedicated = Cvar_Get( "dedicated", "0", CVAR_LATCH );
	cl_packetdelay = Cvar_Get( "cl_packetdelay", "0", 0 );
	sv_packetdelay = Cvar_Get( "sv_packetdelay", "0", 0 );
	com_sv_running = Cvar_Get( "sv_running", "0", CVAR_ROM );
	com_cl_running = Cvar_Get( "cl_running", "0", CVAR_ROM );
	com_buildScript = Cvar_Get( "com_buildScript", "0", 0 );
	com_radar_range = Cvar_Get( "com_radar_range", "1024", CVAR_ARCHIVE | CVAR_SYSTEMINFO );
	com_ansiColor = Cvar_Get( "com_ansiColor", "0", CVAR_ARCHIVE );

	com_unfocused = Cvar_Get( "com_unfocused", "0", CVAR_ROM );
	com_maxfpsUnfocused = Cvar_Get( "com_maxfpsUnfocused", "0", CVAR_ARCHIVE );
	com_minimized = Cvar_Get( "com_minimized", "0", CVAR_ROM );
	com_maxfpsMinimized = Cvar_Get( "com_maxfpsMinimized", "0", CVAR_ARCHIVE );
	com_abnormalExit = Cvar_Get( "com_abnormalExit", "0", CVAR_ROM );
	com_busyWait = Cvar_Get("com_busyWait", "0", CVAR_ARCHIVE);

	if( com_dedicated->integer )
	{
		if( !com_viewlog->integer ) {
			Cvar_Set( "viewlog", "1" );
		}
	}

    // Make the original game version appear first for backward compatibility.
    // => Some software rely on the beginning of the string starting with "Medal of Honor [...]".
    //    There are mods that also check for this.
	s = va( "%s (%s %s) %s %s", com_target_version->string, PRODUCT_NAME, PRODUCT_VERSION_FULL, PLATFORM_STRING, PRODUCT_VERSION_DATE);
    com_version = Cvar_Get( "version", s, CVAR_ROM | CVAR_SERVERINFO );
	com_gamename = Cvar_Get("com_gamename", "", CVAR_SERVERINFO | CVAR_INIT | CVAR_USERINFO | CVAR_SERVERINFO);
	com_shortversion = Cvar_Get( "shortversion", PRODUCT_VERSION, CVAR_ROM | CVAR_USERINFO | CVAR_SERVERINFO );
	com_protocol = Cvar_Get("com_protocol", va("%i", PROTOCOL_VERSION), CVAR_INIT);
#ifdef LEGACY_PROTOCOL
	com_legacyprotocol = Cvar_Get("com_legacyprotocol", va("%i", PROTOCOL_LEGACY_VERSION), CVAR_INIT);

	// Keep for compatibility with old mods / mods that haven't updated yet.
	if(com_legacyprotocol->integer > 0)
		Cvar_Get("protocol", com_legacyprotocol->string, CVAR_SERVERINFO | CVAR_ROM);
	else
#endif
		Cvar_Get("protocol", com_protocol->string, CVAR_SERVERINFO | CVAR_ROM);

#ifndef DEDICATED
	con_autochat = Cvar_Get("con_autochat", "1", CVAR_ARCHIVE);
#endif

	demo_protocols[0] = com_protocol->integer;

	Sys_Init();

#ifdef NDEBUG
	Sys_InitPIDFile(FS_GetCurrentGameDir());
#endif

	// Pick a random port value
	Com_RandomBytes((byte*)&qport, sizeof(int));
	Netchan_Init(qport & 0xffff);
	SV_Init();

	com_dedicated->modified = qfalse;

#ifndef DEDICATED
	if( com_dedicated->integer )
	{
		Sys_CloseMutex();
	}
	else
	{
		CL_Init();
		Sys_ShowConsole( com_viewlog->integer, qfalse );
	}
#else
    Sys_CloseMutex();
#endif

	// set com_frameTime so that if a map is started on the
	// command line it will still be able to count on com_frameTime
	// being random enough for a serverid
	com_frameTime = Com_Milliseconds();

	// add + commands from command line
	Com_AddStartupCommands();

	com_fullyInitialized = qtrue;

	// always set the cvar, but only print the info if it makes sense.
	Com_DetectAltivec();
#if idppc
	Com_Printf ("Altivec support is %s\n", com_altivec->integer ? "enabled" : "disabled");
#endif

	com_pipefile = Cvar_Get( "com_pipefile", "", CVAR_ARCHIVE|CVAR_LATCH );
	if( com_pipefile->string[0] )
	{
		pipefile = FS_FCreateOpenPipeFile( com_pipefile->string );
	}

	RecoverLostAutodialData();

    // Added in OPM
    //  Initialize GameSpy related stuff
    Com_InitGameSpy();

	iEnd = Sys_Milliseconds();
	Com_Printf( "--- Common Initialization Complete --- %i ms\n", iEnd - iStart );
}

/*
===============
Com_ReadFromPipe

Read whatever is in com_pipefile, if anything, and execute it
===============
*/
void Com_ReadFromPipe( void )
{
	static char buf[MAX_STRING_CHARS];
	static int accu = 0;
	int read;

	if( !pipefile )
		return;

	while( ( read = FS_Read( buf + accu, sizeof( buf ) - accu - 1, pipefile ) ) > 0 )
	{
		char *brk = NULL;
		int i;

		for( i = accu; i < accu + read; ++i )
		{
			if( buf[ i ] == '\0' )
				buf[ i ] = '\n';
			if( buf[ i ] == '\n' || buf[ i ] == '\r' )
				brk = &buf[ i + 1 ];
		}
		buf[ accu + read ] = '\0';

		accu += read;

		if( brk )
		{
			char tmp = *brk;
			*brk = '\0';
			Cbuf_ExecuteText( EXEC_APPEND, buf );
			*brk = tmp;

			accu -= brk - buf;
			memmove( buf, brk, accu + 1 );
		}
		else if( accu >= sizeof( buf ) - 1 ) // full
		{
			Cbuf_ExecuteText( EXEC_APPEND, buf );
			accu = 0;
		}
	}
}


//==================================================================

void Com_WriteConfigToFile( const char *filename ) {
	fileHandle_t	f;
	char			szFullName[ 256 ];

	memcpy( szFullName, "configs/", 9 );
	strcat( szFullName, filename );
	COM_StripExtension( szFullName, szFullName, sizeof( szFullName ) );
	strcat( szFullName, ".cfg" );

	f = FS_FOpenFileWrite_HomeConfig( szFullName );
	if ( !f ) {
		Com_Printf ("Couldn't write %s.\n", filename );
		return;
	}

	FS_Printf (f, "// generated by " PRODUCT_NAME ", do not modify\n");
	FS_Printf( f, "//\n" );
	FS_Printf( f, "// Key Bindings\n" );
	FS_Printf( f, "//\n" );
	Key_WriteBindings( f );
	FS_Printf( f, "//\n" );
	FS_Printf( f, "// Cvars\n" );
	FS_Printf( f, "//\n" );
	Cvar_WriteVariables( f );
	FS_Printf( f, "//\n" );
	FS_Printf( f, "// Aliases\n" );
	FS_Printf( f, "//\n" );
	Cmd_WriteAliases( f );

	FS_FCloseFile( f );
}


/*
===============
Com_WriteConfiguration

Writes key bindings and archived cvars to config file if modified
===============
*/
void Com_WriteConfiguration( void ) {
#ifndef DEDICATED
	cvar_t	*fs;
#endif
	// if we are quiting without fully initializing, make sure
	// we don't write out anything
	if ( !com_fullyInitialized ) {
		return;
	}

	if ( !(cvar_modifiedFlags & CVAR_ARCHIVE ) ) {
		return;
	}
	cvar_modifiedFlags &= ~CVAR_ARCHIVE;

	Com_WriteConfigToFile( config->string );

	// not needed for dedicated
#ifndef DEDICATED
	fs = Cvar_Get ("fs_game", "", CVAR_INIT|CVAR_SYSTEMINFO );
#endif
}


/*
===============
Com_WriteConfig_f

Write the config file to a specific name
===============
*/
void Com_WriteConfig_f( void ) {
	char	filename[MAX_QPATH];

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "Usage: writeconfig <filename>\n" );
		return;
	}

	Q_strncpyz( filename, Cmd_Argv( 1 ), sizeof( filename ) );
	COM_DefaultExtension( filename, sizeof( filename ), ".cfg" );
	Com_Printf( "Writing %s.\n", filename );
	Com_WriteConfigToFile( filename );
}

/*
================
Com_ModifyMsec
================
*/
/*
=================
Com_LatchRenderFraction

One monotonic read, latched to com_frameTime. The fraction is how far the
monotonic clock has moved into the current server millisecond. It is not
added to serverTime and it is not a Pmove input.
=================
*/
static int		renderLatchMs;
static int64_t	renderLatchUs;
static float	renderFraction;

void Com_LatchRenderFraction( void ) {
	int64_t	nowUs;
	float	frac;

	nowUs = Sys_Microseconds();
	if ( com_frameTime != renderLatchMs ) {
		renderLatchMs = com_frameTime;
		renderLatchUs = nowUs;
		renderFraction = 0.0f;
		return;
	}

	frac = (float)( nowUs - renderLatchUs ) / 1000.0f;
	if ( frac < 0.0f ) {
		frac = 0.0f;
	} else if ( frac > 0.999f ) {
		frac = 0.999f;
	}
	renderFraction = frac;
}

float Com_RenderTimeFraction( void ) {
	return renderFraction;
}

int Com_ModifyMsec( int msec ) {
	int		clampTime;

	//
	// modify time for debugging values
	//
	if ( com_fixedtime->integer ) {
		msec = com_fixedtime->integer;
	} else if ( com_timescale->value ) {
		msec *= com_timescale->value;
	}

	// don't let it scale below 1 msec; at timescale 1 a frame shorter than a
	// millisecond (client caps above 1000, uncapped) lasts 0 ms, else the game
	// would run ahead of real time
	if ( msec < 1 && com_timescale->value && com_timescale->value != 1.0f ) {
		msec = 1;
	}

	if ( com_dedicated->integer ) {
		// dedicated servers don't want to clamp for a much longer
		// period, because it would mess up all the client's views
		// of time.
		if (com_sv_running->integer && msec > 500)
			Com_Printf( "Hitch warning: %i msec frame time\n", msec );

		clampTime = 5000;
	} else
	if ( !com_sv_running->integer ) {
		// clients of remote servers do not want to clamp time, because
		// it would skew their view of the server's time temporarily
		clampTime = 5000;
	} else {
		// for local single player gaming
		// we may want to clamp the time to prevent players from
		// flying off edges when something hitches.
		clampTime = 200;
	}

	if ( msec > clampTime ) {
		msec = clampTime;
	}

	return msec;
}

/*
=================
Com_TimeVal
=================
*/

int Com_TimeVal(int minMsec)
{
	int timeVal;

	timeVal = Sys_Milliseconds() - com_frameTime;

	if(timeVal >= minMsec)
		timeVal = 0;
	else
		timeVal = minMsec - timeVal;

	return timeVal;
}

/*
=================
Com_FrameWait

Waits until the next frame may start: until Sys_Milliseconds has passed
com_frameTime by minMsec and Sys_Nanoseconds has reached deadlineNs (0:
none), and returns the time it saw both, the frame's start. The network is
read as packets come (NET_SleepUs, at least once a frame: nothing else
reads it) and the local server sends its queued packets when they are due.

Both are waited for the same way: asleep until pollNs before, then
spinning. Sleeps end late, by up to 0.9 ms on a Ryzen 7 8700GE when the
core reached its deepest idle state; shorter sleeps instead of the
spinning used less CPU but let the core clock down, and the frame's work
(input read to present) took longer. pollNs is COM_POLL_NS at least and
grows when a sleep ends later than that (a loaded machine, a coarser
timer), to COM_POLL_PAD_NS over the lateness, up to COM_POLL_MAX_NS; it
shrinks back by a millisecond a minute. The spinning polls the network
until the last COM_SPIN_NS, which it spins on the clocks alone: the
deadline is then seen within a clock read (tens of ns), where a select()
per iteration saw it 1 to 2 us late, and the next frame's deadline (one
period after this frame's start, Com_Frame) would carry that forward.

The millisecond clock is waited for in whole milliseconds, its last one
spun (when it ticks is not known); with a cap it never binds, since its
deadline is at least a period after the previous start. The spinning
notes when it ticks (com_tickNs): the phase of the millisecond clock
against the deadlines, for the whole-millisecond periods (Com_Frame). On
Windows, where Sys_Milliseconds follows the timer interrupts and a
select() ends on one of them, up to 1 ms late, the whole milliseconds
but two are slept in select() (packets wake it) and the rest to pollNs in
a waitable timer (Sys_SleepNs: high resolution on Windows 10 1803 and
later), and the millisecond clock, when a deadline wakes the frame on
time anyway, is slept for a millisecond less.

When the spinning starts after a sleep, the window system's events are
handled once (IN_PumpEvents), so the next frame's input read has only what
came after.
=================
*/
#define COM_POLL_NS		1000000LL	// the sleeping stops this long before a deadline, at least
#define COM_POLL_MAX_NS	4000000LL	// and at most, after late wake-ups
#define COM_POLL_PAD_NS	250000LL	// what a late wake-up grows it by, over its lateness
#define COM_SPIN_NS		50000LL		// the last of a deadline, spun on the clocks alone
#define COM_TICK_GUARD_NS	50000LL		// a start this close before a millisecond tick moves to the tick (Com_Frame)

#if defined( _WIN32 )
#define Com_SpinPause()	YieldProcessor()
#elif defined( __x86_64__ ) || defined( __i386__ )
#define Com_SpinPause()	__builtin_ia32_pause()
#elif defined( __aarch64__ )
#define Com_SpinPause()	__asm__ __volatile__( "yield" )
#else
#define Com_SpinPause()	( (void)0 )
#endif

/*
=================
Com_PresentGate

The frame limiter's hold before a present. Com_Frame records each frame's
start (the time its wait ended) under the frame's number, which the
renderer takes as a token at the frame's end (Com_FrameToken, carried
through its command stream to whichever thread presents) and gives back
before presenting the frame: to Com_PresentGate, which holds until one
period of the cap after the previous present, outside any lock the present
needs (the submit thread's queue lock, which the main thread's submissions
take too), and to Com_PresentMark right before the present call, inside
it, which is the present's moment the next gate counts from: no two
presents are closer than the period, whatever the frames' work did and
whatever delayed the call after the gate. The frame starts alone, paced
one period apart, let a frame whose work was shorter than the previous
frame's present early by the difference, and the frame after a long one by
most of a period.

It also measures each frame's work, its start to its arrival here (not
the hold: that is not work), and keeps the 90th percentile of the last
COM_LEAD_SAMPLES as the lead (com_leadNs): how long before its present a
frame should start. Com_Frame starts a frame no earlier than the lead
before the gate of its own present (one period after the previous
present), so that a frame after a long one is not started at once to then
wait here for most of a period, and the time from input to present stays
the lead, or the work when that is longer. And it counts the presents: the
fps counter (currentfps), presents a second over the last COM_FPS_SAMPLES,
which no present interval below the period can push above the cap.

Runs on the thread that presents (the main thread, the render thread with
r_smp, the submit thread with r_vkSubmitThread), one at a time; Com_Frame
reads what it leaves. The shared words are 64-bit and aligned: read and
written whole on the platforms this builds for.
=================
*/
#define COM_FRAME_RING		16
#define COM_LEAD_SAMPLES	64
#define COM_FPS_SAMPLES		16

typedef struct {
	uint64_t	number;		// the frame (com_frameNumber)
	int64_t		startNs;	// when its wait ended
	int64_t		presentNs;	// when its present was released, 0: not yet
} comFrame_t;

static volatile comFrame_t	com_frames[COM_FRAME_RING];	// by number
static volatile int64_t	com_presentPeriodNs;	// the cap's period for the gate, 0: none
static volatile int64_t	com_lastPresentNs;		// the last gate's release
static volatile int64_t	com_leadNs;				// the lead (above), 0: unknown yet
static int64_t	com_leadSamples[COM_LEAD_SAMPLES];	// the presenting thread's
static int		com_leadIndex, com_numLeadSamples;
static int64_t	com_fpsTimes[COM_FPS_SAMPLES];		// the last presents
static int		com_numPresents;

uint64_t Com_FrameToken( void )
{
	return (uint64_t)com_frameNumber;
}

static int Com_CompareNs( const void *a, const void *b )
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? -1 : x > y;
}

void Com_PresentGate( uint64_t token )
{
	volatile comFrame_t	*frame = &com_frames[token % COM_FRAME_RING];
	int64_t	nowNs, periodNs, gateNs;
	int		n;

	nowNs = Sys_Nanoseconds();

	// the frame's work, for the lead
	if ( frame->number == token && frame->startNs && nowNs > frame->startNs ) {
		com_leadSamples[com_leadIndex] = nowNs - frame->startNs;
		com_leadIndex = ( com_leadIndex + 1 ) % COM_LEAD_SAMPLES;
		if ( com_numLeadSamples < COM_LEAD_SAMPLES ) {
			com_numLeadSamples++;
		}
		if ( com_numLeadSamples < 8 || ( com_leadIndex & 7 ) == 0 ) {
			int64_t sorted[COM_LEAD_SAMPLES];

			n = com_numLeadSamples;
			Com_Memcpy( sorted, com_leadSamples, n * sizeof( sorted[0] ) );
			qsort( sorted, n, sizeof( sorted[0] ), Com_CompareNs );
			com_leadNs = sorted[n * 9 / 10];
		}
	}

	// the hold
	periodNs = com_presentPeriodNs;
	if ( periodNs ) {
		gateNs = com_lastPresentNs + periodNs;
		while ( nowNs < gateNs ) {
			Com_SpinPause();
			nowNs = Sys_Nanoseconds();
		}
	}
}

void Com_PresentMark( uint64_t token )
{
	volatile comFrame_t	*frame = &com_frames[token % COM_FRAME_RING];
	int64_t	nowNs, oldestNs;
	int		i;

	nowNs = Sys_Nanoseconds();
	com_lastPresentNs = nowNs;
	if ( frame->number == token ) {
		frame->presentNs = nowNs;
	}

	// the fps counter
	if ( fps && fps->integer ) {
		i = com_numPresents % COM_FPS_SAMPLES;
		oldestNs = com_fpsTimes[i];
		com_fpsTimes[i] = nowNs;
		com_numPresents++;
		if ( com_numPresents > COM_FPS_SAMPLES && nowNs > oldestNs ) {
			currentfps = COM_FPS_SAMPLES * 1000000000.0 / ( nowNs - oldestNs );
		}
	}
}

// when a frame's present was released, 0: not yet, or not that frame
static int64_t Com_FramePresentNs( int number )
{
	volatile comFrame_t	*frame;

	if ( number < 0 ) {
		return 0;
	}
	frame = &com_frames[number % COM_FRAME_RING];
	return frame->number == (uint64_t)number ? frame->presentNs : 0;
}

static int64_t	com_tickNs;		// when Sys_Milliseconds last ticked while a wait spun, 0: never seen

static int64_t Com_FrameWait( int minMsec, int64_t deadlineNs )
{
	static int64_t	pollNs = COM_POLL_NS;	// how long before a deadline the sleeping stops
	static int64_t	lastNs;		// the last frame's start
	int64_t	nowNs, waitNs, sleepNs, msWaitNs, msSleepNs, wakeNs, lateNs;
	int		ms, lastMs = 0, timeVal, timeValSV;
	qboolean	slept = qfalse, pumped = qfalse, polled = qfalse;

	for ( ;; ) {
		nowNs = Sys_Nanoseconds();
		ms = Sys_Milliseconds();
		if ( lastMs && ms != lastMs ) {
			// a tick, seen while spinning (lastMs is 0 after a sleep)
			com_tickNs = nowNs;
		}
		lastMs = ms;
		timeVal = ms - com_frameTime;	// whole milliseconds of Sys_Milliseconds to go, 0: passed (Com_TimeVal)
		timeVal = timeVal >= minMsec ? 0 : minMsec - timeVal;
		if ( polled && !timeVal && nowNs >= deadlineNs ) {
			break;
		}

		// the sleep ends pollNs before the later of the deadline and the
		// tick, which is timeVal - 1 to timeVal ms away
		waitNs = deadlineNs ? deadlineNs - nowNs : 0;
		sleepNs = waitNs - pollNs;
		if ( timeVal ) {
			msSleepNs = ( timeVal - 1 ) * 1000000LL;
			msWaitNs = msSleepNs + 1000000;
#ifdef _WIN32
			if ( deadlineNs ) {
				msSleepNs -= 1000000;
			}
#endif
			if ( waitNs < msWaitNs ) {
				waitNs = msWaitNs;
			}
			if ( sleepNs < msSleepNs ) {
				sleepNs = msSleepNs;
			}
		}

		if ( sleepNs > 0 && com_sv_running->integer ) {
			timeValSV = SV_SendQueuedPackets();
			if ( sleepNs > ( timeValSV - 1 ) * 1000000LL ) {
				sleepNs = ( timeValSV - 1 ) * 1000000LL;
			}
		}
		if ( com_busyWait->integer ) {
			sleepNs = 0;
		}
#ifdef _WIN32
		if ( !deadlineNs ) {
			// a server's frame has no deadline to be on time for: whole
			// milliseconds in select(), the last one spun
			sleepNs -= sleepNs % 1000000;
		}
#endif

		if ( sleepNs > 0 ) {
			slept = qtrue;
			lastMs = 0;
			wakeNs = nowNs + sleepNs;
#ifdef _WIN32
			// select() ends on a tick of the system timer, up to 1 ms late:
			// the whole milliseconds but two in it (packets wake it), the
			// rest in a timer
			if ( !deadlineNs || sleepNs >= 3000000 ) {
				NET_SleepUs( deadlineNs ? ( sleepNs / 1000000 - 2 ) * 1000 : sleepNs / 1000 );
				polled = qtrue;
				continue;
			}
			Sys_SleepNs( sleepNs );
#else
			NET_SleepUs( sleepNs / 1000 );
			polled = qtrue;
#endif
			// a wake-up later than the margin would start the frame late:
			// the margin grows to cover the next one
			lateNs = Sys_Nanoseconds() - wakeNs;
			if ( lateNs + COM_POLL_PAD_NS > pollNs ) {
				pollNs = lateNs + COM_POLL_PAD_NS;
				if ( pollNs > COM_POLL_MAX_NS ) {
					pollNs = COM_POLL_MAX_NS;
				}
			}
			continue;
		}

		// spinning
		if ( slept && !pumped ) {
			IN_PumpEvents();
			pumped = qtrue;
		}
		if ( !polled || waitNs > COM_SPIN_NS ) {
			if ( com_sv_running->integer ) {
				SV_SendQueuedPackets();
			}
			NET_SleepUs( 0 );
			polled = qtrue;
		} else {
			Com_SpinPause();
		}
	}

	// the margin shrinks back slowly, a millisecond a minute
	if ( lastNs ) {
		pollNs -= ( nowNs - lastNs ) / 60000;
		if ( pollNs < COM_POLL_NS ) {
			pollNs = COM_POLL_NS;
		}
	}
	lastNs = nowNs;

	return nowNs;
}

/*
=================
Com_Frame
=================
*/
void Com_Frame( void ) {

	int		msec, minMsec;
	static int	lastTime = 0;
	static int64_t	frameStartNs;	// when the last frame's wait ended: its start
	int64_t	periodNs, deadlineNs, nowNs, gateNs, phaseNs;
	int		periodMs, maxfps;

	int		timeBeforeFirstEvents;
	int		timeBeforeServer;
	int		timeBeforeEvents;
	int		timeBeforeClient;
	int		timeAfter;
  

	if ( setjmp (abortframe) ) {
		return;			// an ERR_DROP was thrown
	}

	SV_SetFrameNumber(com_frameNumber);

#ifndef DEDICATED
	if (!com_dedicated || !com_dedicated->integer)
	{
		//
		// Update the client frame number
		// So stuff like TIKI caches can get refreshed
		// And TIKI models can get rendered correctly
		//
		CL_SetFrameNumber(com_frameNumber);
	}
#endif

	timeBeforeFirstEvents =0;
	timeBeforeServer =0;
	timeBeforeEvents =0;
	timeBeforeClient = 0;
	timeAfter = 0;

	// write config file if anything changed
	Com_WriteConfiguration();

#ifndef DEDICATED
    if (com_viewlog->modified) {
        if (!com_dedicated || !com_dedicated->integer) {
            Sys_ShowConsole(com_viewlog->integer, qfalse);
        }

        com_viewlog->modified = qfalse;
    }
#endif

	//
	// main event loop
	//
	if ( com_speeds->integer ) {
		timeBeforeFirstEvents = Sys_Milliseconds ();
	}

    // Figure out how much time we have: a dedicated server's frame, and the
    // period of a client's frame rate cap. Caps up to 1000 also wait for 1 ms
    // of Sys_Milliseconds (no frame of 0 ms); at a timescale of 1, a client
    // cap above 1000 waits only for its deadline in nanoseconds and an
    // uncapped client not at all: its frames can last 0 ms of game time
    // (Com_ModifyMsec, which makes them 1 ms at other timescales), the
    // commands to the server come at their own rate (cl_movefps). With
    // cl_movefps 0 every frame makes a command, so caps above 1000 and
    // uncapped are 1000 (a frame per tick), as stock
    minMsec = 1;
    periodNs = 0;
    periodMs = 0;
    if (!com_timedemo->integer)
    {
        if (com_dedicated->integer)
            minMsec = SV_FrameMsec();
        else
        {
            if (com_minimized->integer && com_maxfpsMinimized->integer > 0)
                maxfps = com_maxfpsMinimized->integer;
            else if (com_unfocused->integer && com_maxfpsUnfocused->integer > 0)
                maxfps = com_maxfpsUnfocused->integer;
            else
                maxfps = com_maxfps->integer;

#ifndef DEDICATED
            if ((maxfps > 1000 || maxfps <= 0) && CL_CommandPerFrame())
                maxfps = 1000;
#endif

            if (maxfps > 0)
            {
                periodNs = (1000000000LL + maxfps - 1) / maxfps; // rounded up: never above the cap
                if (maxfps > 1000)
                {
                    if (com_timescale->value == 1.0f)
                        minMsec = 0;
                }
                else if (1000 % maxfps == 0)
                    periodMs = 1000 / maxfps;
            }
            else if (com_timescale->value == 1.0f)
                minMsec = 0;
        }
    }

    // The next frame starts one period after this one started, never
    // sooner: no interval between two frame starts is below the period,
    // and a frame late by any amount (its work ran past the period, a late
    // wake-up) moves every frame after it by as much. What the rate loses
    // is how late the wait sees each deadline, tens of ns (Com_FrameWait).
    // The presents are held apart by the period too (Com_PresentGate), and
    // a frame starts no earlier than the lead (the time a frame takes from
    // its start to its present, com_leadNs) before the gate of its own
    // present, one period after the previous frame's: a frame after a long
    // one isn't started at once to then wait at the gate for most of a
    // period, and the time from input to present stays the lead, or the
    // work when that is longer. The thread that presents may not have
    // presented the previous frame yet: then two periods after the one
    // before it.
    nowNs = Sys_Nanoseconds();
    com_presentPeriodNs = periodNs;
    deadlineNs = 0;
    if (periodNs)
    {
        deadlineNs = frameStartNs + periodNs;
        gateNs = Com_FramePresentNs(com_frameNumber - 1);
        if (gateNs)
            gateNs += periodNs;
        else if ((gateNs = Com_FramePresentNs(com_frameNumber - 2)) != 0)
            gateNs += 2 * periodNs;
        if (gateNs && deadlineNs < gateNs - com_leadNs)
            deadlineNs = gateNs - com_leadNs;
    }

    // Whole-millisecond periods keep every frame the same whole milliseconds
    // of game time: com_frameTime, read 7 to 14 us after the start, must
    // fall in the same millisecond relative to the start each frame (else 3
    // and 5 ms frames instead of 4 at 250, see the README), so a start
    // within COM_TICK_GUARD_NS before a tick of Sys_Milliseconds (whole
    // milliseconds after the last tick a wait's spinning saw) moves to the
    // tick. The starts drift forward only (a late frame, a long one), so
    // this happens once per millisecond of drift.
    if (periodMs && com_tickNs && nowNs - com_tickNs < 10000000)
    {
        phaseNs = (deadlineNs - com_tickNs) % 1000000;
        if (phaseNs < 0)
            phaseNs += 1000000;
        if (phaseNs > 1000000 - COM_TICK_GUARD_NS)
            deadlineNs += 1000000 - phaseNs;
    }

    frameStartNs = Com_FrameWait(minMsec, deadlineNs);
    com_frames[com_frameNumber % COM_FRAME_RING].presentNs = 0;
    com_frames[com_frameNumber % COM_FRAME_RING].startNs = frameStartNs;
    com_frames[com_frameNumber % COM_FRAME_RING].number = (uint64_t)com_frameNumber;

    IN_Frame();

    lastTime = com_frameTime;
    com_frameTime = Com_EventLoop();
    Com_LatchRenderFraction();

    msec = com_frameTime - lastTime;

#ifndef DEDICATED
	if (com_dedicated->integer || CL_FinishedIntro())
	{
		Cbuf_Execute(0);
		SV_CheckSaveGame();
	}
#else
	// Fixed in OPM
	//   On dedicated servers, the client part that runs buffered commands is never run.
	//   Therefore, the delta time has to be specified here for wait commands to work
	Cbuf_Execute(msec);
#endif

    if (com_altivec->modified)
    {
        Com_DetectAltivec();
        com_altivec->modified = qfalse;
    }

    // mess with msec if needed
    msec = Com_ModifyMsec(msec);

    //
    // server side
    //
    if (com_speeds->integer) {
        timeBeforeServer = Sys_Milliseconds();
    }

	SV_Frame( msec );

	// if "dedicated" has been modified, start up
	// or shut down the client system.
	// Do this after the server may have started,
	// but before the client tries to auto-connect
	if ( com_dedicated->modified ) {
		// get the latched value
		Cvar_Get( "dedicated", "0", 0 );
		com_dedicated->modified = qfalse;
		if ( !com_dedicated->integer ) {
			SV_Shutdown( "dedicated set to 0" );
			CL_FlushMemory();
		} else {
			CL_Shutdown("dedicated set to 1", qtrue, qtrue);
			Sys_CloseMutex();
			Sys_ShowConsole(0, 0);
			NET_Init();
		}
	}

#ifndef DEDICATED
	//
	// client system
	//
	//
	// run event loop a second time to get server to client packets
	// without a frame of latency
	//
	if ( com_speeds->integer ) {
		timeBeforeEvents = Sys_Milliseconds ();
	}
	Com_EventLoop();
	if (CL_FinishedIntro()) {
		Cbuf_Execute(msec);
	}


	//
	// client side
	//
	if ( com_speeds->integer ) {
		timeBeforeClient = Sys_Milliseconds ();
	}

	CL_Frame( msec );

	if ( com_speeds->integer ) {
		timeAfter = Sys_Milliseconds ();
	}
#else
	if ( com_speeds->integer ) {
		timeAfter = Sys_Milliseconds ();
		timeBeforeEvents = timeAfter;
		timeBeforeClient = timeAfter;
	}
#endif


	NET_FlushPacketQueue();

	//
	// report timing information
	//
	if ( com_speeds->integer ) {
		int			all, sv, ev, cl;

		all = timeAfter - timeBeforeServer;
		sv = timeBeforeEvents - timeBeforeServer;
		ev = timeBeforeServer - timeBeforeFirstEvents + timeBeforeClient - timeBeforeEvents;
		cl = timeAfter - timeBeforeClient;
		sv -= time_game;
		cl -= time_frontend + time_backend;

		Com_Printf ("frame:%i all:%3i sv:%3i ev:%3i cl:%3i gm:%3i rf:%3i bk:%3i\n",
					 com_frameNumber, all, sv, ev, cl, time_game, time_frontend, time_backend );
	}

	//
	// trace optimization tracking
	//
	if ( com_showtrace->integer ) {

		extern	int c_traces, c_brush_traces, c_patch_traces, c_terrain_patch_traces;
		extern	int	c_pointcontents;

		Com_Printf ("%4i traces  (%ib %ip %itp) %4i points\n", c_traces,
			c_brush_traces, c_patch_traces, c_terrain_patch_traces, c_pointcontents);
		c_traces = 0;
		c_brush_traces = 0;
		c_patch_traces = 0;
		c_terrain_patch_traces = 0;
		c_pointcontents = 0;
	}

	Com_ReadFromPipe();

    Sys_ProcessBackgroundTasks();

	com_frameNumber++;
}

/*
=================
Com_Shutdown
=================
*/
void Com_Shutdown (void) {
	if (logfile) {
		FS_FCloseFile (logfile);
		logfile = 0;
	}

	if ( com_journalFile ) {
		FS_FCloseFile( com_journalFile );
		com_journalFile = 0;
	}

	if( pipefile ) {
		FS_FCloseFile( pipefile );
		FS_Remove_HomeData( com_pipefile->string );
    }
}

qboolean Com_SanitizeName(const char* pszOldName, char* pszNewName, size_t bufferSize)
{
    int i;
    qboolean bBadName = qfalse;
    const char* p = pszOldName;
    size_t maxLength;

    maxLength = (bufferSize / sizeof(char)) - 1;
    bBadName = qfalse;

    for (p = pszOldName; *p && (unsigned char)*p <= ' '; p++) {
        bBadName = qtrue;
    }

    for (i = 0; *p && (unsigned char)*p >= ' ' && i < maxLength; p++, i++)
    {
        if (*p == '~' || *p == '`')
        {
            pszNewName[i] = '*';
            bBadName = qtrue;
        }
        else if (*p == '\"')
        {
            pszNewName[i] = '\'';
            bBadName = qtrue;
        }
        else if (*p == '\\')
        {
            pszNewName[i] = '/';
            bBadName = qtrue;
        }
        else if (*p == ';')
        {
            pszNewName[i] = ':';
            bBadName = qtrue;
        }
        else
        {
            pszNewName[i] = *p;
        }
    }

    for (; i > 0 && (unsigned char)pszNewName[i - 1] <= ' '; i--) {
        bBadName = qtrue;
    }

    pszNewName[i] = 0;

    if (!i)
    {
        const char* pNewNameDynamic = va("*** Blank Name #%04d ***", rand() % 100000);
        Q_strncpyz(pszNewName, pNewNameDynamic, bufferSize);
        bBadName = qtrue;
    }

    if (*p)
        bBadName = qtrue;

    return bBadName;
}

const char *Com_GetArchiveFileName( const char *filename, const char *extension )
{
	static char name[ 256 ];
	Com_sprintf( name, sizeof( name ), "%s/%s.%s", Com_GetArchiveFolder(), filename, extension );
	return name;
}

const char *Com_GetArchiveFolder()
{
	static char name[ 256 ];
	char trimmedconfig[ 256 ];

	COM_StripExtension( config->string, trimmedconfig, sizeof( trimmedconfig ) );
	Com_sprintf( name, sizeof( name ), "%s/%s", "save", trimmedconfig );
	return name;
}

void Com_WipeSavegame( const char *savename )
{
	Com_DPrintf( "Com_WipeSaveGame(%s)\n", savename );
	FS_Remove_HomeData( Com_GetArchiveFileName( savename, "sav" ) );
	FS_Remove_HomeData( Com_GetArchiveFileName( savename, "ssv" ) );
	FS_Remove_HomeData( Com_GetArchiveFileName( savename, "tga" ) );
}

qboolean Com_ShiftedStrStr(const char* shifted, const char* name, int offset) {
	char string[1024];
	int i;

	for (i = 0; name[i]; i++) {
		string[i] = name[i] - offset;
	}
	string[i] = 0;

	return strstr(string, shifted) != NULL;
}

qboolean COM_IsMapValid(const char* name) {
	// FIXME: Not sure where this method comes from
	//  this function is present only in the BT 2.40 Mac build
	//  it is not present on the BT 2.40 Win32 build
	return qtrue;

	/*
	char lowered[MAX_QPATH];

	strcpy(lowered, name);
	strlwr(lowered);

	return Com_ShiftedStrStr("adSUbn]cS`]V", lowered, 12)
		|| Com_ShiftedStrStr("RUDUFQJWRTDTGO", lowered, 27)
		|| Com_ShiftedStrStr("etgfkvu", lowered, -2);
	*/
}

void Com_SwapSaveStruct(savegamestruct_t* save) {
	save->version = LittleShort(save->version);
	save->time = LittleLong(save->time);
	save->mapTime = LittleLong(save->mapTime);

	save->tm_loopcount = LittleLong(save->tm_loopcount);
	save->tm_offset = LittleLong(save->tm_offset);
}

//------------------------------------------------------------------------


/*
===========================================
command line completion
===========================================
*/

/*
==================
Field_Clear
==================
*/
void Field_Clear(field_t* edit) {
    memset(edit->buffer, 0, MAX_EDIT_LINE);
    edit->cursor = 0;
    edit->scroll = 0;
}

static const char* completionString;
static char shortestMatch[MAX_TOKEN_CHARS];
static int	matchCount;
// field we are working on, passed to Field_AutoComplete(&g_consoleCommand for instance)
static field_t* completionField;

/*
===============
FindMatches

===============
*/
static void FindMatches(const char* s) {
    int		i;

    if (Q_stricmpn(s, completionString, strlen(completionString))) {
        return;
    }
    matchCount++;
    if (matchCount == 1) {
        Q_strncpyz(shortestMatch, s, sizeof(shortestMatch));
        return;
    }

    // cut shortestMatch to the amount common with s
    for (i = 0; shortestMatch[i]; i++) {
        if (i >= strlen(s)) {
            shortestMatch[i] = 0;
            break;
        }

        if (tolower(shortestMatch[i]) != tolower(s[i])) {
            shortestMatch[i] = 0;
        }
    }
}

/*
===============
PrintMatches

===============
*/
static void PrintMatches(const char* s) {
    if (!Q_stricmpn(s, shortestMatch, strlen(shortestMatch))) {
        Com_Printf("    %s\n", s);
    }
}

/*
===============
PrintCvarMatches

===============
*/
static void PrintCvarMatches(const char* s) {
    char value[TRUNCATE_LENGTH];

    if (!Q_stricmpn(s, shortestMatch, strlen(shortestMatch))) {
        Com_TruncateLongString(value, Cvar_VariableString(s));
        Com_Printf("    %s = \"%s\"\n", s, value);
    }
}

/*
===============
Field_FindFirstSeparator
===============
*/
static char* Field_FindFirstSeparator(char* s)
{
    int i;

    for (i = 0; i < strlen(s); i++)
    {
        if (s[i] == ';')
            return &s[i];
    }

    return NULL;
}

/*
===============
Field_Complete
===============
*/
static qboolean Field_Complete(void)
{
    int completionOffset;

    if (matchCount == 0)
        return qtrue;

    completionOffset = strlen(completionField->buffer) - strlen(completionString);

    Q_strncpyz(&completionField->buffer[completionOffset], shortestMatch,
        sizeof(completionField->buffer) - completionOffset);

    completionField->cursor = strlen(completionField->buffer);

    if (matchCount == 1)
    {
        Q_strcat(completionField->buffer, sizeof(completionField->buffer), " ");
        completionField->cursor++;
        return qtrue;
    }

    Com_Printf("]%s\n", completionField->buffer);

    return qfalse;
}

#ifndef DEDICATED
/*
===============
Field_CompleteKeyname
===============
*/
void Field_CompleteKeyname(void)
{
    matchCount = 0;
    shortestMatch[0] = 0;

    Key_KeynameCompletion(FindMatches);

    if (!Field_Complete())
        Key_KeynameCompletion(PrintMatches);
}
#endif

/*
===============
Field_CompleteFilename
===============
*/
void Field_CompleteFilename(const char* dir,
    const char* ext, qboolean stripExt, qboolean allowNonPureFilesOnDisk)
{
    matchCount = 0;
    shortestMatch[0] = 0;

    FS_FilenameCompletion(dir, ext, stripExt, FindMatches, allowNonPureFilesOnDisk);

    if (!Field_Complete())
        FS_FilenameCompletion(dir, ext, stripExt, PrintMatches, allowNonPureFilesOnDisk);
}

/*
===============
Field_CompleteCommand
===============
*/
void Field_CompleteCommand(char* cmd,
    qboolean doCommands, qboolean doCvars)
{
    int		completionArgument = 0;

    // Skip leading whitespace and quotes
    cmd = Com_SkipCharset(cmd, " \"");

    Cmd_TokenizeStringIgnoreQuotes(cmd);
    completionArgument = Cmd_Argc();

    // If there is trailing whitespace on the cmd
    if (*(cmd + strlen(cmd) - 1) == ' ')
    {
        completionString = "";
        completionArgument++;
    }
    else
        completionString = Cmd_Argv(completionArgument - 1);

#if 0
#ifndef DEDICATED
    // add a '\' to the start of the buffer if it might be sent as chat otherwise
    if (con_autochat->integer && completionField->buffer[0] &&
        completionField->buffer[0] != '\\')
    {
        if (completionField->buffer[0] != '/')
        {
            // Buffer is full, refuse to complete
            if (strlen(completionField->buffer) + 1 >=
                sizeof(completionField->buffer))
                return;

            memmove(&completionField->buffer[1],
                &completionField->buffer[0],
                strlen(completionField->buffer) + 1);
            completionField->cursor++;
        }

        completionField->buffer[0] = '\\';
    }
#endif
#endif

    if (completionArgument > 1)
    {
        const char* baseCmd = Cmd_Argv(0);
        char* p;

#ifndef DEDICATED
        // This should always be qtrue
        if (baseCmd[0] == '\\' || baseCmd[0] == '/')
            baseCmd++;
#endif

        if ((p = Field_FindFirstSeparator(cmd)))
            Field_CompleteCommand(p + 1, qtrue, qtrue); // Compound command
        else
            Cmd_CompleteArgument(baseCmd, cmd, completionArgument);
    }
    else
    {
        if (completionString[0] == '\\' || completionString[0] == '/')
            completionString++;

        matchCount = 0;
        shortestMatch[0] = 0;

        if (strlen(completionString) == 0)
            return;

        if (doCommands)
            Cmd_CommandCompletion(FindMatches);

        if (doCvars)
            Cvar_CommandCompletion(FindMatches);

        if (!Field_Complete())
        {
            // run through again, printing matches
            if (doCommands)
                Cmd_CommandCompletion(PrintMatches);

            if (doCvars)
                Cvar_CommandCompletion(PrintCvarMatches);
        }
    }
}

/*
===============
Field_AutoComplete

Perform Tab expansion
===============
*/
void Field_AutoComplete(field_t* field)
{
    completionField = field;

    Field_CompleteCommand(completionField->buffer, qtrue, qtrue);
}

/*
==================
Com_RandomBytes

fills string array with len random bytes, preferably from the OS randomizer
==================
*/
void Com_RandomBytes( byte *string, int len )
{
	int i;

	if( Sys_RandomBytes( string, len ) )
		return;

	Com_Printf( "Com_RandomBytes: using weak randomization\n" );
	for( i = 0; i < len; i++ )
		string[i] = (unsigned char)( rand() % 256 );
}


/*
==================
Com_IsVoipTarget

Returns non-zero if given clientNum is enabled in voipTargets, zero otherwise.
If clientNum is negative return if any bit is set.
==================
*/
qboolean Com_IsVoipTarget(uint8_t *voipTargets, int voipTargetsSize, int clientNum)
{
	int index;
	if(clientNum < 0)
	{
		for(index = 0; index < voipTargetsSize; index++)
		{
			if(voipTargets[index])
				return qtrue;
		}
		
		return qfalse;
	}

	index = clientNum >> 3;
	
	if(index < voipTargetsSize)
		return (voipTargets[index] & (1 << (clientNum & 0x07)));

	return qfalse;
}

/*
===============
Field_CompletePlayerName
===============
*/
static qboolean Field_CompletePlayerNameFinal( qboolean whitespace )
{
	int completionOffset;

	if( matchCount == 0 )
		return qtrue;

	completionOffset = strlen( completionField->buffer ) - strlen( completionString );

	Q_strncpyz( &completionField->buffer[ completionOffset ], shortestMatch,
		sizeof( completionField->buffer ) - completionOffset );

	completionField->cursor = strlen( completionField->buffer );

	if( matchCount == 1 && whitespace )
	{
		Q_strcat( completionField->buffer, sizeof( completionField->buffer ), " " );
		completionField->cursor++;
		return qtrue;
	}

	return qfalse;
}

static void Name_PlayerNameCompletion( const char **names, int nameCount, void(*callback)(const char *s) ) 
{
	int i;

	for( i = 0; i < nameCount; i++ ) {
		callback( names[ i ] );
	}
}

qboolean Com_FieldStringToPlayerName( char *name, int length, const char *rawname )
{
	char		hex[5];
	int			i;
	int			ch;

	if( name == NULL || rawname == NULL )
		return qfalse;

	if( length <= 0 )
		return qtrue;

	for( i = 0; *rawname && i + 1 <= length; rawname++, i++ ) {
		if( *rawname == '\\' ) {
			Q_strncpyz( hex, rawname + 1, sizeof(hex) );
			ch = Com_HexStrToInt( hex );
			if( ch > -1 ) {
				name[i] = ch;
				rawname += 4; //hex string length, 0xXX
			} else {
				name[i] = *rawname;
			}
		} else {
			name[i] = *rawname;
		}
	}
	name[i] = '\0';

	return qtrue;
}

qboolean Com_PlayerNameToFieldString( char *str, int length, const char *name )
{
	const char *p;
	int i;
	int x1, x2;

	if( str == NULL || name == NULL )
		return qfalse;

	if( length <= 0 )
		return qtrue;

	*str = '\0';
	p = name;

	for( i = 0; *p != '\0'; i++, p++ )
	{
		if( i + 1 >= length )
			break;

		if( *p <= ' ' )
		{
			if( i + 5 + 1 >= length )
				break;

			x1 = *p >> 4;
			x2 = *p & 15;

			str[i+0] = '\\';
			str[i+1] = '0';
			str[i+2] = 'x';
			str[i+3] = x1 > 9 ? x1 - 10 + 'a' : x1 + '0';
			str[i+4] = x2 > 9 ? x2 - 10 + 'a' : x2 + '0';

			i += 4;
		} else {
			str[i] = *p;
		}		
	}
	str[i] = '\0';

	return qtrue;
}

void Field_CompletePlayerName( const char **names, int nameCount )
{
	qboolean whitespace;

	matchCount = 0;
	shortestMatch[ 0 ] = 0;

	if( nameCount <= 0 )
		return;

	Name_PlayerNameCompletion( names, nameCount, FindMatches );

	if( completionString[0] == '\0' )
	{
		Com_PlayerNameToFieldString( shortestMatch, sizeof( shortestMatch ), names[ 0 ] );
	}

	//allow to tab player names
	//if full player name switch to next player name
	if( completionString[0] != '\0'
		&& Q_stricmp( shortestMatch, completionString ) == 0 
		&& nameCount > 1 ) 
	{
		int i;

		for( i = 0; i < nameCount; i++ ) {
			if( Q_stricmp( names[ i ], completionString ) == 0 ) 
			{
				i++;
				if( i >= nameCount )
				{
					i = 0;
				}

				Com_PlayerNameToFieldString( shortestMatch, sizeof( shortestMatch ), names[ i ] );
				break;
			}
		}
	}

	if( matchCount > 1 )
	{
		Com_Printf( "]%s\n", completionField->buffer );
		
		Name_PlayerNameCompletion( names, nameCount, PrintMatches );
	}

	whitespace = nameCount == 1? qtrue: qfalse;
	if( !Field_CompletePlayerNameFinal( whitespace ) )
	{

	}
}

int QDECL Com_strCompare( const void *a, const void *b )
{
    const char **pa = (const char **)a;
    const char **pb = (const char **)b;
    return strcmp( *pa, *pb );
}

void Com_InitTargetGameWithType(target_game_e target_game, qboolean bIsDemo)
{
    switch (target_game)
    {
    case TG_MOH:
		if (!bIsDemo) {
			Cvar_Set("com_protocol", va("%i", PROTOCOL_MOH));
			Cvar_Set("com_legacyprotocol", va("%i", PROTOCOL_MOH));
		} else {
			Cvar_Set("com_protocol", va("%i", PROTOCOL_MOH_DEMO));
			Cvar_Set("com_legacyprotocol", va("%i", PROTOCOL_MOH_DEMO));
        }
		protocol_version_demo = protocol_version_full = PROTOCOL_MOH;
		Cvar_Set("com_target_shortversion", va("%s+%s", TARGET_GAME_VERSION_MOH, PRODUCT_VERSION));
		Cvar_Set("com_target_extension", PRODUCT_EXTENSION_MOH);
        Cvar_Set("com_target_version", va("Medal of Honor %s %s", com_target_extension->string, com_target_shortversion->string));
		Cvar_Set("com_gamename", TARGET_GAME_NAME_MOH);
		// "main" is already used as first argument of FS_Startup
		Cvar_Set("fs_basegame", "");
        break;

    case TG_MOHTA:
		if (!bIsDemo) {
			Cvar_Set("com_protocol", va("%i", PROTOCOL_MOHTA));
            Cvar_Set("com_legacyprotocol", va("%i", PROTOCOL_MOHTA));
		} else {
			Cvar_Set("com_protocol", va("%i", PROTOCOL_MOHTA_DEMO));
            Cvar_Set("com_legacyprotocol", va("%i", PROTOCOL_MOHTA_DEMO));
		}
		protocol_version_demo = PROTOCOL_MOHTA_DEMO;
		protocol_version_full = PROTOCOL_MOHTA;
		Cvar_Set("com_target_shortversion", va("%s+%s", TARGET_GAME_VERSION_MOHTA, PRODUCT_VERSION));
		Cvar_Set("com_target_extension", PRODUCT_EXTENSION_MOHTA);
        Cvar_Set("com_target_version", va("Medal of Honor %s %s", com_target_extension->string, com_target_shortversion->string));
		Cvar_Set("com_gamename", TARGET_GAME_NAME_MOHTA);
		if (!bIsDemo) {
			Cvar_Set("fs_basegame", "mainta");
		} else {
			// Targeting a demo
			Cvar_Set("fs_basegame", "demota");
		}
        break;

    case TG_MOHTT:
		// mohta and mohtt use the same protocol version number
		if (!bIsDemo) {
			Cvar_Set("com_protocol", va("%i", PROTOCOL_MOHTA));
            Cvar_Set("com_legacyprotocol", va("%i", PROTOCOL_MOHTA));
            Cvar_Set("com_protocol_alt", va("%i", PROTOCOL_MOHTA_DEMO));
            Cvar_Set("com_target_shortversion", va("%s+%s", TARGET_GAME_VERSION_MOHTT, PRODUCT_VERSION));
		} else {
			Cvar_Set("com_protocol", va("%i", PROTOCOL_MOHTA_DEMO));
            Cvar_Set("com_legacyprotocol", va("%i", PROTOCOL_MOHTA_DEMO));
            Cvar_Set("com_protocol_alt", va("%i", PROTOCOL_MOHTA));
            Cvar_Set("com_target_shortversion", va("%s+%s", TARGET_GAME_VERSION_MOHTT_DEMO, PRODUCT_VERSION));
        }
        protocol_version_demo = PROTOCOL_MOHTA_DEMO;
        protocol_version_full = PROTOCOL_MOHTA;
        Cvar_Set("com_protocol_alt", va("%i", PROTOCOL_MOHTA));
		Cvar_Set("com_target_extension", PRODUCT_EXTENSION_MOHTT);
        Cvar_Set("com_target_version", va("Medal of Honor: %s %s", com_target_extension->string, com_target_shortversion->string));
		Cvar_Set("com_gamename", TARGET_GAME_NAME_MOHTT);
		if (!bIsDemo) {
			Cvar_Set("fs_basegame", "maintt");
		} else {
			// Targeting a demo
			Cvar_Set("fs_basegame", "demott");
		}
        break;

    default:
		Com_Error(ERR_FATAL, "Invalid target game '%d'", target_game);
		return;
	}

	Cvar_Set("protocol", Cvar_VariableString("com_protocol"));
}

/*
===============
Com_InitTargetGame
===============
*/
void Com_InitTargetGame() {
	com_target_game = Cvar_Get("com_target_game", "0", CVAR_INIT|CVAR_PROTECTED);
	com_target_demo = Cvar_Get("com_target_demo", "0", CVAR_INIT|CVAR_PROTECTED);
	com_target_shortversion = Cvar_Get("com_target_shortversion", "0.00", CVAR_ROM);
	com_target_version = Cvar_Get("com_target_version", "", CVAR_ROM);
	com_target_extension = Cvar_Get("com_target_extension", "", CVAR_ROM);

	Com_InitTargetGameWithType((target_game_e)com_target_game->integer, com_target_demo->integer);
}

#ifdef __cplusplus
}
#endif
