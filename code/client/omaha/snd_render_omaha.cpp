/*
===========================================================================
Copyright (C) 2026 Project: Omaha

This file is part of Project: Omaha source code.

Project: Omaha builds upon OpenMoHAA / ioquake3 / F.A.K.K. foundations.
Project: Omaha source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Project: Omaha source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Project: Omaha source code; if not, see COPYING.txt in the
source tree, or write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/*
 * Omaha mixer. Device I/O is miniaudio. Voices resample with a Blackman sinc
 * that widens when downsampling, pan with pairwise speaker gains (stereo, quad,
 * 5.1) or the headphone HRTF, then a shared limiter, EQ and HF duck.
 *
 * Threads. The game thread is the only producer: it owns the id free lists and
 * table growth, and sends lifecycle commands through a single-producer ring
 * (spilling to a game-side list when the ring is full). Voice parameters and the
 * listener go through seqlock mailboxes, latest wins. Whoever holds s_mixGate
 * mixes: the device callback, or the game thread while capturing or while the
 * device is down. The mixer never blocks or allocates; it hands ids and garbage
 * back through a second ring that the game thread drains in Update.
 */

#include "../snd_local.h"
#include "snd_adpcm_omaha.h"
#include "snd_hrtf_omaha.h"
#include "snd_renderer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <deque>
#include <new>
#include <stdint.h>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#	include <immintrin.h>
#	define OM_HAVE_SSE2 1
#else
#	define OM_HAVE_SSE2 0
#endif

#include "miniaudio.h"

#ifndef M_PI
#	define M_PI 3.14159265358979323846
#endif

#define OM_GUARD       4
#define OM_FRAC_ONE    65536u
#define OM_LINE        1024
#define OM_RAMP        128
#define OM_OUT_MAX     6
#define OM_GAIN_MAX    16.0f
#define OM_SILENCE     0.00001f
#define OM_DIRECT_CAP  12
#define OM_HRTF_MIN_DIST 1.0f

#define OM_LIMITER_LOOKAHEAD_MS 1.5f
#define OM_LIMITER_HOLD_MS      2.0f
#define OM_LIMITER_CEILING_DB   (-0.2f)
#define OM_LIMITER_PRE_TRIM_DB  (-8.7f)
#define OM_LIMITER_THRESHOLD_DB (-3.6f)
#define OM_LIMITER_THRESHOLD_MIN_DB (-12.0f)
#define OM_LIMITER_THRESHOLD_MAX_DB 0.0f
#define OM_LIMITER_RELEASE_MS   20.0f
#define OM_LIMITER_MAX_DELAY    512

#define OM_SPK_EQ_BANDS 3
#define OM_HP_EQ_BANDS  4
#define OM_HP_EQ_TYPE_PEAK   0
#define OM_HP_EQ_TYPE_HSHELF 1

#define OM_HF_DUCK_HP_SECTIONS     4
#define OM_HF_DUCK_SHELF_STAGES    4
#define OM_HF_DUCK_FC_HZ           15500.0f
#define OM_HF_DUCK_SC_BOOST_MAX_DB 24.0f
#define OM_HF_DUCK_THRESH_DEFAULT  (-15.0f)
#define OM_HF_DUCK_RELEASE_MS      300.0f
#define OM_HF_DUCK_SHELF_S         1.0f
#define OM_HF_DUCK_DEFAULT_DB      24.0f
#define OM_HF_DUCK_SC_BOOST_DEFAULT 5.0f
#define OM_HF_DUCK_REDESIGN_DB     0.25f
#define OM_HF_DUCK_BYPASS_DB       0.05f

#define OM_OBSTRUCT_CUTOFF_HZ      7000.0f
#define OM_OBSTRUCT_OPEN_HZ        20000.0f
#define OM_OBSTRUCT_OCC_BLOCKED_HZ 10000.0f
#define OM_OBSTRUCT_SMOOTH_MS      80.0f
#define OM_OBSTRUCT_EPSILON        0.01f
#define OM_OBSTRUCT_DIST_RANGE     3000.0f
#define OM_OBSTRUCT_MIX            0.5f
#define OM_OBSTRUCT_MAKEUP_MAX     2.0f
#define OM_OCCLUDE_CUTOFF_HZ       1800.0f
#define OM_OCCLUDE_GAIN            0.35f

#define OM_REAR_GAIN      0.75f
#define OM_REAR_SHELF_HZ  3000.0f
#define OM_REAR_SHELF_HF  0.5f
#define OM_REAR_SMOOTH_MS 50.0f

/* Added in Omaha: id tables start empty and grow by this many slots. A block is never moved. */
#define OM_BLOCK         32
#define OM_CHUNK_BLOCKS  64
#define OM_ID_NONE       (-1)
#define OM_MAX_STEP      8
#define OM_SINC_TAPS     8  /* kernel for steps up to 1 */
#define OM_SINC_MAX_TAPS 64 /* kernel at the step cap */
#define OM_SINC_PHASES   256
#define OM_SINC_BANDS    16 /* step edges spaced geometrically from 1 to OM_MAX_STEP */
#define OM_GATHER        (OM_LINE * OM_MAX_STEP + OM_SINC_MAX_TAPS + 16)

#define OM_CMD_RING       4096 /* power of two */
#define OM_RET_RING       4096 /* power of two */
#define OM_STREAM_CHUNKS  256  /* per stream voice; the host queues at most 128 */
#define OM_DIRECT_HOLD_MS 200.f
#define OM_PARAM_WORDS    ((int)(sizeof(omVoiceParams_t) / sizeof(uint32_t)))
#define OM_LISTENER_WORDS 15

static_assert(sizeof(omVoiceParams_t) % sizeof(uint32_t) == 0, "omVoiceParams_t must be whole words");

enum {
	OM_OP_PLAY = 1,
	OM_OP_OPEN,
	OM_OP_STOP,
	OM_OP_PAUSE,
	OM_OP_RESUME,
	OM_OP_DESTROY,
	OM_OP_SEEK,
	OM_OP_QUEUE,
	OM_OP_UNQUEUE,
	OM_OP_FREE_SOUND,
	OM_OP_RETIRE_SOUND
};

/* What the mixer hands back to the game thread. */
enum {
	OM_RET_HANDLE = 1,
	OM_RET_VOICE,
	OM_RET_SOUND_ID,
	OM_RET_SOUND,
	OM_RET_CHUNK
};

/* Immutable once published. Only the game thread deletes one, after the mixer let go. */
struct omSound_t {
	int                channels;
	int                frames;
	int                rate;
	std::vector<float> planar[2];
	omSound_t         *next; /* mixer: waiting to go back to the game thread */
};

struct omChunk_t {
	int                channels;
	int                frames;
	int                rate;
	std::vector<float> planar[2];
	omChunk_t         *next; /* mixer: waiting to go back to the game thread */
};

/* Mixer thread only. */
struct omVoice_t {
	qboolean        used;
	int             state;
	int             handle; /* -1 once the voice no longer reports to a handle */
	int             sound;
	qboolean        isStream;
	qboolean        primed;
	qboolean        ending; /* input is over or stopped: fade, ring out, free */
	qboolean        allowDirect;
	int             tail; /* HRTF ring-out samples left */
	omVoiceParams_t params;
	int             pos;
	unsigned        frac;
	qboolean        looped; /* passed loopEnd once; reads before loopStart wrap from now on */
	float           dist;
	float           distScale;
	float           dir[3];
	float           spk[2][OM_OUT_MAX];
	float           ambi[OM_AMBI_CHANNELS];
	float           direct;
	float           rear;
	float           rearZ;
	float           lpZ[2];
	float           obstruct;
	float           occlude;
	omHrtfVoice_t  *hrtf;
	qboolean        prevDirect; /* had a direct filter last block */
	int             directHold; /* frames left before a voice that lost its rank is demoted */
	omChunk_t      *chunks[OM_STREAM_CHUNKS]; /* stream queue, oldest at chunkHead */
	int             chunkHead;
	int             chunkCount;
	int             self;
	int             nextFree;
};

/*
 * Game thread: used, stream, the mailbox writes, dirty set. Mixer: voice, dead,
 * state, offset, processed, dirty clear. queued / pendingUnqueue are counters
 * both sides move. nextFree links the game free list, or the mixer return list
 * while dead.
 */
struct omHandle_t {
	std::atomic<int>      used;
	qboolean              stream;
	int                   voice;
	qboolean              dead; /* mixer: freed, waiting to go back on the free list */
	std::atomic<int>      state;
	std::atomic<unsigned> offset;
	std::atomic<int>      queued;
	std::atomic<int>      processed;
	std::atomic<int>      pendingUnqueue;
	std::atomic<unsigned> mailSeq; /* odd while the game thread writes mail */
	std::atomic<uint32_t> mail[OM_PARAM_WORDS];
	std::atomic<int>      dirty;
	int                   nextFree;
};

struct omSoundSlot_t {
	std::atomic<omSound_t *> sound;
	qboolean                 live; /* game thread: created and not yet destroyed */
	int                      nextFree;
};

struct omSoundBlock_t {
	omSoundSlot_t slot[OM_BLOCK];
};

struct omHandleBlock_t {
	omHandle_t slot[OM_BLOCK];
};

struct omVoiceBlock_t {
	omVoice_t slot[OM_BLOCK];
};

/* The game thread links blocks and chunks; the mixer walks them after an acquire of the capacity. */
struct omSoundChunk_t {
	std::atomic<omSoundBlock_t *> block[OM_CHUNK_BLOCKS];
	std::atomic<omSoundChunk_t *> next;
};

struct omHandleChunk_t {
	std::atomic<omHandleBlock_t *> block[OM_CHUNK_BLOCKS];
	std::atomic<omHandleChunk_t *> next;
};

struct omVoiceChunk_t {
	std::atomic<omVoiceBlock_t *> block[OM_CHUNK_BLOCKS];
	std::atomic<omVoiceChunk_t *> next;
};

/* A replaced rank array, freed once the mixer epoch has moved past `epoch`. */
struct omRankRetired_t {
	int             *rank;
	float           *gain;
	unsigned         epoch;
	omRankRetired_t *next;
};

struct omCmd_t {
	int             op;
	int             id;
	int             sound;
	int             slot; /* voice reserved on the game thread; PLAY / OPEN only */
	unsigned        offset;
	omVoiceParams_t params;
	omChunk_t      *chunk;
	omSound_t      *retire;
};

struct omRet_t {
	int   type;
	int   id;
	void *ptr;
};

struct omSincBank_t {
	int                taps;
	int                half; /* taps / 2; the cursor sits at tap half - 1 */
	float              edge; /* largest step this kernel serves */
	std::vector<float> k;    /* [phase][taps] */
};

/* Added in Omaha: cvars the callback reads, copied in Update. */
struct omCvarSnap_t {
	std::atomic<int>   limiter;
	std::atomic<float> limiterThreshDb;
	std::atomic<int>   obstruction;
	std::atomic<int>   hfDuck;
	std::atomic<float> hfDuckDb;
	std::atomic<float> hfDuckThresh;
	std::atomic<float> hfDuckScBoost;
	std::atomic<int>   mix;
};

struct omEq_t {
	float b0, b1, b2, a1, a2;
	float z1[OM_OUT_MAX], z2[OM_OUT_MAX];
};

struct omHp_t {
	float b0, b1, b2, a1, a2;
	float z1, z2;
};

struct omShelf_t {
	float b0, b1, b2, a1, a2;
	float z1[OM_OUT_MAX], z2[OM_OUT_MAX];
};

/* Id tables. Free heads belong to the game thread; return heads to the mixer. */
static std::atomic<omSoundChunk_t *>  s_soundChunks{NULL};
static std::atomic<int>               s_soundCap{0};
static int                            s_soundFreeHead = OM_ID_NONE;
static int                            s_soundIdRetHead = OM_ID_NONE;
static std::atomic<omHandleChunk_t *> s_handleChunks{NULL};
static std::atomic<int>               s_handleCap{0};
static int                            s_handleFreeHead = OM_ID_NONE;
static int                            s_handleRetHead = OM_ID_NONE;
static std::atomic<omVoiceChunk_t *>  s_voiceChunks{NULL};
static std::atomic<int>               s_voiceCap{0};
static int                            s_voiceFreeHead = OM_ID_NONE;
static int                            s_voiceRetHead = OM_ID_NONE;
static omSound_t                     *s_soundRetHead;
static omChunk_t                     *s_chunkRetHead;

/* Game to mixer commands; game-side spill keeps FIFO order when the ring is full. */
static omCmd_t               s_cmdRing[OM_CMD_RING];
static std::atomic<unsigned> s_cmdHead{0};
static std::atomic<unsigned> s_cmdTail{0};
static std::deque<omCmd_t>   s_cmdSpill;

/* Mixer to game returns. */
static omRet_t               s_retRing[OM_RET_RING];
static std::atomic<unsigned> s_retHead{0};
static std::atomic<unsigned> s_retTail{0};

static std::atomic_flag      s_mixGate = ATOMIC_FLAG_INIT;
static std::atomic<unsigned> s_mixEpoch{0}; /* odd while a mix runs */
static omRankRetired_t      *s_rankRetired;

/* Listener mailbox: pos, vel, forward, left, up. */
static std::atomic<unsigned> s_lisSeq{0};
static std::atomic<uint32_t> s_lisMail[OM_LISTENER_WORDS];
static std::atomic<int>      s_lisDirty{0};
static std::atomic<float>    s_lisGainTarget{1.f};
static float                 s_lisPosGame[3]; /* game thread: last position sent */

static omCvarSnap_t       s_cv;
static std::atomic<float> s_hfMeterOut{0.f};
static std::atomic<float> s_hfSideOut{-100.f};

static ma_context s_context;
static qboolean   s_contextReady;
static ma_device  s_device;
static qboolean   s_deviceReady;
static qboolean   s_nullDevice;
static ma_channel s_map[OM_OUT_MAX];

/*
 * Added: on Linux the device opens through PipeWire's ALSA plugin when it can.
 * PipeWire's PulseAudio layer wakes a client at most every 5.3 ms, and every
 * 10.7 ms when it resamples 44.1 kHz; a sound starts at the next wake-up, so
 * shots 100 ms apart were heard 90 to 110 ms apart. The ALSA plugin follows
 * s_period_ms, so a sound starts within one period.
 */
static std::atomic<unsigned> s_underruns{0}; /* ALSA EPIPE on write: the device ran dry */
static qboolean              s_lowLatency;  /* WASAPI granted its low-latency shared mode */
static unsigned              s_underrunsShown;
static int                   s_underrunPrinted;

/*
 * Added in Omaha: device loss recovery. Changed: a device is lost when its
 * callbacks stopped (OM_DEVICE_STALL_MS), not on a stop notification: WASAPI
 * notifies the stop of a reroute it restarts itself, and an unplugged device
 * whose stop failed notifies nothing.
 */
#define OM_DEVICE_STALL_MS 500
static std::atomic<unsigned> s_callbacks{0}; /* device callbacks so far */
static unsigned              s_seenCallbacks;
static int                   s_lastCallbackTime;
static bool                  s_deviceLost;
static sndDeviceConfig_t     s_cfg;
static char                  s_cfgDevice[256];
static int                   s_lastReopen;

static int      s_sampleRate = 44100;
static int      s_outCh      = 2;
static qboolean s_hrtfOn;
static int      s_layout; /* 0 stereo, 1 hrtf, 2 5.1, 3 quad */

static float s_lisPos[3];
static float s_lisVel[3];
static float s_lisFwd[3]  = {1, 0, 0};
static float s_lisLeft[3] = {0, 1, 0};
static float s_lisUp[3]   = {0, 0, 1};
static float s_masterCur  = 1.f;
static qboolean s_masterPrimed;

static float s_bus[OM_OUT_MAX][OM_LINE];
static float s_ambi[OM_AMBI_CHANNELS][OM_LINE];
static float s_dry[2][OM_LINE];
static float s_tmp[OM_LINE];
static float s_gather[OM_GATHER];
static int   s_ambiFlush;
static float s_stash[OM_HRTF_BLOCK * OM_OUT_MAX];
static int   s_stashPos;
static int   s_stashLen;
static std::atomic<int *>   s_rank{NULL};
static std::atomic<float *> s_rankG{NULL};

static float s_spkAz[OM_OUT_MAX];
static int   s_spkCh[OM_OUT_MAX];
static int   s_spkN;
static int   s_frontL;
static int   s_frontR;

static cvar_t *s_period_ms;
static cvar_t *s_limiter;
static cvar_t *s_limiter_threshold;
static cvar_t *s_obstruction;
static cvar_t *s_hf_duck;
static cvar_t *s_hf_duck_db;
static cvar_t *s_hf_duck_thresh;
static cvar_t *s_hf_duck_sc_boost;
static cvar_t *s_mix;

static float s_limDelay[OM_LIMITER_MAX_DELAY][OM_OUT_MAX];
static float s_limPeak[OM_LIMITER_MAX_DELAY];
static int   s_limDelayLen, s_limWrite, s_limFilled, s_limHoldLen, s_limHoldLeft;
static float s_limGain = 1.f, s_limReleaseCoeff = 1.f, s_limSlope;
static float s_limPreTrim = 1.f, s_limThresh = 1.f, s_limMakeup = 1.f, s_limCeiling = 1.f;
static float s_limCalibDb = 1.0e9f;

static omEq_t    s_spkEq[OM_SPK_EQ_BANDS];
static qboolean  s_spkEqReady;
static omEq_t    s_hpEq[OM_HP_EQ_BANDS];
static qboolean  s_hpEqReady;
static omHp_t    s_hfHp[OM_HF_DUCK_HP_SECTIONS];
static omShelf_t s_hfShelf[OM_HF_DUCK_SHELF_STAGES];
static float     s_hfEnv, s_hfGainDb, s_hfRelease, s_hfBoost;
static qboolean  s_hfReady, s_hfShelfOn;
static float     s_rearA = 1.f;
static float     s_rearStep = 1.f; /* 50 ms rear fade for the current mix line */
static omSincBank_t s_sincBank[OM_SINC_BANDS];
static float     s_pairAmbi[2][OM_AMBI_CHANNELS];

struct OmFtzGuard {
	unsigned old;
	bool     on;
	OmFtzGuard() : old(0), on(false)
	{
#if OM_HAVE_SSE2
		old = _mm_getcsr();
		_mm_setcsr(old | 0x8040);
		on = true;
#elif defined(__aarch64__)
		unsigned long fpcr;
		__asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
		old = (unsigned)fpcr;
		__asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ul << 24)));
		on = true;
