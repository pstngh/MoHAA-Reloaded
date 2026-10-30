/*
===========================================================================
Copyright (C) 2026 the MoHAA Reloaded team

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

// cl_discord.c -- Discord Rich Presence
//
// Shows "Playing Medal of Honor: Reloaded" on the player's Discord profile,
// with what they are doing ("Team-Match on mohdm6") and for how long.
//
// It talks to the Discord desktop app over its local IPC socket (a named
// pipe on Windows), so it needs no Discord library. Each message is an
// 8-byte header (opcode, length, little-endian) followed by JSON. The game
// sends a handshake with the application ID, waits for READY, then sends
// SET_ACTIVITY whenever what it shows changes. Closing the socket clears
// the presence. If Discord isn't running, it tries again every 15 seconds
// and otherwise does nothing.

#include "client.h"

#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// The "Medal of Honor: Reloaded" application on the Discord developer portal.
// Its name is what Discord shows after "Playing".
#define DISCORD_APP_ID "1565696304808591452"

#define DISCORD_RETRY_MSEC   15000 // between attempts to reach Discord
#define DISCORD_CHECK_MSEC   1000  // between looks at what the game is doing
#define DISCORD_SEND_MSEC    5000  // Discord allows 5 activity updates per 20 s
#define DISCORD_HEADER_SIZE  8
#define DISCORD_MAX_FRAME    16384

typedef enum {
    OP_HANDSHAKE,
    OP_FRAME,
    OP_CLOSE,
    OP_PING,
    OP_PONG
} discordOp_t;

static cvar_t *cl_discordPresence;

static struct {
#ifdef _WIN32
    HANDLE pipe;
#else
    int fd;
#endif
    qboolean connected;
    qboolean ready;       // READY received: activities may be sent
    int      retryTime;   // when to try to reach Discord again
    int      checkTime;   // when to look at the game state again
    int      sendTime;    // when the next activity may be sent
    char     shown[128];  // what Discord shows now (it allows 128 characters)
    char     wanted[128]; // what it should show
    long long startTime;  // when "wanted" began, for the elapsed time
    int      nonce;

    byte     recv[DISCORD_HEADER_SIZE + DISCORD_MAX_FRAME + 1]; // + a 0 after a payload
    int      recvLen;
} discord;

/*
===============================================================================

Transport

===============================================================================
*/

#ifdef _WIN32

static qboolean Discord_Open(void)
{
    int i;

    for (i = 0; i < 10; i++) {
        HANDLE pipe = CreateFileA(
            va("\\\\.\\pipe\\discord-ipc-%d", i), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL
        );
        if (pipe != INVALID_HANDLE_VALUE) {
            discord.pipe = pipe;
            return qtrue;
        }
    }
    return qfalse;
}

static void Discord_CloseHandle(void)
{
    CloseHandle(discord.pipe);
    discord.pipe = INVALID_HANDLE_VALUE;
}

static qboolean Discord_Write(const void *data, int len)
{
    DWORD written;
    return WriteFile(discord.pipe, data, len, &written, NULL) && (int)written == len;
}

// Returns bytes read, 0 if nothing is waiting, -1 if the pipe is gone.
static int Discord_Read(void *data, int len)
{
    DWORD avail, got;

    if (!PeekNamedPipe(discord.pipe, NULL, 0, NULL, &avail, NULL)) {
        return -1;
    }
    if (!avail) {
        return 0;
    }
    if (avail < (DWORD)len) {
        len = avail;
    }
    if (!ReadFile(discord.pipe, data, len, &got, NULL)) {
        return -1;
    }
    return got;
}

static int Discord_Pid(void)
{
    return (int)GetCurrentProcessId();
}

#else

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