#endif
	}
	~OmFtzGuard()
	{
#if OM_HAVE_SSE2
		if (on) {
			_mm_setcsr(old);
		}
#elif defined(__aarch64__)
		if (on) {
			__asm__ __volatile__("msr fpcr, %0" : : "r"((unsigned long)old));
		}
#endif
	}
};

static float Om_Clamp(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float Om_Lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

static void Om_DefaultParams(omVoiceParams_t *p)
{
	memset(p, 0, sizeof(*p));
	p->pitch    = 1.f;
	p->minDist  = 200.f;
	p->maxDist  = 12800.f;
	p->loopEnd  = -1;
	p->route    = OM_ROUTE_BUS;
}

static void Om_SetupSpeakers(void)
{
	const float d30  = (float)(30.0 * M_PI / 180.0);
	const float d45  = (float)(45.0 * M_PI / 180.0);
	const float d110 = (float)(110.0 * M_PI / 180.0);
	const float d135 = (float)(135.0 * M_PI / 180.0);
	s_frontL = 0;
	s_frontR = 1;
	if (s_layout == 3) {
		s_outCh = 4;
		s_spkN  = 4;
		s_spkAz[0] = -d135; s_spkCh[0] = 3;
		s_spkAz[1] = -d45;  s_spkCh[1] = 1;
		s_spkAz[2] = d45;   s_spkCh[2] = 0;
		s_spkAz[3] = d135;  s_spkCh[3] = 2;
		s_map[0] = MA_CHANNEL_FRONT_LEFT;
		s_map[1] = MA_CHANNEL_FRONT_RIGHT;
		s_map[2] = MA_CHANNEL_BACK_LEFT;
		s_map[3] = MA_CHANNEL_BACK_RIGHT;
	} else if (s_layout == 2) {
		s_outCh = 6;
		s_spkN  = 5;
		s_spkAz[0] = -d110; s_spkCh[0] = 5;
		s_spkAz[1] = -d30;  s_spkCh[1] = 1;
		s_spkAz[2] = 0.f;   s_spkCh[2] = 2;
		s_spkAz[3] = d30;   s_spkCh[3] = 0;
		s_spkAz[4] = d110;  s_spkCh[4] = 4;
		s_map[0] = MA_CHANNEL_FRONT_LEFT;
		s_map[1] = MA_CHANNEL_FRONT_RIGHT;
		s_map[2] = MA_CHANNEL_FRONT_CENTER;
		s_map[3] = MA_CHANNEL_LFE;
		s_map[4] = MA_CHANNEL_SIDE_LEFT;
		s_map[5] = MA_CHANNEL_SIDE_RIGHT;
	} else {
		s_outCh = 2;
		s_spkN  = 2;
		s_spkAz[0] = -d30; s_spkCh[0] = 1;
		s_spkAz[1] = d30;  s_spkCh[1] = 0;
		s_map[0] = MA_CHANNEL_FRONT_LEFT;
		s_map[1] = MA_CHANNEL_FRONT_RIGHT;
	}
}

static void Om_LimiterReset(void)
{
	memset(s_limDelay, 0, sizeof(s_limDelay));
	memset(s_limPeak, 0, sizeof(s_limPeak));
	s_limWrite    = 0;
	s_limFilled   = 0;
	s_limGain     = 1.f;
	s_limSlope    = 0.f;
	s_limHoldLeft = 0;
	s_limDelayLen = (int)(OM_LIMITER_LOOKAHEAD_MS * 0.001f * (float)s_sampleRate + 0.5f);
	if (s_limDelayLen < 1) {
		s_limDelayLen = 1;
	}
	if (s_limDelayLen > OM_LIMITER_MAX_DELAY) {
		s_limDelayLen = OM_LIMITER_MAX_DELAY;
	}
	s_limHoldLen = (int)(OM_LIMITER_HOLD_MS * 0.001f * (float)s_sampleRate + 0.5f);
	if (s_limHoldLen < 1) {
		s_limHoldLen = 1;
	}
	{
		const float rel = OM_LIMITER_RELEASE_MS * 0.001f * (float)s_sampleRate;
		s_limReleaseCoeff = rel > 1.f ? (1.f - expf(-1.f / rel)) : 1.f;
	}
}

static void Om_LimiterCalib(void)
{
	float db = s_cv.limiterThreshDb.load(std::memory_order_relaxed);
	if (db == s_limCalibDb) {
		return;
	}
	s_limCalibDb = db;
	db           = Om_Clamp(db, OM_LIMITER_THRESHOLD_MIN_DB, OM_LIMITER_THRESHOLD_MAX_DB);
	s_limPreTrim  = powf(10.f, OM_LIMITER_PRE_TRIM_DB * 0.05f);
	s_limThresh   = powf(10.f, (OM_LIMITER_CEILING_DB + db) * 0.05f);
	s_limMakeup   = db >= 0.f ? 1.f : powf(10.f, -db * 0.05f);
	s_limCeiling  = powf(10.f, OM_LIMITER_CEILING_DB * 0.05f);
}

static float Om_WindowPeak(void)
{
	float m = 0.f;
	int   i;
	for (i = 0; i < s_limFilled && i < s_limDelayLen; i++) {
		if (s_limPeak[i] > m) {
			m = s_limPeak[i];
		}
	}
	return m;
}

static void Om_ApplyLimiter(float *interleaved, int frames, int ch)
{
	int i, c;
	if (!s_cv.limiter.load(std::memory_order_relaxed) || s_limDelayLen < 1 || ch < 1) {
		return;
	}
	Om_LimiterCalib();
	for (i = 0; i < frames; i++) {
		float in[OM_OUT_MAX], out[OM_OUT_MAX];
		float peak = 0.f;
		float window, needed;
		for (c = 0; c < ch; c++) {
			in[c] = interleaved[i * ch + c] * s_limPreTrim;
			if (fabsf(in[c]) > peak) {
				peak = fabsf(in[c]);
			}
		}
		if (s_limFilled < s_limDelayLen) {
			for (c = 0; c < ch; c++) {
				s_limDelay[s_limWrite][c] = in[c];
			}
			s_limPeak[s_limWrite] = peak;
			s_limWrite++;
			if (s_limWrite >= s_limDelayLen) {
				s_limWrite = 0;
			}
			s_limFilled++;
			for (c = 0; c < ch; c++) {
				interleaved[i * ch + c] = 0.f;
			}
			continue;
		}
		/* Fixed in Omaha: output the oldest frame before overwriting it; that is the lookahead. */
		for (c = 0; c < ch; c++) {
			out[c]                    = s_limDelay[s_limWrite][c];
			s_limDelay[s_limWrite][c] = in[c];
		}
		s_limPeak[s_limWrite] = peak;
		window = Om_WindowPeak();
		needed = window > s_limThresh ? (s_limThresh / window) : 1.f;
		if (needed < s_limGain) {
			/* Ramp down across the window so the gain is at `needed` when this peak comes out. */
			const float slope = (s_limGain - needed) / (float)s_limDelayLen;
			if (slope > s_limSlope) {
				s_limSlope = slope;
			}
			s_limHoldLeft = s_limHoldLen;
		}
		if (s_limSlope > 0.f) {
			s_limGain -= s_limSlope;
			if (s_limGain <= needed) {
				s_limGain  = needed;
				s_limSlope = 0.f;
			}
		} else if (s_limHoldLeft > 0) {
			s_limHoldLeft--;
		} else {
			s_limGain += (1.f - s_limGain) * s_limReleaseCoeff;
			if (s_limGain > needed) {
				s_limGain = needed;
			}
		}
		for (c = 0; c < ch; c++) {
			float o = out[c] * s_limGain * s_limMakeup;
			interleaved[i * ch + c] = Om_Clamp(o, -s_limCeiling, s_limCeiling);
		}
		s_limWrite++;
		if (s_limWrite >= s_limDelayLen) {
			s_limWrite = 0;
		}
	}
}

static float Om_EqQ(float bwOct)
{
	const float a = powf(2.0f, bwOct);
	return sqrtf(a) / (a - 1.0f);
}

static void Om_DesignPeak(omEq_t *b, float fs, float f0, float gainDb, float bwOct)
{
	const float A     = powf(10.0f, gainDb / 40.0f);
	const float w0    = 2.0f * (float)M_PI * f0 / fs;
	const float cosw  = cosf(w0);
	const float sinw  = sinf(w0);
	const float alpha = sinw / (2.0f * Om_EqQ(bwOct));
	const float a0    = 1.0f + alpha / A;
	b->b0 = (1.0f + alpha * A) / a0;
	b->b1 = (-2.0f * cosw) / a0;
	b->b2 = (1.0f - alpha * A) / a0;
	b->a1 = (-2.0f * cosw) / a0;
	b->a2 = (1.0f - alpha / A) / a0;
	memset(b->z1, 0, sizeof(b->z1));
	memset(b->z2, 0, sizeof(b->z2));
}

static void Om_DesignShelf(omEq_t *b, float fs, float f0, float gainDb, float S)
{
	const float A     = powf(10.0f, gainDb / 40.0f);
	const float w0    = 2.0f * (float)M_PI * Om_Clamp(f0, 10.0f, fs * 0.45f) / fs;
	const float cosw  = cosf(w0);
	const float sinw  = sinf(w0);
	float       alpha, two, b0, b1, b2, a0, a1, a2;
	if (S < 0.1f) {
		S = 0.1f;
	}
	alpha = sinw * 0.5f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
	two   = 2.0f * sqrtf(A) * alpha;
	b0    = A * ((A + 1.0f) + (A - 1.0f) * cosw + two);
	b1    = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw);
	b2    = A * ((A + 1.0f) + (A - 1.0f) * cosw - two);
	a0    = (A + 1.0f) - (A - 1.0f) * cosw + two;
	a1    = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw);
	a2    = (A + 1.0f) - (A - 1.0f) * cosw - two;
	b->b0 = b0 / a0;
	b->b1 = b1 / a0;
	b->b2 = b2 / a0;
	b->a1 = a1 / a0;
	b->a2 = a2 / a0;
	memset(b->z1, 0, sizeof(b->z1));
	memset(b->z2, 0, sizeof(b->z2));
}

static void Om_SpkEqReset(void)
{
	static const float bands[OM_SPK_EQ_BANDS][3] = {
		{101.0f, 5.0f, 1.00f},
		{6000.0f, -2.6f, 0.22f},
		{7000.0f, -2.3f, 0.25f},
	};
	int i;
	if (s_sampleRate <= 0) {
		s_spkEqReady = qfalse;
		return;
	}
	for (i = 0; i < OM_SPK_EQ_BANDS; i++) {
		float f0 = bands[i][0];
		if (f0 >= (float)s_sampleRate * 0.49f) {
			f0 = (float)s_sampleRate * 0.49f;
		}
		Om_DesignPeak(&s_spkEq[i], (float)s_sampleRate, f0, bands[i][1], bands[i][2]);
	}
	s_spkEqReady = qtrue;
}

static void Om_HpEqReset(void)
{
	static const float bands[OM_HP_EQ_BANDS][3] = {
		{100.0f, 3.0f, 1.01f},
		{400.0f, 1.5f, 2.30f},
		{4500.0f, -2.0f, 2.00f},
		{15000.0f, -4.5f, 1.00f},
	};
	static const int types[OM_HP_EQ_BANDS] = {
		OM_HP_EQ_TYPE_PEAK, OM_HP_EQ_TYPE_PEAK, OM_HP_EQ_TYPE_PEAK, OM_HP_EQ_TYPE_HSHELF
	};
	int i;
	if (s_sampleRate <= 0) {
		s_hpEqReady = qfalse;
		return;
	}
	for (i = 0; i < OM_HP_EQ_BANDS; i++) {
		float f0 = bands[i][0];
		if (f0 >= (float)s_sampleRate * 0.49f) {
			f0 = (float)s_sampleRate * 0.49f;
		}
		if (types[i] == OM_HP_EQ_TYPE_HSHELF) {
			Om_DesignShelf(&s_hpEq[i], (float)s_sampleRate, f0, bands[i][1], bands[i][2]);
		} else {
			Om_DesignPeak(&s_hpEq[i], (float)s_sampleRate, f0, bands[i][1], bands[i][2]);
		}
	}
	s_hpEqReady = qtrue;
}

static void Om_RunEq(omEq_t *bands, int nBands, float *interleaved, int frames, int ch)
{
	int i, b, c;
	for (i = 0; i < frames; i++) {
		for (c = 0; c < ch; c++) {
			float x = interleaved[i * ch + c];
			for (b = 0; b < nBands; b++) {
				omEq_t    *band = &bands[b];
				const float y   = band->b0 * x + band->z1[c];
				band->z1[c]     = band->b1 * x - band->a1 * y + band->z2[c];
				band->z2[c]     = band->b2 * x - band->a2 * y;
				x               = y;
			}
			interleaved[i * ch + c] = x;
		}
	}
}

static void Om_HfDesignHp(omHp_t *s, float fs, float fc, float Q)
{
	float w0, cosw, sinw, alpha, a0;
	fc    = Om_Clamp(fc, 10.0f, fs * 0.45f);
	w0    = 2.0f * (float)M_PI * fc / fs;
	cosw  = cosf(w0);
	sinw  = sinf(w0);
	alpha = sinw / (2.0f * Q);
	a0    = 1.0f + alpha;
	s->b0 = ((1.0f + cosw) * 0.5f) / a0;
	s->b1 = (-(1.0f + cosw)) / a0;
	s->b2 = ((1.0f + cosw) * 0.5f) / a0;
	s->a1 = (-2.0f * cosw) / a0;
	s->a2 = (1.0f - alpha) / a0;
	s->z1 = s->z2 = 0.f;
}

static void Om_HfDesignShelf(omShelf_t *s, float fs, float f0, float gainDb)
{
	omEq_t tmp;
	Om_DesignShelf(&tmp, fs, f0, gainDb, OM_HF_DUCK_SHELF_S);
	s->b0 = tmp.b0;
	s->b1 = tmp.b1;
	s->b2 = tmp.b2;
	s->a1 = tmp.a1;
	s->a2 = tmp.a2;
}

static void Om_HfSetShelf(float totalDb)
{
	const float stage = totalDb / (float)OM_HF_DUCK_SHELF_STAGES;
	int         i;
	s_hfGainDb = totalDb;
	if (fabsf(totalDb) < OM_HF_DUCK_BYPASS_DB) {
		s_hfShelfOn = qfalse;
		return;
	}
	s_hfShelfOn = qtrue;
	for (i = 0; i < OM_HF_DUCK_SHELF_STAGES; i++) {
		Om_HfDesignShelf(&s_hfShelf[i], (float)s_sampleRate, OM_HF_DUCK_FC_HZ, stage);
	}
}

static void Om_HfReset(void)
{
	static const float qs[OM_HF_DUCK_HP_SECTIONS] = {0.50979558f, 0.60134489f, 0.89997622f, 2.5629154f};
	int                i;
	s_hfEnv = 0.f;
	s_hfGainDb = 0.f;
	s_hfMeterOut.store(0.f, std::memory_order_relaxed);
	s_hfSideOut.store(-100.f, std::memory_order_relaxed);
	s_hfBoost = 1.f;
	s_hfShelfOn = qfalse;
	for (i = 0; i < OM_HF_DUCK_SHELF_STAGES; i++) {
		memset(s_hfShelf[i].z1, 0, sizeof(s_hfShelf[i].z1));
		memset(s_hfShelf[i].z2, 0, sizeof(s_hfShelf[i].z2));
	}
	if (s_sampleRate <= 0) {
		s_hfReady = qfalse;
		return;
	}
	{
		const float rel = OM_HF_DUCK_RELEASE_MS * 0.001f * (float)s_sampleRate;
		s_hfRelease     = rel > 1.f ? (1.f - expf(-1.f / rel)) : 1.f;
	}
	for (i = 0; i < OM_HF_DUCK_HP_SECTIONS; i++) {
		Om_HfDesignHp(&s_hfHp[i], (float)s_sampleRate, OM_HF_DUCK_FC_HZ, qs[i]);
	}
	Om_HfSetShelf(0.f);
	s_hfReady = qtrue;
}

static float Om_HfHp(float x)
{
	int i;
	for (i = 0; i < OM_HF_DUCK_HP_SECTIONS; i++) {
		omHp_t     *s = &s_hfHp[i];
		const float y = s->b0 * x + s->z1;
		s->z1         = s->b1 * x - s->a1 * y + s->z2;
		s->z2         = s->b2 * x - s->a2 * y;
		x             = y;
	}
	return x;
}

static float Om_HfThresh(void)
{
	float t = s_cv.hfDuckThresh.load(std::memory_order_relaxed);
	if (t > -1.0f) {
		t = -1.0f;
	}
	if (t < -96.0f) {
		t = -96.0f;
	}
	return t;
}

static void Om_ApplyHfDuck(float *interleaved, int frames, int ch)
{
	float maxCut, thresh, range, meterDb = 0.f, sideDb = -100.f;
	int   i, c, stage;
	/* Fixed in Omaha: below about 34 kHz the 15.5 kHz sidechain and shelf land on audible content. */
	if (!s_hfReady || !s_cv.mix.load(std::memory_order_relaxed) || !s_cv.hfDuck.load(std::memory_order_relaxed)
	    || ch < 1 || 0.45f * (float)s_sampleRate < OM_HF_DUCK_FC_HZ) {
		s_hfMeterOut.store(0.f, std::memory_order_relaxed);
		s_hfSideOut.store(-100.f, std::memory_order_relaxed);
		return;
	}
	maxCut = s_cv.hfDuckDb.load(std::memory_order_relaxed);
	if (maxCut < 0.f) {
		maxCut = -maxCut;
	}
	if (maxCut > 24.f) {
		maxCut = 24.f;
	}
	{
		float sc = s_cv.hfDuckScBoost.load(std::memory_order_relaxed);
		sc       = Om_Clamp(sc, 0.f, OM_HF_DUCK_SC_BOOST_MAX_DB);
		s_hfBoost = powf(10.f, sc / 20.f);
	}
	thresh = Om_HfThresh();
	range  = -thresh;
	if (range < 1.f) {
		range = 1.f;
	}
	for (i = 0; i < frames; i++) {
		float mono = 0.f, sc, envDb, t, gainDb;
		for (c = 0; c < ch; c++) {
			mono += interleaved[i * ch + c];
		}
		mono /= (float)ch;
		sc = fabsf(Om_HfHp(mono) * s_hfBoost);
		if (sc >= s_hfEnv) {
			s_hfEnv = sc;
		} else {
			s_hfEnv += s_hfRelease * (sc - s_hfEnv);
		}
		envDb   = 20.0f * log10f(s_hfEnv + 1.0e-20f);
		sideDb  = envDb;
		t       = Om_Clamp((envDb - thresh) / range, 0.f, 1.f);
		gainDb  = maxCut < OM_HF_DUCK_BYPASS_DB ? 0.f : (-t * maxCut);
		meterDb = gainDb;
		if (fabsf(gainDb - s_hfGainDb) >= OM_HF_DUCK_REDESIGN_DB) {
			Om_HfSetShelf(gainDb);
		}
		if (!s_hfShelfOn) {
			continue;
		}
		for (c = 0; c < ch; c++) {
			float sample = interleaved[i * ch + c];
			for (stage = 0; stage < OM_HF_DUCK_SHELF_STAGES; stage++) {
				omShelf_t *s = &s_hfShelf[stage];
				float      y = s->b0 * sample + s->z1[c];
				s->z1[c]     = s->b1 * sample - s->a1 * y + s->z2[c];
				s->z2[c]     = s->b2 * sample - s->a2 * y;
				sample       = y;
			}
			interleaved[i * ch + c] = sample;
		}
	}
	s_hfMeterOut.store(meterDb, std::memory_order_relaxed);
	s_hfSideOut.store(sideDb, std::memory_order_relaxed);
}

static void Om_ApplyMaster(float *interleaved, int frames, int ch)
{
	const float target = s_lisGainTarget.load(std::memory_order_relaxed);
	float       g0     = s_masterPrimed ? s_masterCur : target;
	int         i, c;
	s_masterPrimed = qtrue;
	for (i = 0; i < frames; i++) {
		const float t = frames > 1 ? (float)i / (float)(frames - 1) : 1.f;
		const float g = g0 + (target - g0) * t;
		for (c = 0; c < ch; c++) {
			interleaved[i * ch + c] *= g;
		}
	}
	s_masterCur = target;
}

static void Om_StorePlanar(std::vector<float> planar[2], const float *interleaved, int channels, int frames)
{
	int c, i;
	for (c = 0; c < channels && c < 2; c++) {
		planar[c].assign((size_t)frames + (size_t)OM_GUARD * 2, 0.f);
		for (i = 0; i < frames; i++) {
			planar[c][(size_t)i + OM_GUARD] = interleaved[i * channels + c];
		}
	}
}

static omSound_t *Om_DecodeSound(
	omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign, int frames
)
{
	omPcmDecode_t pcm;
	omSound_t    *s;
	if (!OmPcm_Decode(fmt, data, sizeBytes, blockAlign, frames, &pcm)) {
		return NULL;
	}
	if (pcm.frames <= 0 || pcm.channels < 1) {
		OmPcm_Free(&pcm);
		return NULL;
	}
	s = new (std::nothrow) omSound_t();
	if (s) {
		s->channels = pcm.channels > 2 ? 2 : pcm.channels;
		s->frames   = pcm.frames;
		s->rate     = rate > 0 ? rate : 22050;
		Om_StorePlanar(s->planar, pcm.interleaved, pcm.channels, pcm.frames);
	}
	OmPcm_Free(&pcm);
	return s;
}

static omChunk_t *Om_MakeChunk(omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign)
{
	omPcmDecode_t pcm;
	omChunk_t    *chunk;
	if (!OmPcm_Decode(fmt, data, sizeBytes, blockAlign, 0, &pcm)) {
		return NULL;
	}
	chunk = new (std::nothrow) omChunk_t();
	if (chunk) {
		chunk->channels = pcm.channels > 2 ? 2 : (pcm.channels < 1 ? 1 : pcm.channels);
		chunk->frames   = pcm.frames > 0 ? pcm.frames : 0;
		chunk->rate     = rate > 0 ? rate : 22050;
		if (chunk->frames > 0) {
			Om_StorePlanar(chunk->planar, pcm.interleaved, pcm.channels, pcm.frames);
		}
	}
	OmPcm_Free(&pcm);
	return chunk;
}

static unsigned Om_StepFor(int srcRate, float pitch)
{
	float adv;
	long  step;
	if (srcRate <= 0) {
		srcRate = s_sampleRate;
	}
	if (pitch < 0.001f) {
		pitch = 0.001f;
	}
	adv = ((float)srcRate / (float)s_sampleRate) * pitch;
	/* Changed in Omaha: a supersonic Doppler ratio is the step cap, not an infinite pitch. */
	if (adv > (float)OM_MAX_STEP) {
		adv = (float)OM_MAX_STEP;
	}
	step = lrintf(adv * (float)OM_FRAC_ONE);
	if (step < 1) {
		step = 1;
	}
	if (step > (long)(OM_MAX_STEP * OM_FRAC_ONE)) {
		step = (long)(OM_MAX_STEP * OM_FRAC_ONE);
	}
	return (unsigned)step;
}

static float Om_Sinc(float x)
{
	float pix;
	if (fabsf(x) < 1.0e-8f) {
		return 1.f;
	}
	pix = (float)M_PI * x;
	return sinf(pix) / pix;
}

/*
 * Changed in Omaha: Blackman sinc banks. Band 0 is the 8-point kernel for steps
 * up to 1. Wider steps (downsampling, high pitch, Doppler) get a kernel with
 * cutoff 1 / edge and about 8 * edge taps, so they do not alias.
 */
static void Om_BuildSinc(void)
{
	int band, p, i;
	for (band = 0; band < OM_SINC_BANDS; band++) {
		omSincBank_t *bk   = &s_sincBank[band];
		const float   edge = powf((float)OM_MAX_STEP, (float)band / (float)(OM_SINC_BANDS - 1));
		const float   cut  = 1.f / edge;
		int           taps = (int)ceilf((float)OM_SINC_TAPS * edge);
		taps               = (taps + 1) & ~1;
		if (taps < OM_SINC_TAPS) {
			taps = OM_SINC_TAPS;
		}
		if (taps > OM_SINC_MAX_TAPS) {
			taps = OM_SINC_MAX_TAPS;
		}
		bk->taps = taps;
		bk->half = taps / 2;
		bk->edge = edge;
		bk->k.assign((size_t)OM_SINC_PHASES * (size_t)taps, 0.f);
		for (p = 0; p < OM_SINC_PHASES; p++) {
			const float frac = (float)p / (float)OM_SINC_PHASES;
			float      *k    = &bk->k[(size_t)p * (size_t)taps];
			float       sum  = 0.f;
			for (i = 0; i < taps; i++) {
				const float offset = (float)i - (float)(bk->half - 1) - frac;
				const float wpos   = (offset + (float)bk->half) / (float)taps;
				float       w      = 0.f;
				if (wpos > 0.f && wpos < 1.f) {
					const float twopi = (float)(2.0 * M_PI);
					w = 0.42f - 0.5f * cosf(twopi * wpos) + 0.08f * cosf(2.f * twopi * wpos);
				}
				k[i] = cut * Om_Sinc(offset * cut) * w;
				sum += k[i];
			}
			if (sum != 0.f) {
				for (i = 0; i < taps; i++) {
					k[i] /= sum;
				}
			}
		}
	}
}

static const omSincBank_t *Om_SincFor(unsigned step)
{
	const float st = (float)step / (float)OM_FRAC_ONE;
	int         band;
	for (band = 0; band < OM_SINC_BANDS - 1; band++) {
		if (st <= s_sincBank[band].edge * 1.0001f) {
			break;
		}
	}
	return &s_sincBank[band];
}

static void Om_BuildStereoPair(void)
{
	const float c30 = cosf((float)(M_PI / 6.0));
	const float s30 = sinf((float)(M_PI / 6.0));
	float       dir[3];
	dir[0] = c30;
	dir[1] = s30;
	dir[2] = 0.f;
	OmHrtf_AmbiCoeffs(dir, s_pairAmbi[0]);
	dir[1] = -s30;
	OmHrtf_AmbiCoeffs(dir, s_pairAmbi[1]);
}

static float Om_StaticAt(const float *s, int frames, int i, qboolean loop, int a, int b, qboolean looped);

static float Om_SincAt(
	const omSincBank_t *bk, unsigned phase, const float *s, int frames, int pos, qboolean loop, int a, int b,
	qboolean looped
)
{
	const float *k   = &bk->k[(size_t)phase * (size_t)bk->taps];
	const int    at  = pos - (bk->half - 1);
	float        sum = 0.f;
	int          t;
	for (t = 0; t < bk->taps; t++) {
		sum += k[t] * Om_StaticAt(s, frames, at + t, loop, a, b, looped);
	}
	return sum;
}

static void Om_LoopRange(const omVoice_t *v, int frames, int *a, int *b)
{
	*a = v->params.loopStart > 0 ? v->params.loopStart : 0;
	*b = (v->params.loopEnd > 0 && v->params.loopEnd <= frames) ? v->params.loopEnd : frames;
	if (*a >= *b) {
		*a = 0;
		*b = frames;
	}
}

static int Om_Wrap(int i, int a, int b)
{
	int len = b - a;
	int r;
	if (len <= 0) {
		return a;
	}
	r = (i - a) % len;
	if (r < 0) {
		r += len;
	}
	return a + r;
}

/*
 * Fixed in Omaha: a loop block plays its intro. Reads past loopEnd wrap into the
 * loop; reads before loopStart wrap only once the cursor has looped.
 */
static float Om_StaticAt(const float *s, int frames, int i, qboolean loop, int a, int b, qboolean looped)
{
	if (loop && (i >= b || (looped && i < a))) {
		i = Om_Wrap(i, a, b);
	} else if (i < 0 || i >= frames) {
		return 0.f;
	}
	return s[i];
}

static void Om_ResamplePtr(
	const omSincBank_t *bk, const float *s, int frames, int *pos, unsigned *frac, unsigned step, qboolean loop, int a,
	int b, qboolean *looped, float *dst, int count
)
{
	int i;
	if (step == OM_FRAC_ONE && *frac == 0 && *pos >= 0 && (!loop || *pos + count <= b)) {
		for (i = 0; i < count; i++) {
			dst[i] = Om_StaticAt(s, frames, *pos + i, loop, a, b, *looped);
		}
		*pos += count;
		if (loop && *pos >= b) {
			*pos    = Om_Wrap(*pos, a, b);
			*looped = qtrue;
		}
		return;
	}
	for (i = 0; i < count; i++) {
		dst[i] = Om_SincAt(bk, *frac >> 8, s, frames, *pos, loop, a, b, *looped);
		*frac += step;
		*pos += (int)(*frac >> 16);
		*frac &= OM_FRAC_ONE - 1;
		if (loop && *pos >= b) {
			*pos    = Om_Wrap(*pos, a, b);
			*looped = qtrue;
		}
	}
}

static const omChunk_t *Om_StreamChunk(const omVoice_t *v, int i)
{
	return v->chunks[(v->chunkHead + i) % OM_STREAM_CHUNKS];
}

/* dst[k] = stream frame (start + k) across the chunk queue, zero outside it. */
static void Om_StreamGather(const omVoice_t *v, int ch, int start, int n, float *dst)
{
	int k    = 0;
	int base = 0;
	int i;
	while (k < n && start + k < 0) {
		dst[k++] = 0.f;
	}
	for (i = 0; i < v->chunkCount && k < n; i++) {
		const omChunk_t *c = Om_StreamChunk(v, i);
		const int        f = start + k - base;
		if (f < c->frames) {
			const int    cc   = ch < c->channels ? ch : 0;
			int          take = c->frames - f;
			if (take > n - k) {
				take = n - k;
			}
			memcpy(dst + k, c->planar[cc].data() + OM_GUARD + f, sizeof(float) * (size_t)take);
			k += take;
		}
		base += c->frames;
	}
	while (k < n) {
		dst[k++] = 0.f;
	}
}

/* g[0] is half - 1 frames before the cursor (first sinc tap). Returns how many frames the cursor moved. */
static int Om_ResampleRun(const omSincBank_t *bk, const float *g, unsigned *frac, unsigned step, float *dst, int count)
{
	unsigned f  = *frac;
	int      ip = 0;
	int      i, t;
	if (step == OM_FRAC_ONE && f == 0) {
		memcpy(dst, g + (bk->half - 1), sizeof(float) * (size_t)count);
		return count;
	}
	for (i = 0; i < count; i++) {
		const float *k   = &bk->k[(size_t)(f >> 8) * (size_t)bk->taps];
		float        sum = 0.f;
		for (t = 0; t < bk->taps; t++) {
			sum += k[t] * g[ip + t];
		}
		dst[i] = sum;
		f += step;
		ip += (int)(f >> 16);
		f &= OM_FRAC_ONE - 1;
	}
	*frac = f;
	return ip;
}

static int Om_StreamFrames(const omVoice_t *v)
{
	int n = 0;
	int i;
	for (i = 0; i < v->chunkCount; i++) {
		n += Om_StreamChunk(v, i)->frames;
	}
	return n;
}

/*
 * Added in Omaha: id tables grow by blocks on the game thread. A block is never
 * moved or freed before shutdown; the mixer only reads a published capacity.
 */
template <typename Block, typename Chunk>
static Block *Om_BlockAt(const std::atomic<Chunk *> &head, int blockIndex)
{
	int    chunkIndex = blockIndex / OM_CHUNK_BLOCKS;
	Chunk *c          = head.load(std::memory_order_acquire);
	while (c && chunkIndex-- > 0) {
		c = c->next.load(std::memory_order_acquire);
	}
	return c ? c->block[blockIndex % OM_CHUNK_BLOCKS].load(std::memory_order_acquire) : NULL;
}

static omSoundSlot_t *Om_SoundSlot(int id)
{
	omSoundBlock_t *b;
	if (id < 0 || id >= s_soundCap.load(std::memory_order_acquire)) {
		return NULL;
	}
	b = Om_BlockAt<omSoundBlock_t>(s_soundChunks, id / OM_BLOCK);
	return b ? &b->slot[id % OM_BLOCK] : NULL;
}

static omHandle_t *Om_HandleAt(int id)
{
	omHandleBlock_t *b;
	if (id < 0 || id >= s_handleCap.load(std::memory_order_acquire)) {
		return NULL;
	}
	b = Om_BlockAt<omHandleBlock_t>(s_handleChunks, id / OM_BLOCK);
	return b ? &b->slot[id % OM_BLOCK] : NULL;
}

static omVoice_t *Om_VoiceAt(int id)
{
	omVoiceBlock_t *b;
	if (id < 0 || id >= s_voiceCap.load(std::memory_order_acquire)) {
		return NULL;
	}
	b = Om_BlockAt<omVoiceBlock_t>(s_voiceChunks, id / OM_BLOCK);
	return b ? &b->slot[id % OM_BLOCK] : NULL;
}

template <typename Fn>
static void Om_ForEachVoice(int cap, Fn fn)
{
	int             left = cap;
	omVoiceChunk_t *c;
	for (c = s_voiceChunks.load(std::memory_order_acquire); c && left > 0; c = c->next.load(std::memory_order_acquire)) {
		int b;
		for (b = 0; b < OM_CHUNK_BLOCKS && left > 0; b++) {
			omVoiceBlock_t *blk = c->block[b].load(std::memory_order_acquire);
			int n = left > OM_BLOCK ? OM_BLOCK : left;
			int s;
			left -= n;
			if (!blk) {
				continue;
			}
			for (s = 0; s < n; s++) {
				fn(&blk->slot[s]);
			}
		}
	}
}

/* Game thread. init fills the new block; it is linked, then the capacity is published. */
template <typename Block, typename Chunk, typename Init>
static qboolean Om_GrowTable(std::atomic<Chunk *> &head, std::atomic<int> &capVar, Init init)
{
	const int cap        = capVar.load(std::memory_order_relaxed);
	const int blockIndex = cap / OM_BLOCK;
	int       chunkIndex = blockIndex / OM_CHUNK_BLOCKS;
	Chunk    *c          = head.load(std::memory_order_relaxed);
	Chunk    *prev       = NULL;
	Block    *b;
	int       i;
	while (c && chunkIndex-- > 0) {
		prev = c;
		c    = c->next.load(std::memory_order_relaxed);
	}
	if (!c) {
		c = new (std::nothrow) Chunk();
		if (!c) {
			return qfalse;
		}
		for (i = 0; i < OM_CHUNK_BLOCKS; i++) {
			c->block[i].store(NULL, std::memory_order_relaxed);
		}
		c->next.store(NULL, std::memory_order_relaxed);
		if (prev) {
			prev->next.store(c, std::memory_order_release);
		} else {
			head.store(c, std::memory_order_release);
		}
	}
	b = new (std::nothrow) Block();
	if (!b) {
		return qfalse;
	}
	init(b, cap);
	c->block[blockIndex % OM_CHUNK_BLOCKS].store(b, std::memory_order_release);
	capVar.store(cap + OM_BLOCK, std::memory_order_release);
	return qtrue;
}

/* Shutdown only, with no mixer running. */
template <typename Block, typename Chunk, typename Fn>
static void Om_FreeTable(std::atomic<Chunk *> &head, std::atomic<int> &capVar, Fn onBlock)
{
	Chunk *c = head.exchange(NULL, std::memory_order_relaxed);
	while (c) {
		Chunk *next = c->next.load(std::memory_order_relaxed);
		int    b;
		for (b = 0; b < OM_CHUNK_BLOCKS; b++) {
			Block *blk = c->block[b].load(std::memory_order_relaxed);
			if (blk) {
				onBlock(blk);
				delete blk;
			}
		}
		delete c;
		c = next;
	}
	capVar.store(0, std::memory_order_relaxed);
}

static void Om_InitVoiceSlot(omVoice_t *v, int id);

static qboolean Om_GrowSounds(void)
{
	return Om_GrowTable<omSoundBlock_t>(s_soundChunks, s_soundCap, [](omSoundBlock_t *b, int cap) {
		int i;
		for (i = 0; i < OM_BLOCK; i++) {
			b->slot[i].sound.store(NULL, std::memory_order_relaxed);
			b->slot[i].live     = qfalse;
			b->slot[i].nextFree = (i + 1 < OM_BLOCK) ? cap + i + 1 : s_soundFreeHead;
		}
		s_soundFreeHead = cap;
	});
}