static qboolean Discord_TryPath(const char *path)
{
    struct sockaddr_un addr;
    int                fd;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        return qfalse;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return qfalse;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    Q_strncpyz(addr.sun_path, path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return qfalse;
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif

    discord.fd = fd;
    return qtrue;
}

static qboolean Discord_Open(void)
{
    // Discord puts its socket in the runtime or temp folder; the Flatpak and
    // Snap builds, and Vesktop's Flatpak, put it in a folder of their own.
    static const char *const envs[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    static const char *const subdirs[] = {
        "", "app/com.discordapp.Discord/", "snap.discord/", ".flatpak/dev.vencord.Vesktop/xdg-run/"
    };
    const char *dirs[ARRAY_LEN(envs) + 1];
    int         numDirs = 0;
    int         d, s, i;

    for (i = 0; i < (int)ARRAY_LEN(envs); i++) {
        const char *dir = getenv(envs[i]);
        if (dir && *dir) {
            dirs[numDirs++] = dir;
        }
    }
    dirs[numDirs++] = "/tmp";

    for (d = 0; d < numDirs; d++) {
        for (s = 0; s < (int)ARRAY_LEN(subdirs); s++) {
            for (i = 0; i < 10; i++) {
                const char *dir = dirs[d];
                const char *sep = dir[strlen(dir) - 1] == '/' ? "" : "/";

                if (Discord_TryPath(va("%s%s%sdiscord-ipc-%d", dir, sep, subdirs[s], i))) {
                    return qtrue;
                }
            }
        }
    }
    return qfalse;
}

static void Discord_CloseHandle(void)
{
    close(discord.fd);
    discord.fd = -1;
}

static qboolean Discord_Write(const void *data, int len)
{
    return send(discord.fd, data, len, MSG_NOSIGNAL) == len;
}

// Returns bytes read, 0 if nothing is waiting, -1 if the socket is gone.
static int Discord_Read(void *data, int len)
{
    ssize_t got = recv(discord.fd, data, len, 0);

    if (got > 0) {
        return (int)got;
    }
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    return -1;
}

static int Discord_Pid(void)
{
    return (int)getpid();
}

#endif

static void Discord_Disconnect(void)
{
    if (discord.connected) {
        Discord_CloseHandle();
        Com_DPrintf("Discord: disconnected\n");
    }
    discord.connected = qfalse;
    discord.ready     = qfalse;
    discord.recvLen   = 0;
    discord.shown[0]  = 0;
}

static void Discord_PutLong(byte *p, unsigned int v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static unsigned int Discord_GetLong(const byte *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void Discord_Send(discordOp_t op, const char *json, int len)
{
    byte frame[DISCORD_HEADER_SIZE + DISCORD_MAX_FRAME];

    if (len > DISCORD_MAX_FRAME) {
        return;
    }

    Discord_PutLong(frame, op);
    Discord_PutLong(frame + 4, len);
    memcpy(frame + DISCORD_HEADER_SIZE, json, len);

    if (!Discord_Write(frame, DISCORD_HEADER_SIZE + len)) {
        Discord_Disconnect();
    }
}

static void Discord_Connect(void)
{
    const char *handshake;

    if (!Discord_Open()) {
        return;
    }
    discord.connected = qtrue;
    Com_DPrintf("Discord: connected\n");

    handshake = va("{\"v\":1,\"client_id\":\"%s\"}", DISCORD_APP_ID);
    Discord_Send(OP_HANDSHAKE, handshake, (int)strlen(handshake));
}

// Handles one message from Discord: READY after the handshake, the answers
// to SET_ACTIVITY, a PING to answer, or a CLOSE (a refused handshake).
// The payload is 0-terminated.
static void Discord_HandleFrame(unsigned int op, const char *payload, int len)
{
    switch (op) {
    case OP_FRAME:
        if (!discord.ready) {
            discord.ready = qtrue;
            Com_DPrintf("Discord: ready\n");
        } else if (strstr(payload, "\"evt\":\"ERROR\"")) {
            Com_DPrintf("Discord: refused: %s\n", payload);
        }
        break;
    case OP_PING:
        Discord_Send(OP_PONG, payload, len);
        break;
    case OP_CLOSE:
        Com_DPrintf("Discord: closed: %s\n", payload);
        Discord_Disconnect();
        break;
    default:
        break;
    }
}

static void Discord_Receive(void)
{
    while (discord.connected) {
        int got = Discord_Read(discord.recv + discord.recvLen, DISCORD_HEADER_SIZE + DISCORD_MAX_FRAME - discord.recvLen);

        if (got < 0) {
            Discord_Disconnect();
            return;
        }
        discord.recvLen += got;

        // handle every complete message in the buffer
        while (discord.connected && discord.recvLen >= DISCORD_HEADER_SIZE) {
            unsigned int op      = Discord_GetLong(discord.recv);
            unsigned int len     = Discord_GetLong(discord.recv + 4);
            char        *payload = (char *)discord.recv + DISCORD_HEADER_SIZE;
            int          size    = DISCORD_HEADER_SIZE + len;
            char         next;

            if (len > DISCORD_MAX_FRAME) {
                Discord_Disconnect();
                return;
            }
            if (discord.recvLen < size) {
                break;
            }

            next         = payload[len];
            payload[len] = 0;
            Discord_HandleFrame(op, payload, len);
            payload[len] = next;

            if (discord.connected) {
                discord.recvLen -= size;
                memmove(discord.recv, discord.recv + size, discord.recvLen);
            }
        }

        if (got == 0) {
            return;
        }
    }
}

/*
===============================================================================

Activity

===============================================================================
*/

// Copies a server-supplied string into a JSON string's body. Q_CleanStr
// leaves printable ASCII only, so quotes and backslashes are all that need
// escaping, and the result is always valid UTF-8.
static void Discord_JsonEscape(char *out, int size, const char *in)
{
    char        clean[MAX_STRING_CHARS];
    char       *o   = out;
    char       *end = out + size - 2; // room for an escape and the 0
    const char *c;

    Q_strncpyz(clean, in, sizeof(clean));
    Q_CleanStr(clean);

    for (c = clean; *c && o < end; c++) {
        if (*c == '"' || *c == '\\') {
            *o++ = '\\';
        }
        *o++ = *c;
    }
    *o = 0;
}

// "dm/mohdm6" or "m2l1$spawn" -> "mohdm6" or "m2l1"
static void Discord_MapName(char *out, int size, const char *mapname)
{
    char *cut;

    Q_strncpyz(out, COM_SkipPath(mapname), size);
    cut = strpbrk(out, ".$");
    if (cut) {
        *cut = 0;
    }
}

// What the player is doing, as one line: "Team-Match on mohdm6",
// "Single player on m1l1", "Watching a demo" or "In the menus".
static void Discord_Describe(char *out, int size)
{
    const char *info;
    const char *mode;
    char        map[MAX_QPATH];

    if (clc.state != CA_ACTIVE) {
        Q_strncpyz(out, "In the menus", size);
        return;
    }
    if (clc.demoplaying) {
        Q_strncpyz(out, "Watching a demo", size);
        return;
    }

    info = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];
    Discord_MapName(map, sizeof(map), Info_ValueForKey(info, "mapname"));

    if (!atoi(Info_ValueForKey(info, "g_gametype"))) {
        mode = "Single player";
    } else {
        mode = Info_ValueForKey(info, "g_gametypestring");
        if (!*mode) {
            mode = "Multiplayer";
        }
    }
    Q_strncpyz(out, va("%s on %s", mode, map), size);
}

static void Discord_SendActivity(void)
{
    char details[2 * sizeof(discord.wanted)];
    char json[1024];
    int  len;

    Discord_JsonEscape(details, sizeof(details), discord.wanted);
    len = (int)Com_sprintf(
        json,
        sizeof(json),
        "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%d\",\"args\":{\"pid\":%d,\"activity\":{"
        "\"details\":\"%s\",\"timestamps\":{\"start\":%lld}}}}",
        ++discord.nonce,
        Discord_Pid(),
        details,
        discord.startTime
    );

    Discord_Send(OP_FRAME, json, len);
    if (discord.connected) {
        Q_strncpyz(discord.shown, discord.wanted, sizeof(discord.shown));
    }
}

/*
===============================================================================

Client interface

===============================================================================
*/

void CL_Discord_Init(void)
{
    cl_discordPresence = Cvar_Get("cl_discordPresence", "1", CVAR_ARCHIVE);
    Cvar_SetDescription(cl_discordPresence, "Show what you are playing on your Discord profile.");

#ifdef _WIN32
    discord.pipe = INVALID_HANDLE_VALUE;
#else
    discord.fd = -1;
#endif
    discord.retryTime = discord.checkTime = discord.sendTime = Sys_Milliseconds();
}

void CL_Discord_Frame(void)
{
    int  now = Sys_Milliseconds();
    char wanted[sizeof(discord.wanted)];

    if (!cl_discordPresence->integer) {
        Discord_Disconnect();
        return;
    }

    if (!discord.connected) {
        if (now - discord.retryTime < 0) {
            return;
        }
        discord.retryTime = now + DISCORD_RETRY_MSEC;
        Discord_Connect();
    }

    Discord_Receive();
    if (!discord.ready) {
        return;
    }

    if (now - discord.checkTime >= 0) {
        discord.checkTime = now + DISCORD_CHECK_MSEC;

        Discord_Describe(wanted, sizeof(wanted));
        if (strcmp(wanted, discord.wanted)) {
            Q_strncpyz(discord.wanted, wanted, sizeof(discord.wanted));
            discord.startTime = (long long)time(NULL);
        }
    }

    if (strcmp(discord.wanted, discord.shown) && now - discord.sendTime >= 0) {
        discord.sendTime = now + DISCORD_SEND_MSEC;
        Discord_SendActivity();
    }
}

void CL_Discord_Shutdown(void)
{
    Discord_Disconnect();
}