static qboolean Om_GrowHandles(void)
{
	return Om_GrowTable<omHandleBlock_t>(s_handleChunks, s_handleCap, [](omHandleBlock_t *b, int cap) {
		int i, w;
		for (i = 0; i < OM_BLOCK; i++) {
			omHandle_t *h = &b->slot[i];
			h->used.store(0, std::memory_order_relaxed);
			h->stream = qfalse;
			h->voice  = -1;
			h->dead   = qfalse;
			h->state.store(OM_VOICE_STOPPED, std::memory_order_relaxed);
			h->offset.store(0, std::memory_order_relaxed);
			h->queued.store(0, std::memory_order_relaxed);
			h->processed.store(0, std::memory_order_relaxed);
			h->pendingUnqueue.store(0, std::memory_order_relaxed);
			h->mailSeq.store(0, std::memory_order_relaxed);
			for (w = 0; w < OM_PARAM_WORDS; w++) {
				h->mail[w].store(0, std::memory_order_relaxed);
			}
			h->dirty.store(0, std::memory_order_relaxed);
			h->nextFree = (i + 1 < OM_BLOCK) ? cap + i + 1 : s_handleFreeHead;
		}
		s_handleFreeHead = cap;
	});
}

/* Game thread: free rank arrays the mixer can no longer be reading. */
static void Om_FreeRetiredRanks(qboolean force)
{
	const unsigned    now = s_mixEpoch.load(std::memory_order_seq_cst);
	omRankRetired_t **pp  = &s_rankRetired;
	while (*pp) {
		omRankRetired_t *r    = *pp;
		const unsigned   need = (r->epoch & 1u) ? r->epoch + 1u : r->epoch;
		if (force || (int)(now - need) >= 0) {
			*pp = r->next;
			delete[] r->rank;
			delete[] r->gain;
			delete r;
		} else {
			pp = &r->next;
		}
	}
}

static qboolean Om_GrowVoices(void)
{
	const int        newCap = s_voiceCap.load(std::memory_order_relaxed) + OM_BLOCK;
	int             *rank   = new (std::nothrow) int[newCap];
	float           *gain   = new (std::nothrow) float[newCap];
	omRankRetired_t *old    = new (std::nothrow) omRankRetired_t();
	int             *oldRank = NULL;
	float           *oldGain = NULL;
	if (!rank || !gain || !old) {
		delete[] rank;
		delete[] gain;
		delete old;
		return qfalse;
	}
	if (!Om_GrowTable<omVoiceBlock_t>(s_voiceChunks, s_voiceCap, [&](omVoiceBlock_t *b, int cap) {
		    int i;
		    for (i = 0; i < OM_BLOCK; i++) {
			    Om_InitVoiceSlot(&b->slot[i], cap + i);
			    b->slot[i].nextFree = (i + 1 < OM_BLOCK) ? cap + i + 1 : s_voiceFreeHead;
		    }
		    s_voiceFreeHead = cap;
		    /* The rank arrays must cover the new capacity before it is published. */
		    oldRank = s_rank.exchange(rank, std::memory_order_seq_cst);
		    oldGain = s_rankG.exchange(gain, std::memory_order_seq_cst);
	    })) {
		delete[] rank;
		delete[] gain;
		delete old;
		return qfalse;
	}
	if (oldRank || oldGain) {
		/* An odd epoch means a mix may still hold the old arrays; it ends at epoch + 1. */
		old->rank     = oldRank;
		old->gain     = oldGain;
		old->epoch    = s_mixEpoch.load(std::memory_order_seq_cst);
		old->next     = s_rankRetired;
		s_rankRetired = old;
	} else {
		delete old;
	}
	return qtrue;
}

/* Game thread. */
static int Om_TakeSound(void)
{
	int id;
	omSoundSlot_t *s;
	if (s_soundFreeHead == OM_ID_NONE && !Om_GrowSounds()) {
		return OM_SOUND_NONE;
	}
	id = s_soundFreeHead;
	s = Om_SoundSlot(id);
	if (!s) {
		return OM_SOUND_NONE;
	}
	s_soundFreeHead = s->nextFree;
	s->nextFree = OM_ID_NONE;
	return id;
}

/* Game thread. */
static int Om_TakeHandle(void)
{
	int id;
	omHandle_t *h;
	if (s_handleFreeHead == OM_ID_NONE && !Om_GrowHandles()) {
		return OM_VOICE_NONE;
	}
	id = s_handleFreeHead;
	h = Om_HandleAt(id);
	if (!h) {
		return OM_VOICE_NONE;
	}
	s_handleFreeHead = h->nextFree;
	h->nextFree = OM_ID_NONE;
	return id;
}

/* Game thread. The slot stays unused until the mixer binds it. */
static int Om_TakeVoice(void)
{
	int id;
	omVoice_t *v;
	if (s_voiceFreeHead == OM_ID_NONE && !Om_GrowVoices()) {
		return OM_ID_NONE;
	}
	id = s_voiceFreeHead;
	v = Om_VoiceAt(id);
	if (!v) {
		return OM_ID_NONE;
	}
	s_voiceFreeHead = v->nextFree;
	v->nextFree = OM_ID_NONE;
	return id;
}

/* Game thread: the mixer is done with this handle (or never saw it). */
static void Om_ReturnHandle(int id)
{
	omHandle_t *h = Om_HandleAt(id);
	if (!h) {
		return;
	}
	h->used.store(0, std::memory_order_relaxed);
	h->dead = qfalse;
	h->voice = -1;
	h->nextFree = s_handleFreeHead;
	s_handleFreeHead = id;
}

static const omSound_t *Om_VoiceSound(const omVoice_t *v)
{
	omSoundSlot_t *s;
	if (v->isStream || v->sound < 0) {
		return NULL;
	}
	s = Om_SoundSlot(v->sound);
	return s ? s->sound.load(std::memory_order_acquire) : NULL;
}

static int Om_VoiceChannels(const omVoice_t *v)
{
	const omSound_t *s;
	if (v->isStream) {
		return v->chunkCount ? Om_StreamChunk(v, 0)->channels : 0;
	}
	s = Om_VoiceSound(v);
	return s ? s->channels : 0;
}

static void Om_DistanceGain(omVoice_t *v)
{
	const omVoiceParams_t *p = &v->params;
	float                  rel[3], front, left, up, d;
	v->distScale = 1.f;
	v->dist      = 0.f;
	v->dir[0]    = 1.f;
	v->dir[1]    = 0.f;
	v->dir[2]    = 0.f;
	/* Changed in Omaha: Miles pan sits on the unit circle behind the listener (AL +Z). */
	if (p->flags & OM_FLAG_PAN) {
		const float pan = Om_Clamp(p->pan, -1.f, 1.f);
		const float z   = sqrtf(fmaxf(0.f, 1.f - pan * pan));
		v->dir[0]       = -z;
		v->dir[1]       = -pan;
		v->dir[2]       = 0.f;
		v->dist         = 1.f;
		return;
	}
	if (p->flags & OM_FLAG_2D) {
		return;
	}
	rel[0] = p->pos[0] - s_lisPos[0];
	rel[1] = p->pos[1] - s_lisPos[1];
	rel[2] = p->pos[2] - s_lisPos[2];
	front  = rel[0] * s_lisFwd[0] + rel[1] * s_lisFwd[1] + rel[2] * s_lisFwd[2];
	left   = rel[0] * s_lisLeft[0] + rel[1] * s_lisLeft[1] + rel[2] * s_lisLeft[2];
	up     = rel[0] * s_lisUp[0] + rel[1] * s_lisUp[1] + rel[2] * s_lisUp[2];
	d      = sqrtf(front * front + left * left + up * up);
	v->dist = d;
	if (d > 0.0001f) {
		v->dir[0] = front / d;
		v->dir[1] = left / d;
		v->dir[2] = up / d;
	}
	if (p->minDist > 0.f) {
		const float c = Om_Clamp(d, p->minDist, p->maxDist > p->minDist ? p->maxDist : p->minDist);
		v->distScale  = p->minDist / c;
	}
}

/*
 * Lateral image (+1 left .. -1 right), horizontal weight and azimuth. The image
 * is the sine of (pi/2 * lateral share) scaled by the horizontal length, so a
 * source above or below the head collapses to the centre instead of flipping
 * sides on tiny horizontal offsets.
 */
static void Om_Image(const omVoice_t *v, float *img, float *horiz, float *az)
{
	float h, x;
	h = sqrtf(v->dir[0] * v->dir[0] + v->dir[1] * v->dir[1]);
	if (h < 1.0e-4f) {
		*img   = 0.f;
		*horiz = 0.f;
		*az    = 0.f;
		return;
	}
	x      = Om_Clamp(v->dir[1] / h, -1.f, 1.f);
	*img   = sinf((float)(M_PI * 0.5) * x) * h;
	*horiz = h > 1.f ? 1.f : h;
	*az    = atan2f(v->dir[1], v->dir[0]);
}

static void Om_StereoGains(float img, float *gL, float *gR)
{
	const float th = (1.f - Om_Clamp(img, -1.f, 1.f)) * (float)(M_PI * 0.25);
	*gL            = cosf(th);
	*gR            = sinf(th);
}

/*
 * Changed in Omaha: two-speaker image from main. Lateral angle is (pi/2) * left/h,
 * encoded as first-order W/Y/X and decoded with the stereo matrix. h scales the
 * encoded direction, so a source overhead collapses to W instead of flipping sides.
 */
static void Om_FoaStereo(const float dir[3], float *gL, float *gR)
{
	const float sqrt3 = 1.73205080757f;
	const float h     = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
	float       side, front, Y, X;
	if (h < 1.0e-4f) {
		side  = 0.f;
		front = 0.f;
	} else {
		const float x     = Om_Clamp(dir[1] / h, -1.f, 1.f);
		const float angle = (float)(M_PI * 0.5f) * x;
		side              = sinf(angle) * h;
		front             = cosf(angle) * h;
	}
	Y   = sqrt3 * side;
	X   = sqrt3 * front;
	*gL = 0.5f + 0.288675135f * Y + 0.0552305643f * X;
	*gR = 0.5f - 0.288675135f * Y + 0.0552305643f * X;
}

/* Changed in Omaha: main's Doppler. Speeds are world-space and already scaled. */
static float Om_Doppler(const omVoice_t *v)
{
	const float c = 343.3f;
	float       to[3], d, vss, vls;
	int         i;
	to[0] = v->params.pos[0] - s_lisPos[0];
	to[1] = v->params.pos[1] - s_lisPos[1];
	to[2] = v->params.pos[2] - s_lisPos[2];
	d     = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
	if (d < 1.0e-4f) {
		return 1.f;
	}
	for (i = 0; i < 3; i++) {
		to[i] /= d;
	}
	vss = -(v->params.vel[0] * to[0] + v->params.vel[1] * to[1] + v->params.vel[2] * to[2]);
	vls = -(s_lisVel[0] * to[0] + s_lisVel[1] * to[1] + s_lisVel[2] * to[2]);
	if (!(vls < c)) {
		return 0.f;
	}
	if (!(vss < c)) {
		return 1.0e6f;
	}
	return (c - vls) / (c - vss);
}

/* Pairwise gains on the speaker ring, spread evenly as the source goes overhead. */
static void Om_RingGains(float az, float horiz, float g[OM_OUT_MAX])
{
	int i;
	memset(g, 0, sizeof(float) * OM_OUT_MAX);
	for (i = 0; i < s_spkN; i++) {
		const int j   = (i + 1) % s_spkN;
		const float a0 = s_spkAz[i];
		float       a1 = s_spkAz[j];
		float       an = az;
		if (j == 0) {
			a1 += (float)(2.0 * M_PI);
		}
		if (an < a0) {
			an += (float)(2.0 * M_PI);
		}
		if (an >= a0 && an <= a1 && a1 > a0) {
			const float t = (an - a0) / (a1 - a0);
			g[s_spkCh[i]] = cosf(t * (float)(M_PI * 0.5));
			g[s_spkCh[j]] = sinf(t * (float)(M_PI * 0.5));
			break;
		}
	}
	if (horiz < 1.f) {
		const float u = (1.f - horiz) / sqrtf((float)s_spkN);
		float       e = 0.f;
		for (i = 0; i < s_spkN; i++) {
			g[s_spkCh[i]] = g[s_spkCh[i]] * horiz + u;
			e += g[s_spkCh[i]] * g[s_spkCh[i]];
		}
		if (e > 1.0e-6f) {
			e = 1.f / sqrtf(e);
			for (i = 0; i < s_spkN; i++) {
				g[s_spkCh[i]] *= e;
			}
		}
	}
}

/* Mix `in` into `out`, gain moving linearly from `from` to `to` across the block. */
static void Om_AddGain(const float *in, float *out, float from, float to, int count)
{
	int i;
	if (fabsf(from) <= OM_SILENCE && fabsf(to) <= OM_SILENCE) {
		return;
	}
	if (from == to) {
		for (i = 0; i < count; i++) {
			out[i] += in[i] * to;
		}
		return;
	}
	{
		const float step = (to - from) / (float)count;
		float       g    = from;
		for (i = 0; i < count; i++) {
			g += step;
			out[i] += in[i] * g;
		}
	}
}

static void Om_Filter(float *buf, int count, float a, float *z)
{
	int   i;
	float s = *z;
	for (i = 0; i < count; i++) {
		s += (buf[i] - s) * a;
		buf[i] = s;
	}
	*z = s;
}

static void Om_Rear(float *buf, int count, float rear, float *z)
{
	int   i;
	float s  = *z;
	float hf = Om_Lerp(1.f, OM_REAR_SHELF_HF, rear);
	if (rear <= 0.001f) {
		*z = count > 0 ? buf[count - 1] : s;
		return;
	}
	for (i = 0; i < count; i++) {
		s += (buf[i] - s) * s_rearA;
		buf[i] = s + hf * (buf[i] - s);
	}
	*z = s;
}

/* Mixer: garbage and ids wait on intrusive lists until the return ring has room. */
static void Om_RetChunk(omChunk_t *c)
{
	if (c) {
		c->next        = s_chunkRetHead;
		s_chunkRetHead = c;
	}
}

static void Om_RetSound(omSound_t *snd)
{
	if (snd) {
		snd->next      = s_soundRetHead;
		s_soundRetHead = snd;
	}
}

static void Om_FreeHrtf(omVoice_t *v)
{
	if (v->hrtf) {
		OmHrtf_VoiceFree(v->hrtf);
		v->hrtf = NULL;
	}
	v->tail = 0;
}

static void Om_ClearChunks(omVoice_t *v)
{
	while (v->chunkCount > 0) {
		Om_RetChunk(v->chunks[v->chunkHead]);
		v->chunks[v->chunkHead] = NULL;
		v->chunkHead            = (v->chunkHead + 1) % OM_STREAM_CHUNKS;
		v->chunkCount--;
	}
	v->chunkHead = 0;
}

static void Om_ResetVoice(omVoice_t *v)
{
	Om_FreeHrtf(v);
	Om_ClearChunks(v);
	v->used        = qfalse;
	v->state       = OM_VOICE_STOPPED;
	v->handle      = -1;
	v->sound       = -1;
	v->isStream    = qfalse;
	v->primed      = qfalse;
	v->ending      = qfalse;
	v->allowDirect = qfalse;
	v->prevDirect  = qfalse;
	v->directHold  = 0;
	v->pos         = 0;
	v->frac        = 0;
	v->looped      = qfalse;
	v->dist        = 0.f;
	v->distScale   = 1.f;
	v->dir[0]      = 1.f;
	v->dir[1]      = 0.f;
	v->dir[2]      = 0.f;
	memset(v->spk, 0, sizeof(v->spk));
	memset(v->ambi, 0, sizeof(v->ambi));
	v->direct   = 0.f;
	v->rear     = 0.f;
	v->rearZ    = 0.f;
	v->lpZ[0]   = 0.f;
	v->lpZ[1]   = 0.f;
	v->obstruct = 0.f;
	v->occlude  = 0.f;
}

/* Game thread, for a block that is not published yet. */
static void Om_InitVoiceSlot(omVoice_t *v, int id)
{
	int i;
	v->hrtf       = NULL;
	v->tail       = 0;
	v->chunkHead  = 0;
	v->chunkCount = 0;
	for (i = 0; i < OM_STREAM_CHUNKS; i++) {
		v->chunks[i] = NULL;
	}
	Om_DefaultParams(&v->params);
	Om_ResetVoice(v);
	v->self     = id;
	v->nextFree = OM_ID_NONE;
}

static qboolean Om_HasGain(const omVoice_t *v)
{
	int k;
	for (k = 0; k < OM_OUT_MAX; k++) {
		if (v->spk[0][k] > OM_SILENCE || v->spk[1][k] > OM_SILENCE) {
			return qtrue;
		}
	}
	for (k = 0; k < OM_AMBI_CHANNELS; k++) {
		if (fabsf(v->ambi[k]) > OM_SILENCE) {
			return qtrue;
		}
	}
	return v->direct > OM_SILENCE ? qtrue : qfalse;
}

static omHandle_t *Om_VoiceHandle(const omVoice_t *v)
{
	if (v->handle < 0) {
		return NULL;
	}
	return Om_HandleAt(v->handle);
}

/* Mixer. The id goes back to the game thread through the return ring. */
static void Om_RetireVoice(omVoice_t *v)
{
	int id;
	if (!v || !v->used) {
		return;
	}
	id = v->self;
	Om_ResetVoice(v);
	v->self       = id;
	v->nextFree   = s_voiceRetHead;
	s_voiceRetHead = id;
}

static void Om_RecycleUnusedVoice(int id)
{
	omVoice_t *v;
	if (id < 0) {
		return;
	}
	v = Om_VoiceAt(id);
	if (!v || v->used) {
		return;
	}
	v->nextFree    = s_voiceRetHead;
	s_voiceRetHead = id;
}

static void Om_Publish(omVoice_t *v)
{
	omHandle_t *h = Om_VoiceHandle(v);
	int         passed, left, i;
	if (!h) {
		return;
	}
	h->offset.store(v->pos > 0 ? (unsigned)v->pos : 0u, std::memory_order_relaxed);
	if (!v->isStream) {
		return;
	}
	passed = 0;
	left   = v->pos;
	for (i = 0; i < v->chunkCount; i++) {
		const int f = Om_StreamChunk(v, i)->frames;
		if (left < f) {
			break;
		}
		passed++;
		left -= f;
	}
	h->processed.store(passed);
}

/* The handle reads STOPPED from now on; the voice keeps fading on its own. */
static void Om_Detach(omVoice_t *v)
{
	omHandle_t *h = Om_VoiceHandle(v);
	if (h) {
		Om_Publish(v);
		h->state.store(OM_VOICE_STOPPED);
		if (h->voice >= 0 && Om_VoiceAt(h->voice) == v) {
			h->voice = -1;
		}
	}
	v->handle = -1;
}

/* Paused or idle voices go at once; audible ones fade over the next block. */
static void Om_Release(omVoice_t *v)
{
	Om_Detach(v);
	if (v->state != OM_VOICE_PLAYING || !v->primed) {
		Om_RetireVoice(v);
		return;
	}
	v->ending = qtrue;
}

static void Om_Targets(
	omVoice_t *v, int channels, float base, float rearStep, float tSpk[2][OM_OUT_MAX], float tAmbi[OM_AMBI_CHANNELS],
	float *tDirect
)
{
	const omVoiceParams_t *p = &v->params;
	float                  img, horiz, az, rear = 0.f;
	int                    k;

	memset(tSpk, 0, sizeof(float) * 2 * OM_OUT_MAX);
	memset(tAmbi, 0, sizeof(float) * OM_AMBI_CHANNELS);
	*tDirect = 0.f;
	if (v->ending) {
		return;
	}
	if (channels >= 2) {
		if (s_hrtfOn) {
			/* ±30° pair on the 3rd-order bus. tSpk[c][0] is that channel's gain. */
			tSpk[0][0] = base;
			tSpk[1][0] = base;
			return;
		}
		if (s_layout == 3) {
			float g[OM_OUT_MAX];
			int   s;
			Om_RingGains((float)(M_PI / 6.0), 1.f, g);
			for (s = 0; s < s_outCh; s++) {
				tSpk[0][s] = base * g[s];
			}
			Om_RingGains((float)(-M_PI / 6.0), 1.f, g);
			for (s = 0; s < s_outCh; s++) {
				tSpk[1][s] = base * g[s];
			}
			return;
		}
		tSpk[0][s_frontL] = base;
		tSpk[1][s_frontR] = base;
		return;
	}
	if (p->flags & OM_FLAG_FLAT) {
		tSpk[0][s_frontL] = base * 0.7071f;
		tSpk[0][s_frontR] = base * 0.7071f;
		return;
	}
	if (s_hrtfOn) {
		/* Plain 2D and dry stay equal-power. A Miles pan is a real behind-the-head direction. */
		if (p->route == OM_ROUTE_DRY || ((p->flags & OM_FLAG_2D) && !(p->flags & OM_FLAG_PAN))) {
			Om_Image(v, &img, &horiz, &az);
			Om_StereoGains(img, &tSpk[0][0], &tSpk[0][1]);
			tSpk[0][0] *= base;
			tSpk[0][1] *= base;
		} else if (v->allowDirect) {
			*tDirect = base;
		} else {
			OmHrtf_AmbiCoeffs(v->dir, tAmbi);
			for (k = 0; k < OM_AMBI_CHANNELS; k++) {
				tAmbi[k] *= base;
			}
		}
		return;
	}
	if (s_layout == 0) {
		float gL, gR;
		/* PAN is behind the listener, so it takes the rear cue. Plain 2D is in front. */
		if (!(p->flags & OM_FLAG_2D) || (p->flags & OM_FLAG_PAN)) {
			rear = Om_Clamp(-v->dir[0], 0.f, 1.f);
		}
		v->rear += (rear - v->rear) * rearStep;
		if (fabsf(rear - v->rear) < 0.0005f) {
			v->rear = rear;
		}
		base *= Om_Lerp(1.f, OM_REAR_GAIN, v->rear);
		Om_FoaStereo(v->dir, &gL, &gR);
		tSpk[0][s_frontL] = base * gL;
		tSpk[0][s_frontR] = base * gR;
		return;
	}
	Om_Image(v, &img, &horiz, &az);
	Om_RingGains(az, horiz, tSpk[0]);
	for (k = 0; k < s_outCh; k++) {
		tSpk[0][k] *= base;
	}
}

static qboolean Om_TargetsSilent(const float tSpk[2][OM_OUT_MAX], const float tAmbi[OM_AMBI_CHANNELS], float tDirect)
{
	int k;
	for (k = 0; k < OM_OUT_MAX; k++) {
		if (tSpk[0][k] > OM_SILENCE || tSpk[1][k] > OM_SILENCE) {
			return qfalse;
		}
	}
	for (k = 0; k < OM_AMBI_CHANNELS; k++) {
		if (fabsf(tAmbi[k]) > OM_SILENCE) {
			return qfalse;
		}
	}
	return tDirect > OM_SILENCE ? qfalse : qtrue;
}

/* Run the direct HRTF filter: ramped input while audible, then zeros until it rings out. */
static void Om_MixDirect(omVoice_t *v, float tDirect, int count)
{
	int i;
	/* The filter comes from the preallocated pool before the targets are set; no allocation here. */
	if (tDirect > OM_SILENCE || v->direct > OM_SILENCE) {
		if (v->hrtf) {
			const float step = (tDirect - v->direct) / (float)count;
			float       g    = v->direct;
			for (i = 0; i < count; i++) {
				g += step;
				s_tmp[i] = s_dry[0][i] * g;
			}
			OmHrtf_VoiceProcess(v->hrtf, v->dir, s_tmp, s_bus[0], s_bus[1], count);
			v->tail = OmHrtf_TailBlocks() * OM_HRTF_BLOCK;
		}
	} else if (v->hrtf) {
		if (v->tail > 0) {
			memset(s_tmp, 0, sizeof(float) * (size_t)count);
			OmHrtf_VoiceProcess(v->hrtf, v->dir, s_tmp, s_bus[0], s_bus[1], count);
			v->tail -= count;
		} else {
			Om_FreeHrtf(v);
		}
	}
	v->direct = tDirect;
}

/* Added in Omaha: one block moves a gain at most count / OM_RAMP of the way, so short periods do not click. */
static float Om_RampEnd(float cur, float target, float r)
{
	const float e = cur + (target - cur) * r;
	return fabsf(e - target) < OM_SILENCE ? target : e;
}

static void Om_RampTargets(
	const omVoice_t *v, int count, float tSpk[2][OM_OUT_MAX], float tAmbi[OM_AMBI_CHANNELS], float *tDirect
)
{
	const float r = count >= OM_RAMP ? 1.f : (float)count / (float)OM_RAMP;
	int         c, k;
	if (r >= 1.f) {
		return;
	}
	for (c = 0; c < 2; c++) {
		for (k = 0; k < OM_OUT_MAX; k++) {
			tSpk[c][k] = Om_RampEnd(v->spk[c][k], tSpk[c][k], r);
		}
	}
	for (k = 0; k < OM_AMBI_CHANNELS; k++) {
		tAmbi[k] = Om_RampEnd(v->ambi[k], tAmbi[k], r);
	}
	*tDirect = Om_RampEnd(v->direct, *tDirect, r);
}

static void Om_MixVoice(omVoice_t *v, int count)
{
	omVoiceParams_t    *p   = &v->params;
	const omSound_t    *snd = NULL;
	const omSincBank_t *bank;
	float               tSpk[2][OM_OUT_MAX], tAmbi[OM_AMBI_CHANNELS], tDirect;
	float               base;
	int                 channels, frames, rate, a = 0, b = 0, c, o, k;
	unsigned            step;
	qboolean            loop, finished = qfalse;

	if (v->state != OM_VOICE_PLAYING) {
		return;
	}

	/* Faded out: only the HRTF filter may still ring. */
	if (v->ending && !Om_HasGain(v)) {
		Om_MixDirect(v, 0.f, count);
		if (!v->hrtf) {
			Om_RetireVoice(v);
		}
		return;
	}

	if (v->isStream) {
		frames = Om_StreamFrames(v);
		if (v->pos >= frames) {
			/* Ran dry: wait for more data or a stop. */
			v->pos  = frames;
			v->frac = 0;
			v->state = OM_VOICE_STOPPED;
			Om_Publish(v);
			if (Om_VoiceHandle(v)) {
				Om_VoiceHandle(v)->state.store(OM_VOICE_STOPPED);
			}
			if (v->ending) {
				Om_RetireVoice(v);
			}
			return;
		}
		channels = Om_StreamChunk(v, 0)->channels;
		rate     = Om_StreamChunk(v, 0)->rate;
		loop     = qfalse;
	} else {
		snd = Om_VoiceSound(v);
		if (!snd) {
			Om_Detach(v);
			Om_RetireVoice(v);
			return;
		}
		channels = snd->channels;
		frames   = snd->frames;
		rate     = snd->rate;
		loop     = (p->flags & OM_FLAG_LOOP) ? qtrue : qfalse;
		Om_LoopRange(v, frames, &a, &b);
	}
	if (channels > 2) {
		channels = 2;
	}
	{
		float pitch = p->pitch;
		if (channels == 1 && !(p->flags & (OM_FLAG_2D | OM_FLAG_PAN | OM_FLAG_FLAT))) {
			pitch *= Om_Doppler(v);
		}
		step = Om_StepFor(rate, pitch);
	}
	bank = Om_SincFor(step);
	base = Om_Clamp(p->gain, 0.f, OM_GAIN_MAX) * v->distScale;

	/* Added in Omaha: a direct voice with no free pool filter stays on the shared bus. */
	if (v->allowDirect && !v->hrtf) {
		v->hrtf = OmHrtf_VoiceAlloc();
		if (!v->hrtf) {
			v->allowDirect = qfalse;
		}
	}
	Om_Targets(v, channels, base, s_rearStep, tSpk, tAmbi, &tDirect);
	if (!v->primed) {
		memcpy(v->spk, tSpk, sizeof(v->spk));
		memcpy(v->ambi, tAmbi, sizeof(v->ambi));
		v->direct = tDirect;
		v->primed = qtrue;
	}
	Om_RampTargets(v, count, tSpk, tAmbi, &tDirect);

	/* Inaudible (beyond max distance, zero gain): move the cursor only. */
	if (!v->hrtf && !Om_HasGain(v) && Om_TargetsSilent(tSpk, tAmbi, tDirect)) {
		const uint64_t f = (uint64_t)v->frac + (uint64_t)step * (uint64_t)count;
		v->pos += (int)(f >> 16);
		v->frac = (unsigned)(f & (OM_FRAC_ONE - 1));
		if (loop) {
			if (v->pos >= b) {
				v->pos    = Om_Wrap(v->pos, a, b);
				v->looped = qtrue;
			}
		} else if (v->isStream && v->pos > frames) {
			v->pos  = frames;
			v->frac = 0;
		}
		finished = !loop && !v->isStream && v->pos >= frames;
		Om_Publish(v);
		if (finished || v->ending) {
			Om_Detach(v);
			Om_RetireVoice(v);
		}
		return;
	}

	for (c = 0; c < channels; c++) {
		int      pos    = v->pos;
		unsigned frac   = v->frac;
		qboolean looped = v->looped;
		if (v->isStream) {
			const int need = (int)(((uint64_t)frac + (uint64_t)step * (uint64_t)count) >> 16) + bank->taps;
			Om_StreamGather(v, c, pos - (bank->half - 1), need, s_gather);
			pos += Om_ResampleRun(bank, s_gather, &frac, step, s_dry[c], count);
		} else {
			const float *ptr = snd->planar[c].data() + OM_GUARD;
			Om_ResamplePtr(bank, ptr, frames, &pos, &frac, step, loop, a, b, &looped, s_dry[c], count);
		}
		if (c + 1 == channels) {
			v->pos    = pos;
			v->frac   = frac;
			v->looped = looped;
		}
	}
	if (v->isStream) {
		if (v->pos > frames) {
			v->pos  = frames;
			v->frac = 0;
		}
	} else if (!loop && v->pos >= frames) {
		finished = qtrue;
	}

	if (s_cv.obstruction.load(std::memory_order_relaxed) && !(p->flags & (OM_FLAG_2D | OM_FLAG_FLAT | OM_FLAG_PAN))) {
		const float smooth = 1.f - expf(-(float)count / (OM_OBSTRUCT_SMOOTH_MS * 0.001f * (float)s_sampleRate));
		float       distT, ray, room, fcRay, fc, dry;
		v->obstruct += (p->obstruction - v->obstruct) * smooth;
		v->occlude += (p->occlusion - v->occlude) * smooth;
		distT = Om_Clamp(v->dist / OM_OBSTRUCT_DIST_RANGE, 0.f, 1.f);
		ray   = Om_Clamp(v->obstruct, 0.f, 1.f);
		room  = Om_Clamp(v->occlude, 0.f, 1.f);
		fcRay = Om_Lerp(
            Om_Lerp(OM_OBSTRUCT_OPEN_HZ, OM_OBSTRUCT_CUTOFF_HZ, distT),
            Om_Lerp(OM_OBSTRUCT_OPEN_HZ, OM_OBSTRUCT_OCC_BLOCKED_HZ, ray),
            OM_OBSTRUCT_MIX
        );
		/* Closed portal replaces the brush lowpass. It does not stack on it. */
		fc  = Om_Lerp(fcRay, OM_OCCLUDE_CUTOFF_HZ, room);
		dry = Om_Lerp(1.f, OM_OCCLUDE_GAIN, room);
		if ((distT >= OM_OBSTRUCT_EPSILON || ray >= OM_OBSTRUCT_EPSILON || room >= OM_OBSTRUCT_EPSILON)
		    && fc < (float)s_sampleRate * 0.45f) {
			const float lpA = fmaxf(1.f - expf(-2.f * (float)M_PI * fmaxf(fc, 20.f) / (float)s_sampleRate), 1.0e-4f);
			const float makeup = Om_Clamp(sqrtf(sqrtf((2.f - lpA) / lpA)), 1.f, OM_OBSTRUCT_MAKEUP_MAX);
			for (c = 0; c < channels; c++) {
				Om_Filter(s_dry[c], count, lpA, &v->lpZ[c]);
				for (k = 0; k < count; k++) {
					s_dry[c][k] *= makeup * dry;
				}
			}
		} else if (dry < 1.f) {
			for (c = 0; c < channels; c++) {
				for (k = 0; k < count; k++) {
					s_dry[c][k] *= dry;
				}
			}
		}
	} else {
		v->obstruct = 0.f;
		v->occlude  = 0.f;
	}
	if (!s_hrtfOn && s_layout == 0 && channels == 1) {
		Om_Rear(s_dry[0], count, v->rear, &v->rearZ);
	}

	if (s_hrtfOn && channels >= 2) {
		qboolean any = qfalse;
		for (c = 0; c < 2; c++) {
			const float from = v->spk[c][0];
			const float to   = tSpk[c][0];
			const float step = (to - from) / (float)count;
			float       g    = from;
			int         i;
			for (i = 0; i < count; i++) {
				g += step;
				s_tmp[i] = s_dry[c][i] * g;
			}
			for (k = 0; k < OM_AMBI_CHANNELS; k++) {
				const float coeff = s_pairAmbi[c][k];
				if (coeff == 0.f) {
					continue;
				}
				for (i = 0; i < count; i++) {
					s_ambi[k][i] += s_tmp[i] * coeff;
				}
				any = qtrue;
			}
		}
		if (any) {
			s_ambiFlush = (OmHrtf_TailBlocks() + 2) * OM_HRTF_BLOCK;
		}
	} else {
		for (c = 0; c < channels; c++) {
			for (o = 0; o < s_outCh; o++) {
				Om_AddGain(s_dry[c], s_bus[o], v->spk[c][o], tSpk[c][o], count);
			}
		}
	}
	memcpy(v->spk, tSpk, sizeof(v->spk));
	if (s_hrtfOn && channels == 1) {
		qboolean any = qfalse;
		for (k = 0; k < OM_AMBI_CHANNELS; k++) {
			if (fabsf(v->ambi[k]) > OM_SILENCE || fabsf(tAmbi[k]) > OM_SILENCE) {
				Om_AddGain(s_dry[0], s_ambi[k], v->ambi[k], tAmbi[k], count);
				any = qtrue;
			}
		}
		if (any) {
			s_ambiFlush = (OmHrtf_TailBlocks() + 2) * OM_HRTF_BLOCK;
		}
		memcpy(v->ambi, tAmbi, sizeof(v->ambi));
		Om_MixDirect(v, tDirect, count);
	}

	Om_Publish(v);
	if (finished) {
		/* Past the end the input is silent, so there is nothing left to fade. */
		Om_Detach(v);
		memset(v->spk, 0, sizeof(v->spk));
		memset(v->ambi, 0, sizeof(v->ambi));
		v->direct = 0.f;
		v->ending = qtrue;
		if (!v->hrtf) {
			Om_RetireVoice(v);
		}
	}
}

static void Om_MixBlock(int count)
{
	int i, o, n = 0;

	/* Changed in Omaha: one 50 ms step for this line, shared by every voice. */
	if (s_sampleRate > 0 && count > 0) {
		s_rearStep = 1.f - expf(-(float)count / (OM_REAR_SMOOTH_MS * 0.001f * (float)s_sampleRate));
	} else {
		s_rearStep = 1.f;
	}

	for (o = 0; o < s_outCh; o++) {
		memset(s_bus[o], 0, sizeof(float) * (size_t)count);
	}
	if (s_hrtfOn) {
		for (o = 0; o < OM_AMBI_CHANNELS; o++) {
			memset(s_ambi[o], 0, sizeof(float) * (size_t)count);
		}
	}
	{
		/* Capacity first: rank arrays are published before the capacity they cover. */
		const int cap   = s_voiceCap.load(std::memory_order_acquire);
		int      *rank  = s_rank.load(std::memory_order_acquire);
		float    *rankG = s_rankG.load(std::memory_order_acquire);
		const int hold  = (int)(OM_DIRECT_HOLD_MS * 0.001f * (float)s_sampleRate);
		if (rank && rankG && cap > 0) {
			int extra = 0;
			Om_ForEachVoice(cap, [&](omVoice_t *v) {
				if (!v->used) {
					return;
				}
				Om_DistanceGain(v);
				v->allowDirect = qfalse;
				if (n >= cap) {
					return;
				}
				if (s_hrtfOn && v->state == OM_VOICE_PLAYING && !v->ending && v->params.route == OM_ROUTE_DIRECT
					&& !(v->params.flags & (OM_FLAG_2D | OM_FLAG_FLAT | OM_FLAG_PAN)) && v->dist > OM_HRTF_MIN_DIST
					&& Om_VoiceChannels(v) == 1) {
					rank[n]  = v->self;
					rankG[n] = v->params.gain * v->distScale;
					n++;
				}
			});
			/* The loudest few direct voices get their own HRTF filter; the rest share the bus. */
			for (i = 0; i < n && i < OM_DIRECT_CAP; i++) {
				int best = i, k;
				for (k = i + 1; k < n; k++) {
					if (rankG[k] > rankG[best]) {
						best = k;
					}
				}
				if (best != i) {
					const int   ti = rank[i];
					const float tg = rankG[i];
					rank[i]      = rank[best];
					rankG[i]     = rankG[best];
					rank[best]   = ti;
					rankG[best]  = tg;
				}
				{
					omVoice_t *chosen = Om_VoiceAt(rank[i]);
					if (chosen) {
						chosen->allowDirect = qtrue;
						chosen->directHold  = hold;
					}
				}
			}
			/*
			 * Added in Omaha: a voice that drops out of the top few keeps its filter for
			 * OM_DIRECT_HOLD_MS, so near-equal voices do not trade places every block.
			 */
			for (i = OM_DIRECT_CAP; i < n && extra < OM_DIRECT_CAP; i++) {
				omVoice_t *held = Om_VoiceAt(rank[i]);
				if (!held || !held->prevDirect || held->directHold <= 0) {
					continue;
				}
				held->directHold -= count;
				if (held->directHold > 0) {
					held->allowDirect = qtrue;
					extra++;
				}
			}
			Om_ForEachVoice(cap, [&](omVoice_t *v) {
				if (v->used) {
					Om_MixVoice(v, count);
					v->prevDirect = v->allowDirect;
				}
			});
		}
	}
	if (s_hrtfOn && s_ambiFlush > 0) {
		float *ambi[OM_AMBI_CHANNELS];
		for (o = 0; o < OM_AMBI_CHANNELS; o++) {
			ambi[o] = s_ambi[o];
		}
		OmHrtf_AmbiDecode(ambi, s_bus[0], s_bus[1], count);
		s_ambiFlush -= count;
	}
}

static void Om_Interleave(float *dst, int frames, int ch)
{
	int i, c;
	for (i = 0; i < frames; i++) {
		for (c = 0; c < ch; c++) {
			dst[i * ch + c] = s_bus[c][i];
		}
	}
}

/* Bit test: stays correct under -ffast-math, unlike isfinite. */
static qboolean Om_Finite(float f)
{
	uint32_t u;
	memcpy(&u, &f, sizeof(u));
	return (u & 0x7f800000u) != 0x7f800000u ? qtrue : qfalse;
}

static qboolean Om_BlockFinite(const float *x, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		if (!Om_Finite(x[i])) {
			return qfalse;
		}
	}
	return qtrue;
}

static void Om_EqClear(omEq_t *bands, int nBands)
{
	int b;
	for (b = 0; b < nBands; b++) {
		memset(bands[b].z1, 0, sizeof(bands[b].z1));
		memset(bands[b].z2, 0, sizeof(bands[b].z2));
	}
}

/* Added in Omaha: a non-finite sample would poison every recursive state below for good. */
static void Om_ResetDsp(void)
{
	Om_EqClear(s_spkEq, OM_SPK_EQ_BANDS);
	Om_EqClear(s_hpEq, OM_HP_EQ_BANDS);
	Om_HfReset();
	Om_LimiterReset();
	if (s_hrtfOn) {
		OmHrtf_AmbiReset();
	}
}

static void Om_Master(float *interleaved, int frames, int ch)
{
	if (!Om_BlockFinite(interleaved, frames * ch)) {
		memset(interleaved, 0, sizeof(float) * (size_t)frames * (size_t)ch);
		Om_ResetDsp();
		return;
	}
	if (!s_hrtfOn && s_layout == 0 && s_spkEqReady) {
		Om_RunEq(s_spkEq, OM_SPK_EQ_BANDS, interleaved, frames, ch);
	}
	if (s_hrtfOn && s_hpEqReady && ch == 2) {
		Om_RunEq(s_hpEq, OM_HP_EQ_BANDS, interleaved, frames, ch);
	}
	Om_ApplyHfDuck(interleaved, frames, ch);
	Om_ApplyLimiter(interleaved, frames, ch);
	Om_ApplyMaster(interleaved, frames, ch);
}

static void Om_RenderLine(float *dst, int frames, int ch)
{
	Om_MixBlock(frames);
	Om_Interleave(dst, frames, ch);
	Om_Master(dst, frames, ch);
}

/* HRTF filters take whole blocks; a short request is served from one stashed block. */
static void Om_Mix(float *dst, int frames, int ch)
{
	OmFtzGuard ftz;
	const int  oc = ch;
	while (frames > 0) {
		int n;
		if (!s_hrtfOn) {
			n = frames < OM_LINE ? frames : OM_LINE;
			Om_RenderLine(dst, n, ch);
		} else if (s_stashLen > 0) {
			n = frames < s_stashLen ? frames : s_stashLen;
			memcpy(dst, s_stash + (size_t)s_stashPos * oc, sizeof(float) * (size_t)n * oc);
			s_stashPos += n;
			s_stashLen -= n;
		} else if (frames >= OM_HRTF_BLOCK) {
			n = frames - frames % OM_HRTF_BLOCK;
			if (n > OM_LINE) {
				n = OM_LINE;
			}
			Om_RenderLine(dst, n, ch);
		} else {
			Om_RenderLine(s_stash, OM_HRTF_BLOCK, ch);
			s_stashPos = 0;
			s_stashLen = OM_HRTF_BLOCK;
			continue;
		}
		dst += (size_t)n * oc;
		frames -= n;
	}
}

/* Mixer: the handle reads STOPPED and goes back to the game thread's free list. */
static void Om_FreeHandle(int id)
{
	omHandle_t *h = Om_HandleAt(id);
	if (!h || h->dead) {
		return;
	}
	h->voice = -1;
	h->dead  = qtrue;
	h->state.store(OM_VOICE_STOPPED);
	h->nextFree     = s_handleRetHead;
	s_handleRetHead = id;
}

static void Om_RecycleSound(int id)
{
	omSoundSlot_t *s = Om_SoundSlot(id);
	if (!s) {
		return;
	}
	s->nextFree     = s_soundIdRetHead;
	s_soundIdRetHead = id;
}

static qboolean Om_RetPush(int type, int id, void *ptr)
{
	const unsigned tail = s_retTail.load(std::memory_order_relaxed);
	omRet_t       *r;
	if (tail - s_retHead.load(std::memory_order_acquire) >= OM_RET_RING) {
		return qfalse;
	}
	r       = &s_retRing[tail & (OM_RET_RING - 1)];
	r->type = type;
	r->id   = id;
	r->ptr  = ptr;
	s_retTail.store(tail + 1, std::memory_order_release);
	return qtrue;
}

/* Mixer, after the mix. What does not fit stays on the lists for the next callback. */
static void Om_PublishReturns(void)
{
	while (s_handleRetHead != OM_ID_NONE) {
		const int next = Om_HandleAt(s_handleRetHead)->nextFree;
		if (!Om_RetPush(OM_RET_HANDLE, s_handleRetHead, NULL)) {
			return;
		}
		s_handleRetHead = next;
	}
	while (s_voiceRetHead != OM_ID_NONE) {
		const int next = Om_VoiceAt(s_voiceRetHead)->nextFree;
		if (!Om_RetPush(OM_RET_VOICE, s_voiceRetHead, NULL)) {
			return;
		}
		s_voiceRetHead = next;
	}
	while (s_soundIdRetHead != OM_ID_NONE) {
		const int next = Om_SoundSlot(s_soundIdRetHead)->nextFree;
		if (!Om_RetPush(OM_RET_SOUND_ID, s_soundIdRetHead, NULL)) {
			return;
		}
		s_soundIdRetHead = next;
	}
	while (s_soundRetHead) {
		omSound_t *next = s_soundRetHead->next;
		if (!Om_RetPush(OM_RET_SOUND, OM_ID_NONE, s_soundRetHead)) {
			return;
		}
		s_soundRetHead = next;
	}
	while (s_chunkRetHead) {
		omChunk_t *next = s_chunkRetHead->next;
		if (!Om_RetPush(OM_RET_CHUNK, OM_ID_NONE, s_chunkRetHead)) {
			return;
		}
		s_chunkRetHead = next;
	}
}

/* Game thread: take back ids and delete garbage the mixer has let go of. */
static void Om_Reclaim(void)
{
	unsigned       head = s_retHead.load(std::memory_order_relaxed);
	const unsigned tail = s_retTail.load(std::memory_order_acquire);
	while (head != tail) {
		const omRet_t *r = &s_retRing[head & (OM_RET_RING - 1)];
		switch (r->type) {
		case OM_RET_HANDLE:
			Om_ReturnHandle(r->id);
			break;
		case OM_RET_VOICE:
			{
				omVoice_t *v = Om_VoiceAt(r->id);
				if (v) {
					v->nextFree     = s_voiceFreeHead;
					s_voiceFreeHead = r->id;
				}
			}
			break;
		case OM_RET_SOUND_ID:
			{
				omSoundSlot_t *slot = Om_SoundSlot(r->id);
				if (slot) {
					slot->nextFree  = s_soundFreeHead;
					s_soundFreeHead = r->id;
				}
			}
			break;
		case OM_RET_SOUND:
			delete (omSound_t *)r->ptr;
			break;
		case OM_RET_CHUNK:
			delete (omChunk_t *)r->ptr;
			break;
		default:
			break;
		}
		head++;
	}
	s_retHead.store(head, std::memory_order_release);
	Om_FreeRetiredRanks(qfalse);
}

/* Added in Omaha: drop non-finite values before they reach the filters. */
static void Om_SanitizeParams(omVoiceParams_t *p)
{
	int i;
	for (i = 0; i < 3; i++) {
		if (!Om_Finite(p->pos[i])) {
			p->pos[i] = 0.f;
		}
		if (!Om_Finite(p->vel[i])) {
			p->vel[i] = 0.f;
		}
	}
	if (!Om_Finite(p->gain)) {
		p->gain = 0.f;
	}
	if (!Om_Finite(p->pitch)) {
		p->pitch = 1.f;
	}
	if (!Om_Finite(p->minDist)) {
		p->minDist = 200.f;
	}
	if (!Om_Finite(p->maxDist)) {
		p->maxDist = 12800.f;
	}
	if (!Om_Finite(p->obstruction)) {
		p->obstruction = 0.f;
	}
	if (!Om_Finite(p->occlusion)) {
		p->occlusion = 0.f;
	}
	if (!Om_Finite(p->pan)) {
		p->pan = 0.f;
	}
}

/* Game thread: one writer, so the sequence only needs to be odd during the copy. */
static void Om_SeqWrite(std::atomic<unsigned> &seq, std::atomic<uint32_t> *mail, const void *src, int words)
{
	uint32_t       w[64];
	const unsigned at = seq.load(std::memory_order_relaxed);
	int            i;
	memcpy(w, src, sizeof(uint32_t) * (size_t)words);
	seq.store(at + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	for (i = 0; i < words; i++) {
		mail[i].store(w[i], std::memory_order_relaxed);
	}
	seq.store(at + 2, std::memory_order_release);
}

/* Mixer: qfalse when the game thread was mid-write; try again next callback. */
static qboolean Om_SeqRead(std::atomic<unsigned> &seq, const std::atomic<uint32_t> *mail, void *dst, int words)
{
	uint32_t w[64];
	int      tries, i;
	for (tries = 0; tries < 4; tries++) {
		const unsigned s1 = seq.load(std::memory_order_acquire);
		if (s1 & 1u) {
			continue;
		}
		for (i = 0; i < words; i++) {
			w[i] = mail[i].load(std::memory_order_relaxed);
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		if (seq.load(std::memory_order_relaxed) == s1) {
			memcpy(dst, w, sizeof(uint32_t) * (size_t)words);
			return qtrue;
		}
	}
	return qfalse;
}

/* Mixer: latest listener wins. */
static void Om_ReadListener(void)
{
	float l[OM_LISTENER_WORDS];
	int   i;
	if (!s_lisDirty.exchange(0, std::memory_order_acq_rel)) {
		return;
	}
	if (!Om_SeqRead(s_lisSeq, s_lisMail, l, OM_LISTENER_WORDS)) {
		s_lisDirty.store(1, std::memory_order_relaxed);
		return;
	}
	for (i = 0; i < OM_LISTENER_WORDS; i++) {
		if (!Om_Finite(l[i])) {
			return;
		}
	}
	memcpy(s_lisPos, l + 0, sizeof(s_lisPos));
	memcpy(s_lisVel, l + 3, sizeof(s_lisVel));
	memcpy(s_lisFwd, l + 6, sizeof(s_lisFwd));
	memcpy(s_lisLeft, l + 9, sizeof(s_lisLeft));
	memcpy(s_lisUp, l + 12, sizeof(s_lisUp));
}

/* Mixer, after the command drain: apply the newest params of every bound voice. */
static void Om_PollMail(void)
{
	const int cap = s_voiceCap.load(std::memory_order_acquire);
	Om_ForEachVoice(cap, [&](omVoice_t *v) {
		omHandle_t     *h;
		omVoiceParams_t p;
		if (!v->used || v->handle < 0) {
			return;
		}
		h = Om_HandleAt(v->handle);
		if (!h || !h->dirty.exchange(0, std::memory_order_acq_rel)) {
			return;
		}
		if (!Om_SeqRead(h->mailSeq, h->mail, &p, OM_PARAM_WORDS)) {
			h->dirty.store(1, std::memory_order_relaxed);
			return;
		}
		Om_SanitizeParams(&p);
		v->params = p;
	});
}

static void Om_Apply(const omCmd_t *c)
{
	omHandle_t *h;
	omVoice_t  *v = NULL;
	int         vi;

	switch (c->op) {
	case OM_OP_RETIRE_SOUND:
		Om_RetSound(c->retire);
		return;
	case OM_OP_FREE_SOUND:
		{
			omSoundSlot_t *slot = Om_SoundSlot(c->sound);
			if (slot) {
				omSound_t *old = slot->sound.exchange(NULL);
				const int cap = s_voiceCap.load(std::memory_order_acquire);
				Om_ForEachVoice(cap, [&](omVoice_t *voice) {
					if (voice->used && !voice->isStream && voice->sound == c->sound) {
						Om_Detach(voice);
						Om_RetireVoice(voice);
					}
				});
				Om_RetSound(old);
				Om_RecycleSound(c->sound);
			}
		}
		return;
	default:
		break;
	}

	h = Om_HandleAt(c->id);
	if (!h) {
		if (c->op == OM_OP_PLAY || c->op == OM_OP_OPEN) {
			Om_RecycleUnusedVoice(c->slot);
		}
		Om_RetChunk(c->chunk);
		return;
	}
	if (h->dead) {
		/* Freed earlier in this or a past drain: only a fresh owner may PLAY / OPEN it. */
		if (c->op != OM_OP_PLAY && c->op != OM_OP_OPEN) {
			Om_RetChunk(c->chunk);
			return;
		}
		h->dead = qfalse;
	}
	if (h->voice >= 0) {
		omVoice_t *live = Om_VoiceAt(h->voice);
		if (live && live->used && live->handle == c->id) {
			v = live;
		}
	}

	switch (c->op) {
	case OM_OP_PLAY:
	case OM_OP_OPEN:
		if (v && v->self == c->slot) {
			v->params   = c->params;
			Om_SanitizeParams(&v->params);
			v->isStream = c->op == OM_OP_OPEN ? qtrue : qfalse;
			v->sound    = v->isStream ? -1 : c->sound;
			v->pos      = (int)c->offset;
			v->frac     = 0;
			v->looped   = qfalse;
			v->state    = v->isStream ? OM_VOICE_INITIAL : OM_VOICE_PLAYING;
			h->voice    = v->self;
			h->state.store(v->state);
			break;
		}
		if (v) {
			Om_Release(v);
		}
		vi = c->slot;
		v = Om_VoiceAt(vi);
		if (!v || v->used) {
			h->voice = -1;
			h->state.store(OM_VOICE_STOPPED);
			break;
		}
		Om_ResetVoice(v);
		v->self     = vi;
		v->used     = qtrue;
		v->handle   = c->id;
		v->params   = c->params;
		Om_SanitizeParams(&v->params);
		v->isStream = c->op == OM_OP_OPEN ? qtrue : qfalse;
		v->sound    = v->isStream ? -1 : c->sound;
		v->pos      = (int)c->offset;
		v->state    = v->isStream ? OM_VOICE_INITIAL : OM_VOICE_PLAYING;
		h->voice    = vi;
		h->state.store(v->state);
		break;
	case OM_OP_STOP:
		if (v && v->isStream) {
			/* Queued chunks all count as processed until unqueued, like an AL source. */
			v->pos   = Om_StreamFrames(v);
			v->frac  = 0;
			v->state = OM_VOICE_STOPPED;
			Om_Publish(v);
			h->state.store(OM_VOICE_STOPPED);
			break;
		}
		if (v) {
			Om_Release(v);
		}
		if (!h->stream) {
			Om_FreeHandle(c->id);
		}
		break;
	case OM_OP_PAUSE:
		if (v && v->state != OM_VOICE_STOPPED) {
			v->state = OM_VOICE_PAUSED;
		}
		h->state.store(v ? v->state : OM_VOICE_STOPPED);
		break;
	case OM_OP_RESUME:
		if (v) {
			v->state = OM_VOICE_PLAYING;
		}
		h->state.store(v ? OM_VOICE_PLAYING : OM_VOICE_STOPPED);
		break;
	case OM_OP_DESTROY:
		if (v) {
			Om_Release(v);
		}
		Om_FreeHandle(c->id);
		break;
	case OM_OP_SEEK:
		if (v) {
			v->pos    = (int)c->offset;
			v->frac   = 0;
			v->looped = qfalse;
			Om_Publish(v);
		}
		break;
	case OM_OP_QUEUE:
		if (v && v->isStream && c->chunk && c->chunk->frames > 0 && v->chunkCount < OM_STREAM_CHUNKS) {
			v->chunks[(v->chunkHead + v->chunkCount) % OM_STREAM_CHUNKS] = c->chunk;
			v->chunkCount++;
		} else {
			Om_RetChunk(c->chunk);
		}
		break;
	case OM_OP_UNQUEUE:
		if (v && v->chunkCount > 0) {
			omChunk_t *front   = v->chunks[v->chunkHead];
			const int  removed = front->frames;
			v->chunks[v->chunkHead] = NULL;
			v->chunkHead            = (v->chunkHead + 1) % OM_STREAM_CHUNKS;
			v->chunkCount--;
			Om_RetChunk(front);
			v->pos = v->pos >= removed ? v->pos - removed : 0;
			Om_Publish(v);
		}
		h->queued.fetch_sub(1);
		h->pendingUnqueue.fetch_sub(1);
		break;
	default:
		Om_RetChunk(c->chunk);
		break;
	}
}

/* Mixer: apply everything the game thread published so far. */
static void Om_Drain(void)
{
	unsigned       head = s_cmdHead.load(std::memory_order_relaxed);
	const unsigned tail = s_cmdTail.load(std::memory_order_acquire);
	while (head != tail) {
		Om_Apply(&s_cmdRing[head & (OM_CMD_RING - 1)]);
		head++;
	}
	s_cmdHead.store(head, std::memory_order_release);
}

/* Game thread. */
static qboolean Om_RingPush(const omCmd_t &c)
{
	const unsigned tail = s_cmdTail.load(std::memory_order_relaxed);
	if (tail - s_cmdHead.load(std::memory_order_acquire) >= OM_CMD_RING) {
		return qfalse;
	}
	s_cmdRing[tail & (OM_CMD_RING - 1)] = c;
	s_cmdTail.store(tail + 1, std::memory_order_release);
	return qtrue;
}

static void Om_FlushSpill(void)
{
	while (!s_cmdSpill.empty()) {
		if (!Om_RingPush(s_cmdSpill.front())) {
			return;
		}
		s_cmdSpill.pop_front();
	}
}

/* Game thread. Lifecycle commands are never dropped: a full ring spills, in order. */
static void Om_Push(const omCmd_t &c)
{
	Om_FlushSpill();
	if (!s_cmdSpill.empty() || !Om_RingPush(c)) {
		s_cmdSpill.push_back(c);
	}
}

/* Holder of s_mixGate only. frames == 0 applies state without mixing (device down). */
static void Om_MixGated(float *dst, int frames, int ch)
{
	s_mixEpoch.fetch_add(1, std::memory_order_seq_cst);
	Om_ReadListener();
	Om_Drain();
	Om_PollMail();
	if (frames > 0) {
		Om_Mix(dst, frames, ch);
	}
	Om_PublishReturns();
	s_mixEpoch.fetch_add(1, std::memory_order_seq_cst);
}

/* Changed in Omaha: takes no lock. A busy gate means the game thread applies the commands of a stalled device; play silence. */
static void Om_DataCallback(ma_device *dev, void *output, const void *input, ma_uint32 frameCount)
{
	(void)dev;
	(void)input;
	s_callbacks.fetch_add(1, std::memory_order_relaxed);
	if (s_mixGate.test_and_set(std::memory_order_acquire)) {
		memset(output, 0, sizeof(float) * s_outCh * (size_t)frameCount);
		return;
	}
	Om_MixGated((float *)output, (int)frameCount, s_outCh);
	s_mixGate.clear(std::memory_order_release);
}

static void Om_LogDevices(void)
{
	ma_device_info *playback = NULL;
	ma_uint32       count    = 0, i;
	if (!s_contextReady || ma_context_get_devices(&s_context, &playback, &count, NULL, NULL) != MA_SUCCESS) {
		return;
	}
	Com_Printf("Sound/omaha: %u playback device(s):\n", (unsigned)count);
	for (i = 0; i < count; i++) {
		Com_Printf("  %s%s\n", playback[i].name, playback[i].isDefault ? " (default)" : "");
	}
}

static qboolean Om_FindDevice(const char *name, ma_device_id *id)
{
	ma_device_info *playback = NULL;
	ma_uint32       count    = 0, i;
	if (!name || !name[0] || !s_contextReady) {
		return qfalse;
	}
	if (ma_context_get_devices(&s_context, &playback, &count, NULL, NULL) != MA_SUCCESS) {
		return qfalse;
	}
	for (i = 0; i < count; i++) {
		if (!Q_stricmp(playback[i].name, name)) {
			*id = playback[i].id;
			return qtrue;
		}
	}
	Com_Printf("Sound/omaha: device '%s' not found, using default.\n", name);
	return qfalse;
}

extern "C" char *Sys_BinaryPath(void);

/*
 * Changed: a file of the client's own main/ folder (where the build and the
 * installs put the HRTF set), read when fs_basepath points at the game data
 * elsewhere. Freed with free().
 */
static void *Om_ReadAppFile(const char *name, long *size)
{
	char  path[MAX_OSPATH];
	FILE *f;
	void *data = NULL;
	long  n;

	*size = 0;
	Com_sprintf(path, sizeof(path), "%s%cmain%c%s", Sys_BinaryPath(), PATH_SEP, PATH_SEP, name);
	f = Sys_FOpen(path, "rb");
	if (!f) {
		return NULL;
	}
	if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET)) {
		data = malloc((size_t)n);
		if (data && fread(data, 1, (size_t)n, f) == (size_t)n) {
			*size = n;
		} else {
			free(data);
			data = NULL;
		}
	}
	fclose(f);
	return data;
}

static qboolean Om_LoadHrtf(void)
{
	cvar_t  *file = Cvar_Get("s_hrtf_file", SND_HRTF_DEFAULT_FILE, CVAR_ARCHIVE | CVAR_SOUND_LATCH);
	void    *data = NULL;
	long     size;
	qboolean ok, appFile = qfalse;
	size = FS_ReadFile(file->string, &data);
	if (size <= 0 || !data) {
		data    = Om_ReadAppFile(file->string, &size);
		appFile = qtrue;
	}
	if (size <= 0 || !data) {
		Com_Printf("Sound/omaha: HRTF file '%s' not found; headphones fall back to stereo.\n", file->string);
		return qfalse;
	}
	ok = OmHrtf_Load(data, (int)size, s_sampleRate, file->string, 3 * OM_DIRECT_CAP);
	if (appFile) {
		free(data);
	} else {
		FS_FreeFile(data);
	}
	if (!ok) {
		Com_Printf("Sound/omaha: headphones fall back to stereo.\n");
	}
	return ok;
}

/*
 * Added in Omaha: device callback length in ms (s_period_ms, 1..20). Changed:
 * 3 by default. A sound starts at the next callback, so this is the step shots
 * are heard on (stock's Miles mixer stepped 8 ms).
 */
static int Om_PeriodMs(void)
{
	int ms = s_period_ms ? s_period_ms->integer : 3;
	if (ms < 1) {
		ms = 1;
	} else if (ms > 20) {
		ms = 20;
	}
	return ms;
}

/*
 * Added: miniaudio posts these at debug level. ALSA: a write into a drained
 * device (EPIPE, heard as a click), audio thread. WASAPI: the low-latency
 * shared mode was granted, during ma_device_init.
 */
static void Om_MaLog(void *userData, ma_uint32 level, const char *message)
{
	(void)userData;
	if (level != MA_LOG_LEVEL_DEBUG || !message) {
		return;
	}
	if (!strncmp(message, "EPIPE (write)", 13)) {
		s_underruns.fetch_add(1, std::memory_order_relaxed);
	} else if (!strncmp(message, "[WASAPI] Using IAudioClient3", 28)) {
		s_lowLatency = qtrue;
	}
}

/* Opens and starts the playback device on s_context. */
static qboolean Om_StartDevice(const ma_device_id *id)
{
	ma_device_config dcfg;

	dcfg                      = ma_device_config_init(ma_device_type_playback);
	dcfg.playback.format      = ma_format_f32;
	dcfg.playback.channels    = (ma_uint32)s_outCh;
	dcfg.playback.pChannelMap = s_map;
	dcfg.playback.pDeviceID   = id;
	dcfg.sampleRate           = (ma_uint32)s_sampleRate;
	dcfg.dataCallback         = Om_DataCallback;
	/* Changed in Omaha: s_period_ms x 3. HRTF stays on 128-sample blocks via the mix stash. */
	dcfg.periodSizeInMilliseconds = (ma_uint32)Om_PeriodMs();
	dcfg.periods                  = 3;
#ifdef _WIN32
	/*
	 * Added: Windows converts the mixer's rate to the device's, not miniaudio.
	 * miniaudio's converter interpolates linearly and folds the treble back
	 * down (44.1 to 48 kHz: a 10 kHz tone gets a false tone at -21 dB, 15 kHz
	 * at -12 dB), heard as hiss; whatever its filter order. So WASAPI's
	 * low-latency mode, which follows s_period_ms, runs only when the mixer
	 * has the device's rate (headphones, or s_khz 48 on a 48 kHz device).
	 * Otherwise Windows wakes the stream every 10 ms, and the buffer asked for
	 * stays at least 15 ms (5 ms x 3 before the 3 ms default); low-latency
	 * mode doesn't use the period count.
	 */
	dcfg.periods = (ma_uint32)((15 + Om_PeriodMs() - 1) / Om_PeriodMs());
	if (dcfg.periods < 3) {
		dcfg.periods = 3;
	}
#endif
	/* Added: where miniaudio converts the rate (a device refusing the mixer's), the highest filter order. */
	dcfg.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
	if (ma_device_init(&s_context, &dcfg, &s_device) != MA_SUCCESS) {
		return qfalse;
	}
	if (ma_device_start(&s_device) != MA_SUCCESS) {
		ma_device_uninit(&s_device);
		return qfalse;
	}
	return qtrue;
}

#if defined(__linux__) && !defined(__ANDROID__)
/*
 * Added: PipeWire's own ALSA PCM ("pipewire", from pipewire-alsa): a shared
 * PipeWire stream like any other application's, never the card itself. It
 * fails to open when PipeWire isn't running or the plugin isn't installed.
 */
static qboolean Om_OpenPipeWire(void)
{
	ma_backend   alsa[1];
	ma_device_id id;

	alsa[0] = ma_backend_alsa;
	if (ma_context_init(alsa, 1, NULL, &s_context) != MA_SUCCESS) {
		return qfalse;
	}
	s_contextReady = qtrue;
	ma_log_register_callback(ma_context_get_log(&s_context), ma_log_callback_init(Om_MaLog, NULL));
	memset(&id, 0, sizeof(id));
	Q_strncpyz(id.alsa, "pipewire", sizeof(id.alsa));
	if (!Om_StartDevice(&id)) {
		ma_context_uninit(&s_context);
		s_contextReady = qfalse;
		return qfalse;
	}
	return qtrue;
}
#endif

static void Om_LogStarted(void)
{
	const ma_uint32 frames  = s_device.playback.internalPeriodSizeInFrames;
	const ma_uint32 periods = s_device.playback.internalPeriods;
	const ma_uint32 devRate = s_device.playback.internalSampleRate;
	const int       rate    = devRate > 0 ? (int)devRate : s_sampleRate; /* the granted frames are the device's */
	const float     ms      = rate > 0 ? (1000.f * (float)frames / (float)rate) : 0.f;
	const char     *backend = "unknown";

	if (s_nullDevice) {
		backend = "null";
	} else if (s_lowLatency) {
		backend = "WASAPI low-latency";
	} else if (s_device.pContext) {
		backend = ma_get_backend_name(s_device.pContext->backend);
	}
	Com_Printf(
		"Sound/omaha: '%s' started (%d Hz, %s, %d ch, %s, requested %d ms x 3, granted %u frames / %.1f ms x %u)\n",
		s_nullDevice ? "null" : s_device.playback.name,
		s_sampleRate,
		s_hrtfOn ? "headphones" : (s_layout == 2 ? "5.1" : (s_layout == 3 ? "quad" : "stereo")),
		s_outCh,
		backend,
		Om_PeriodMs(),
		(unsigned)frames,
		ms,
		(unsigned)periods
	);
	if (devRate > 0 && (int)devRate != s_sampleRate) {
		Com_Printf("Sound/omaha: device rate %u Hz, mixer %d Hz\n", (unsigned)devRate, s_sampleRate);
	}
#ifdef MA_SUPPORT_WASAPI
	if (!s_nullDevice && s_device.pContext && s_device.pContext->backend == ma_backend_wasapi) {
		Com_Printf(
			"Sound/omaha: WASAPI buffer %u frames\n",
			(unsigned)s_device.wasapi.actualBufferSizeInFramesPlayback
		);
	}
#endif
}

static void Om_DeviceStarted(qboolean nullBackend)
{
	s_deviceReady      = qtrue;
	s_nullDevice       = nullBackend;
	s_seenCallbacks    = s_callbacks.load(std::memory_order_relaxed);
	s_lastCallbackTime = Sys_Milliseconds();
}

static qboolean Om_OpenDevice(const sndDeviceConfig_t *cfg, qboolean nullBackend)
{
	ma_device_id     devId;
	qboolean         haveId = qfalse;
	ma_result        res;
	ma_backend       nulls[1];

	if (s_contextReady) {
		ma_context_uninit(&s_context);
		s_contextReady = qfalse;
	}
	s_underruns.store(0, std::memory_order_relaxed);
	s_lowLatency = qfalse;
	s_underrunsShown = 0;
#if defined(__linux__) && !defined(__ANDROID__)
	/* Added: the default output through PipeWire first; a device named in s_device keeps the default backends. */
	if (!nullBackend && !(cfg && cfg->deviceName && cfg->deviceName[0])) {
		if (Om_OpenPipeWire()) {
			Om_DeviceStarted(qfalse);
			return qtrue;
		}
		Com_Printf("Sound/omaha: PipeWire's ALSA device did not open; using the default backends.\n");
	}
#endif
	if (nullBackend) {
		nulls[0] = ma_backend_null;
		res       = ma_context_init(nulls, 1, NULL, &s_context);
	} else {
		res = ma_context_init(NULL, 0, NULL, &s_context);
	}
	if (res != MA_SUCCESS) {
		return qfalse;
	}
	s_contextReady = qtrue;
	ma_log_register_callback(ma_context_get_log(&s_context), ma_log_callback_init(Om_MaLog, NULL));
	if (!nullBackend && developer && developer->integer) {
		Om_LogDevices();
	}
	haveId = nullBackend ? qfalse : Om_FindDevice(cfg ? cfg->deviceName : NULL, &devId);
	if (!Om_StartDevice(haveId ? &devId : NULL)) {
		ma_context_uninit(&s_context);
		s_contextReady = qfalse;
		return qfalse;
	}
	Om_DeviceStarted(nullBackend);
	return qtrue;
}

/* Shutdown or init only: nothing mixes, so garbage is deleted directly. */
static void Om_ResetState(void)
{
	unsigned i;
	for (i = s_cmdHead.load(std::memory_order_relaxed); i != s_cmdTail.load(std::memory_order_relaxed); i++) {
		omCmd_t *c = &s_cmdRing[i & (OM_CMD_RING - 1)];
		delete c->chunk;
		delete c->retire;
		c->chunk  = NULL;
		c->retire = NULL;
	}
	s_cmdHead.store(0, std::memory_order_relaxed);
	s_cmdTail.store(0, std::memory_order_relaxed);
	while (!s_cmdSpill.empty()) {
		delete s_cmdSpill.front().chunk;
		delete s_cmdSpill.front().retire;
		s_cmdSpill.pop_front();
	}

	/* Voice resets put their chunks on the return list; it is emptied below. */
	Om_FreeTable<omVoiceBlock_t>(s_voiceChunks, s_voiceCap, [](omVoiceBlock_t *blk) {
		int k;
		for (k = 0; k < OM_BLOCK; k++) {
			Om_ResetVoice(&blk->slot[k]);
		}
	});
	Om_FreeTable<omSoundBlock_t>(s_soundChunks, s_soundCap, [](omSoundBlock_t *blk) {
		int k;
		for (k = 0; k < OM_BLOCK; k++) {
			delete blk->slot[k].sound.exchange(NULL, std::memory_order_relaxed);
		}
	});
	Om_FreeTable<omHandleBlock_t>(s_handleChunks, s_handleCap, [](omHandleBlock_t *) {});

	for (i = s_retHead.load(std::memory_order_relaxed); i != s_retTail.load(std::memory_order_relaxed); i++) {
		const omRet_t *r = &s_retRing[i & (OM_RET_RING - 1)];
		if (r->type == OM_RET_SOUND) {
			delete (omSound_t *)r->ptr;
		} else if (r->type == OM_RET_CHUNK) {
			delete (omChunk_t *)r->ptr;
		}
	}
	s_retHead.store(0, std::memory_order_relaxed);
	s_retTail.store(0, std::memory_order_relaxed);
	while (s_soundRetHead) {
		omSound_t *next = s_soundRetHead->next;
		delete s_soundRetHead;
		s_soundRetHead = next;
	}
	while (s_chunkRetHead) {
		omChunk_t *next = s_chunkRetHead->next;
		delete s_chunkRetHead;
		s_chunkRetHead = next;
	}

	delete[] s_rank.exchange(NULL, std::memory_order_relaxed);
	delete[] s_rankG.exchange(NULL, std::memory_order_relaxed);
	Om_FreeRetiredRanks(qtrue);

	s_soundFreeHead  = OM_ID_NONE;
	s_soundIdRetHead = OM_ID_NONE;
	s_handleFreeHead = OM_ID_NONE;
	s_handleRetHead  = OM_ID_NONE;
	s_voiceFreeHead  = OM_ID_NONE;
	s_voiceRetHead   = OM_ID_NONE;
	s_lisDirty.store(0, std::memory_order_relaxed);
	s_stashPos  = 0;
	s_stashLen  = 0;
	s_ambiFlush = 0;
}

/* Game thread: cvars the mixer reads. */
static void Om_SnapshotCvars(void)
{
	s_cv.limiter.store(s_limiter ? s_limiter->integer : 1, std::memory_order_relaxed);
	s_cv.limiterThreshDb.store(
		s_limiter_threshold ? s_limiter_threshold->value : OM_LIMITER_THRESHOLD_DB, std::memory_order_relaxed
	);
	s_cv.obstruction.store(s_obstruction ? s_obstruction->integer : 1, std::memory_order_relaxed);
	s_cv.hfDuck.store(s_hf_duck ? s_hf_duck->integer : 1, std::memory_order_relaxed);
	s_cv.hfDuckDb.store(s_hf_duck_db ? s_hf_duck_db->value : OM_HF_DUCK_DEFAULT_DB, std::memory_order_relaxed);
	s_cv.hfDuckThresh.store(
		s_hf_duck_thresh ? s_hf_duck_thresh->value : OM_HF_DUCK_THRESH_DEFAULT, std::memory_order_relaxed
	);
	s_cv.hfDuckScBoost.store(
		s_hf_duck_sc_boost ? s_hf_duck_sc_boost->value : OM_HF_DUCK_SC_BOOST_DEFAULT, std::memory_order_relaxed
	);
	s_cv.mix.store(s_mix ? s_mix->integer : 1, std::memory_order_relaxed);
}

/* Game thread. Stops the callback for good before the device goes. */
static void Om_CloseDevice(void)
{
	if (s_deviceReady) {
		ma_device_uninit(&s_device);
		s_deviceReady = qfalse;
	}
	if (s_contextReady) {
		ma_context_uninit(&s_context);
		s_contextReady = qfalse;
	}
}

static qboolean Om_Init(const sndDeviceConfig_t *cfg)
{
	int speaker = cfg ? cfg->speakerType : 0;
	s_sampleRate = cfg && cfg->sampleRate > 0 ? cfg->sampleRate : 44100;
	s_rearA      = 1.0f - expf(-2.0f * (float)M_PI * OM_REAR_SHELF_HZ / (float)s_sampleRate);
	s_hrtfOn     = qfalse;
	s_layout     = 0;
	memset(&s_cfg, 0, sizeof(s_cfg));
	s_cfgDevice[0] = 0;
	if (cfg) {
		s_cfg = *cfg;
		if (cfg->deviceName) {
			Q_strncpyz(s_cfgDevice, cfg->deviceName, sizeof(s_cfgDevice));
		}
	}
	s_cfg.deviceName = s_cfgDevice;
	OmHrtf_Unload();
	if (speaker == 1) {
		s_hrtfOn = Om_LoadHrtf();
		s_layout = s_hrtfOn ? 1 : 0;
	} else if (speaker == 2) {
		s_layout = 2;
	} else if (speaker == 3) {
		s_layout = 3;
	}
	Om_SetupSpeakers();
	Om_BuildSinc();
	Om_BuildStereoPair();
	/* Added in Omaha: playback callback length in ms. Takes effect on snd_restart. */
	s_period_ms         = Cvar_Get("s_period_ms", "3", CVAR_ARCHIVE | CVAR_SOUND_LATCH);
	Cvar_CheckRange(s_period_ms, 1, 20, qtrue);
	s_limiter           = Cvar_Get("s_limiter", "1", CVAR_ARCHIVE);
	s_limiter_threshold = Cvar_Get("s_limiter_threshold", "-3.6", CVAR_ARCHIVE);
	s_obstruction       = Cvar_Get("s_obstruction", "1", CVAR_ARCHIVE);
	s_hf_duck           = Cvar_Get("s_hf_duck", "1", CVAR_ARCHIVE);
	s_hf_duck_db        = Cvar_Get("s_hf_duck_db", "24", CVAR_ARCHIVE);
	s_hf_duck_thresh    = Cvar_Get("s_hf_duck_thresh", "-15", CVAR_ARCHIVE);
	s_hf_duck_sc_boost  = Cvar_Get("s_hf_duck_sc_boost", "5", CVAR_ARCHIVE);
	s_mix               = Cvar_Get("s_mix", "1", CVAR_ARCHIVE);
	Om_SnapshotCvars();
	s_limCalibDb = 1.0e9f;
	Om_LimiterReset();
	Om_SpkEqReset();
	Om_HpEqReset();
	Om_HfReset();
	Om_ResetState();
	s_lisGainTarget.store(1.f, std::memory_order_relaxed);
	VectorClear(s_lisPosGame);
	s_masterCur     = 1.f;
	s_masterPrimed  = qfalse;
	s_deviceLost    = false;
	s_lastReopen = 0;
	if (!Om_OpenDevice(&s_cfg, qfalse)) {
		Com_Printf("Sound/omaha: no playback device, using a null device (snd_restart tries again).\n");
		if (!Om_OpenDevice(&s_cfg, qtrue)) {
			Com_Printf("Sound/omaha: null device failed\n");
			OmHrtf_Unload();
			s_hrtfOn = qfalse;
			return qfalse;
		}
	}
	Om_LogStarted();
	return qtrue;
}

static void Om_Shutdown(void)
{
	Om_CloseDevice();
	Om_ResetState();
	OmHrtf_Unload();
	s_hrtfOn     = qfalse;
	s_deviceLost = false;
}

/*
 * Added in Omaha: reopen after a lost device. The configured device first, then
 * the default, then the null device so voices keep advancing. At most once a second.
 */
static void Om_RecoverDevice(void)
{
	sndDeviceConfig_t cfg;
	const int         now = Sys_Milliseconds();
	if (!s_deviceLost || (s_lastReopen && now - s_lastReopen < 1000)) {
		return;
	}
	s_lastReopen = now;
	s_deviceLost = false;
	Com_Printf("Sound/omaha: playback device stopped, reopening.\n");
	Om_CloseDevice();
	cfg = s_cfg;
	if (Om_OpenDevice(&cfg, qfalse)) {
		Om_LogStarted();
		return;
	}
	cfg.deviceName = NULL;
	if (s_cfgDevice[0] && Om_OpenDevice(&cfg, qfalse)) {
		Om_LogStarted();
		return;
	}
	if (Om_OpenDevice(&cfg, qtrue)) {
		Com_Printf("Sound/omaha: no playback device, using a null device.\n");
		Om_LogStarted();
		return;
	}
	Com_Printf("Sound/omaha: could not reopen any device; retrying.\n");
	s_deviceLost = true;
}

static omSoundId_t Om_CreateSound(void)
{
	omSoundId_t    id;
	omSoundSlot_t *slot;
	Om_Reclaim();
	id   = Om_TakeSound();
	slot = Om_SoundSlot(id);
	if (slot) {
		slot->live = qtrue;
	}
	return id;
}

static void Om_DestroySound(omSoundId_t id)
{
	omCmd_t        c;
	omSoundSlot_t *slot = Om_SoundSlot(id);
	/* Added in Omaha: a second destroy would put the id on the free list twice. */
	if (!slot || !slot->live) {
		return;
	}
	slot->live = qfalse;
	memset(&c, 0, sizeof(c));
	c.op    = OM_OP_FREE_SOUND;
	c.sound = id;
	Om_Push(c);
}

static qboolean Om_UploadSound(
	omSoundId_t id, omSampleFormat_t fmt, const void *data, int sizeBytes, int rate, int blockAlign, int frames
)
{
	omSound_t *s, *old;
	omSoundSlot_t *slot = Om_SoundSlot(id);
	if (!slot || !slot->live) {
		return qfalse;
	}
	s = Om_DecodeSound(fmt, data, sizeBytes, rate, blockAlign, frames);
	if (!s) {
		return qfalse;
	}
	old = slot->sound.exchange(s, std::memory_order_acq_rel);
	if (old) {
		/* The mixer may still be reading it this block; it comes back through the return ring. */
		omCmd_t c;
		memset(&c, 0, sizeof(c));
		c.op     = OM_OP_RETIRE_SOUND;
		c.retire = old;
		Om_Push(c);
	}
	return qtrue;
}

static int Om_SoundRate(omSoundId_t id)
{
	const omSound_t *s;
	omSoundSlot_t *slot = Om_SoundSlot(id);
	if (!slot) {
		return 22050;
	}
	s = slot->sound.load(std::memory_order_acquire);
	return s && s->rate > 0 ? s->rate : 22050;
}

/* Game thread. */
static int Om_AllocHandle(qboolean stream)
{
	omHandle_t *h;
	int         id = Om_TakeHandle();
	if (id == OM_VOICE_NONE) {
		return OM_VOICE_NONE;
	}
	h         = Om_HandleAt(id);
	h->stream = stream;
	h->voice  = -1;
	h->dead   = qfalse;
	h->state.store(stream ? OM_VOICE_INITIAL : OM_VOICE_PLAYING);
	h->offset.store(0);
	h->queued.store(0);
	h->processed.store(0);
	h->pendingUnqueue.store(0);
	h->dirty.store(0);
	h->used.store(1);
	return id;
}

static qboolean Om_HandleLive(omVoiceId_t id)
{
	omHandle_t *h = Om_HandleAt(id);
	return h && h->used.load() ? qtrue : qfalse;
}

static omVoiceId_t Om_StartVoice(int op, omSoundId_t sound, const omVoiceParams_t *params, unsigned offset)
{
	omCmd_t c;
	int     id;
	Om_Reclaim();
	memset(&c, 0, sizeof(c));
	c.op     = op;
	c.sound  = sound;
	c.offset = offset;
	if (params) {
		c.params = *params;
	} else {
		Om_DefaultParams(&c.params);
	}
	id = Om_AllocHandle(op == OM_OP_OPEN ? qtrue : qfalse);
	if (id == OM_VOICE_NONE) {
		return OM_VOICE_NONE;
	}
	c.slot = Om_TakeVoice();
	if (c.slot < 0) {
		Om_ReturnHandle(id);
		return OM_VOICE_NONE;
	}
	Om_HandleAt(id)->offset.store(offset);
	c.id = id;
	Om_Push(c);
	return id;
}

static omVoiceId_t Om_PlayVoice(omSoundId_t sound, const omVoiceParams_t *params, unsigned offset)
{
	omSoundSlot_t *slot = Om_SoundSlot(sound);
	if (!slot || !slot->live) {
		return OM_VOICE_NONE;
	}
	return Om_StartVoice(OM_OP_PLAY, sound, params, offset);
}

static omVoiceId_t Om_OpenStream(const omVoiceParams_t *params)
{
	return Om_StartVoice(OM_OP_OPEN, OM_SOUND_NONE, params, 0);
}

static void Om_VoiceCmd(int op, omVoiceId_t id, int state)
{
	omCmd_t c;
	omHandle_t *h;
	if (!Om_HandleLive(id)) {
		return;
	}
	h = Om_HandleAt(id);
	memset(&c, 0, sizeof(c));
	c.op = op;
	c.id = id;
	c.slot = OM_ID_NONE;
	if (state >= 0 && h) {
		h->state.store(state);
	}
	Om_Push(c);
	/*
	 * Changed: the mixer frees the handle on this command; until it comes back
	 * (Om_Reclaim) the game thread sends nothing more for the id, which a later
	 * PlayVoice may get again.
	 */
	if (h && (op == OM_OP_DESTROY || (op == OM_OP_STOP && !h->stream))) {
		h->used.store(0, std::memory_order_relaxed);
	}
}

/* Changed in Omaha: params go to the handle mailbox; only the newest reaches the mixer. */
static void Om_UpdateVoice(omVoiceId_t id, const omVoiceParams_t *params)
{
	omHandle_t *h;
	if (!params || !Om_HandleLive(id)) {
		return;
	}
	h = Om_HandleAt(id);
	Om_SeqWrite(h->mailSeq, h->mail, params, OM_PARAM_WORDS);
	h->dirty.store(1, std::memory_order_release);
}

static void Om_StopVoice(omVoiceId_t id)
{
	Om_VoiceCmd(OM_OP_STOP, id, OM_VOICE_STOPPED);
}

static void Om_PauseVoice(omVoiceId_t id)
{
	Om_VoiceCmd(OM_OP_PAUSE, id, OM_VOICE_PAUSED);
}

static void Om_ResumeVoice(omVoiceId_t id)
{
	Om_VoiceCmd(OM_OP_RESUME, id, OM_VOICE_PLAYING);
}

static void Om_DestroyVoice(omVoiceId_t id)
{
	Om_VoiceCmd(OM_OP_DESTROY, id, OM_VOICE_STOPPED);
}

static int Om_VoiceState(omVoiceId_t id)
{
	omHandle_t *h = Om_HandleAt(id);
	if (!h || !h->used.load()) {
		return OM_VOICE_STOPPED;
	}
	return h->state.load();
}

static unsigned Om_GetOffset(omVoiceId_t id)
{
	omHandle_t *h = Om_HandleAt(id);
	if (!h || !h->used.load()) {
		return 0;
	}
	return h->offset.load(std::memory_order_relaxed);
}

static void Om_SetOffset(omVoiceId_t id, unsigned offset)
{
	omCmd_t c;
	if (!Om_HandleLive(id)) {
		return;
	}
	memset(&c, 0, sizeof(c));
	c.op     = OM_OP_SEEK;
	c.id     = id;
	c.slot   = OM_ID_NONE;
	c.offset = offset;
	Om_HandleAt(id)->offset.store(offset, std::memory_order_relaxed);
	Om_Push(c);
}

static int Om_StreamQueued(omVoiceId_t id);

static qboolean Om_QueueStream(omVoiceId_t id, const void *pcm, int bytes, omSampleFormat_t fmt, int rate, int blockAlign)
{
	omCmd_t    c;
	omChunk_t *chunk;
	if (!Om_HandleLive(id) || Om_StreamQueued(id) >= OM_STREAM_CHUNKS) {
		return qfalse;
	}
	chunk = Om_MakeChunk(fmt, pcm, bytes, rate, blockAlign);
	if (!chunk) {
		return qfalse;
	}
	if (chunk->frames <= 0) {
		delete chunk;
		return qfalse;
	}
	memset(&c, 0, sizeof(c));
	c.op    = OM_OP_QUEUE;
	c.id    = id;
	c.slot  = OM_ID_NONE;
	c.chunk = chunk;
	Om_HandleAt(id)->queued.fetch_add(1);
	Om_Push(c);
	return qtrue;
}

static int Om_StreamQueued(omVoiceId_t id)
{
	int q;
	omHandle_t *h = Om_HandleAt(id);
	if (!h || !h->used.load()) {
		return 0;
	}
	q = h->queued.load() - h->pendingUnqueue.load();
	return q > 0 ? q : 0;
}

static int Om_StreamProcessed(omVoiceId_t id)
{
	int p;
	omHandle_t *h = Om_HandleAt(id);
	if (!h || !h->used.load()) {
		return 0;
	}
	p = h->processed.load() - h->pendingUnqueue.load();
	return p > 0 ? p : 0;
}

static void Om_UnqueueStream(omVoiceId_t id, int count)
{
	int i;
	if (!Om_HandleLive(id)) {
		return;
	}
	for (i = 0; i < count; i++) {
		omCmd_t c;
		memset(&c, 0, sizeof(c));
		c.op = OM_OP_UNQUEUE;
		c.id = id;
		c.slot = OM_ID_NONE;
		Om_HandleAt(id)->pendingUnqueue.fetch_add(1);
		Om_Push(c);
	}
}

/* Changed in Omaha: the listener goes to a mailbox; the position stays readable game-side. */
static void Om_SetListener(
	const float pos[3], const float vel[3], const float forward[3], const float left[3], const float up[3]
)
{
	float l[OM_LISTENER_WORDS];
	memcpy(l + 0, pos, sizeof(float) * 3);
	if (vel) {
		memcpy(l + 3, vel, sizeof(float) * 3);
	} else {
		memset(l + 3, 0, sizeof(float) * 3);
	}
	memcpy(l + 6, forward, sizeof(float) * 3);
	memcpy(l + 9, left, sizeof(float) * 3);
	memcpy(l + 12, up, sizeof(float) * 3);
	memcpy(s_lisPosGame, pos, sizeof(s_lisPosGame));
	Om_SeqWrite(s_lisSeq, s_lisMail, l, OM_LISTENER_WORDS);
	s_lisDirty.store(1, std::memory_order_release);
}

static void Om_SetListenerGain(float g)
{
	if (Om_Finite(g)) {
		s_lisGainTarget.store(g, std::memory_order_relaxed);
	}
}

static void Om_GetListenerPosition(float out[3])
{
	memcpy(out, s_lisPosGame, sizeof(s_lisPosGame));
}

/* Game thread: whether the device's callbacks stopped (a lost device, or one that never started). */
static qboolean Om_DeviceStalled(int now, int ms)
{
	const unsigned n = s_callbacks.load(std::memory_order_relaxed);

	if (n != s_seenCallbacks) {
		s_seenCallbacks    = n;
		s_lastCallbackTime = now;
		return qfalse;
	}
	return now - s_lastCallbackTime > ms ? qtrue : qfalse;
}

static void Om_Update(void)
{
	const int now = Sys_Milliseconds();

	Om_SnapshotCvars();
	Om_Reclaim();
	Om_FlushSpill();
	{
		/* Added: report the device running dry (once a second at most). */
		const unsigned underruns = s_underruns.load(std::memory_order_relaxed);
		if (underruns != s_underrunsShown && (!s_underrunPrinted || now - s_underrunPrinted >= 1000)) {
			Com_Printf(
				"Sound/omaha: the device ran dry %u time(s) since it started (a click each; a higher s_period_ms gives more margin)\n",
				underruns
			);
			s_underrunsShown  = underruns;
			s_underrunPrinted = now;
		}
	}
	if (s_deviceReady && Om_DeviceStalled(now, OM_DEVICE_STALL_MS)) {
		s_deviceLost = true;
	}
	Om_RecoverDevice();
	/* No callback is running: apply commands here so handles and rings stay bounded. */
	if ((!s_deviceReady || Om_DeviceStalled(now, 100)) && !s_mixGate.test_and_set(std::memory_order_acquire)) {
		Om_MixGated(NULL, 0, s_outCh);
		s_mixGate.clear(std::memory_order_release);
		Om_Reclaim();
	}
}

static const sndRenderer_t s_omahaRenderer = {
	"omaha",
	Om_Init,
	Om_Shutdown,
	Om_CreateSound,
	Om_DestroySound,
	Om_UploadSound,
	Om_SoundRate,
	Om_PlayVoice,
	Om_OpenStream,
	Om_UpdateVoice,
	Om_StopVoice,
	Om_PauseVoice,
	Om_ResumeVoice,
	Om_DestroyVoice,
	Om_VoiceState,
	Om_GetOffset,
	Om_SetOffset,
	Om_QueueStream,
	Om_StreamQueued,
	Om_StreamProcessed,
	Om_UnqueueStream,
	Om_SetListener,
	Om_SetListenerGain,
	Om_GetListenerPosition,
	Om_Update,
};

extern "C" const sndRenderer_t *S_Renderer_Omaha(void)
{
	return &s_omahaRenderer;
}

extern "C" float S_Omaha_HfDuckGainDb(void)
{
	return s_hfMeterOut.load(std::memory_order_relaxed);
}

extern "C" float S_Omaha_HfDuckMaxCutDb(void)
{
	float maxCut = s_hf_duck_db ? s_hf_duck_db->value : OM_HF_DUCK_DEFAULT_DB;
	if (maxCut < 0.f) {
		maxCut = -maxCut;
	}
	if (maxCut > 24.f) {
		maxCut = 24.f;
	}
	return maxCut;
}

extern "C" float S_Omaha_HfDuckSidechainDb(void)
{
	return s_hfSideOut.load(std::memory_order_relaxed);
}

extern "C" float S_Omaha_HfDuckThreshDb(void)
{
	return s_hf_duck_thresh ? s_hf_duck_thresh->value : OM_HF_DUCK_THRESH_DEFAULT;
}
