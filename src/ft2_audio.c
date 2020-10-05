// for finding memory leaks in debug mode with Visual Studio
#if defined _DEBUG && defined _MSC_VER
#include <crtdbg.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include "ft2_header.h"
#include "ft2_config.h"
#include "ft2_scopes.h"
#include "ft2_video.h"
#include "ft2_gui.h"
#include "ft2_midi.h"
#include "ft2_wav_renderer.h"
#include "ft2_tables.h"
#include "ft2_structs.h"
// --------------------------------
#include "mixer/ft2_mix.h"
#include "mixer/ft2_center_mix.h"
#include "mixer/ft2_silence_mix.h"
// --------------------------------

#define INITIAL_DITHER_SEED 0x12345000

static int8_t pmpCountDiv, pmpChannels = 2;
static uint16_t smpBuffSize;
static int32_t randSeed = INITIAL_DITHER_SEED;
static uint32_t oldAudioFreq, tickTimeLen, tickTimeLenFrac;
static float fAudioNormalizeMul, fPrngStateL, fPrngStateR, fPanningTab[256+1];
static voice_t voice[MAX_VOICES * 2];
static void (*sendAudSamplesFunc)(uint8_t *, uint32_t, uint8_t); // "send mixed samples" routines

static int32_t oldPeriod;
static uint32_t oldRevDelta;
static uint64_t oldDelta;

// globalized
audio_t audio;
pattSyncData_t *pattSyncEntry;
chSyncData_t *chSyncEntry;
chSync_t chSync;
pattSync_t pattSync;
volatile bool pattQueueClearing, chQueueClearing;

void resetCachedMixerVars(void)
{
	oldPeriod = -1;
	oldDelta = 0;
	oldRevDelta = UINT32_MAX;
}

void stopVoice(int32_t i)
{
	voice_t *v;

	v = &voice[i];
	memset(v, 0, sizeof (voice_t));
	v->pan = 128;

	// clear "fade out" voice too

	v = &voice[MAX_VOICES + i];
	memset(v, 0, sizeof (voice_t));
	v->pan = 128;
}

bool setNewAudioSettings(void) // only call this from the main input/video thread
{
	uint32_t stringLen;

	pauseAudio();

	if (!setupAudio(CONFIG_HIDE_ERRORS))
	{
		// set back old known working settings

		config.audioFreq = audio.lastWorkingAudioFreq;
		config.specialFlags &= ~(BITDEPTH_16 + BITDEPTH_32 + BUFFSIZE_512 + BUFFSIZE_1024 + BUFFSIZE_2048);
		config.specialFlags |= audio.lastWorkingAudioBits;

		if (audio.lastWorkingAudioDeviceName != NULL)
		{
			if (audio.currOutputDevice != NULL)
			{
				free(audio.currOutputDevice);
				audio.currOutputDevice = NULL;
			}

			stringLen = (uint32_t)strlen(audio.lastWorkingAudioDeviceName);

			audio.currOutputDevice = (char *)malloc(stringLen + 2);
			if (audio.currOutputDevice != NULL)
			{
				strcpy(audio.currOutputDevice, audio.lastWorkingAudioDeviceName);
				audio.currOutputDevice[stringLen + 1] = '\0'; // UTF-8 needs double null termination
			}
		}

		// also update config audio radio buttons if we're on that screen at the moment
		if (ui.configScreenShown && editor.currConfigScreen == CONFIG_SCREEN_IO_DEVICES)
			setConfigIORadioButtonStates();

		// if it didn't work to use the old settings again, then something is seriously wrong...
		if (!setupAudio(CONFIG_HIDE_ERRORS))
			okBox(0, "System message", "Couldn't find a working audio mode... You'll get no sound / replayer timer!");

		resumeAudio();
		return false;
	}

	calcRevMixDeltaTable();
	resumeAudio();

	setWavRenderFrequency(audio.freq);
	setWavRenderBitDepth((config.specialFlags & BITDEPTH_32) ? 32 : 16);
	return true;
}

// amp = 1..32, masterVol = 0..256
void setAudioAmp(int16_t amp, int16_t masterVol, bool bitDepth32Flag)
{
	amp = CLAMP(amp, 1, 32);
	masterVol = CLAMP(masterVol, 0, 256);

	float fAmp = (amp * masterVol) / (32.0f * 256.0f);
	if (!bitDepth32Flag)
		fAmp *= 32768.0f;

	fAudioNormalizeMul = fAmp;
}

void setNewAudioFreq(uint32_t freq) // for song-to-WAV rendering
{
	if (freq == 0)
		return;

	oldAudioFreq = audio.freq;
	audio.freq = freq;

	const bool mustRecalcTables = audio.freq != oldAudioFreq;
	if (mustRecalcTables)
	{
		calcReplayRate(audio.freq);
		calcRevMixDeltaTable();
	}
}

void setBackOldAudioFreq(void) // for song-to-WAV rendering
{
	const bool mustRecalcTables = audio.freq != oldAudioFreq;

	audio.freq = oldAudioFreq;

	if (mustRecalcTables)
	{
		calcReplayRate(audio.freq);
		calcRevMixDeltaTable();
	}
}

void P_SetSpeed(uint16_t bpm)
{
	if (bpm == 0)
		return;

	// non-FT2 check for security
	if (bpm > MAX_BPM)
		return;

	audio.dSamplesPerTick = audio.dSamplesPerTickTab[bpm];
	audio.samplesPerTick = (int32_t)(audio.dSamplesPerTick + 0.5);

	// get tick time length for audio/video sync timestamp
	const uint64_t tickTimeLen64 = audio.tickTimeTab[bpm];
	tickTimeLen = tickTimeLen64 >> 32;
	tickTimeLenFrac = tickTimeLen64 & UINT32_MAX;

	// used for calculating volume ramp length for "ticks" ramps
	audio.fRampTickMul = audio.fRampTickMulTab[bpm];
}

void audioSetVolRamp(bool volRamp)
{
	lockMixerCallback();
	audio.volumeRampingFlag = volRamp;
	unlockMixerCallback();
}

void audioSetInterpolationType(uint8_t interpolationType)
{
	lockMixerCallback();
	audio.interpolationType = interpolationType;
	unlockMixerCallback();
}

void calcPanningTable(void)
{
	// same formula as FT2's panning table (with 0.0f..1.0f range)
	for (int32_t i = 0; i <= 256; i++)
		fPanningTab[i] = sqrtf(i / 256.0f);
}

static void voiceUpdateVolumes(int32_t i, uint8_t status)
{
	float fDestVolL, fDestVolR;

	voice_t *v = &voice[i];

	const float fVolL = v->fVol * fPanningTab[256-v->pan];
	const float fVolR = v->fVol * fPanningTab[    v->pan];

	if (!audio.volumeRampingFlag)
	{
		// volume ramping is disabled
		v->fVolL = fVolL;
		v->fVolR = fVolR;
		v->volRampSamples = 0;
		return;
	}

	v->fDestVolL = fVolL;
	v->fDestVolR = fVolR;

	if (status & IS_NyTon)
	{
		// sample is about to start, ramp out/in at the same time

		// setup "fade out" voice (only if current voice volume > 0)
		if (v->fVolL > 0.0f || v->fVolR > 0.0f)
		{
			voice_t *f = &voice[MAX_VOICES+i];

			*f = *v; // copy voice

			f->volRampSamples = audio.quickVolRampSamples;

			fDestVolL = -f->fVolL;
			fDestVolR = -f->fVolR;

			f->fVolDeltaL = fDestVolL * audio.fRampQuickVolMul;
			f->fVolDeltaR = fDestVolR * audio.fRampQuickVolMul;

			f->isFadeOutVoice = true;
		}

		// make current voice fade in from zero when it starts
		v->fVolL = 0.0f;
		v->fVolR = 0.0f;
	}

	// ramp volume changes

	/* FT2 has two internal volume ramping lengths:
	** IS_QuickVol: 5ms
	** Normal: The duration of a tick (samplesPerTick)
	*/

	// if destination volume and current volume is the same (and we have no sample trigger), don't do ramp
	if (fVolL == v->fVolL && fVolR == v->fVolR && !(status & IS_NyTon))
	{
		// there is no volume change
		v->volRampSamples = 0;
	}
	else
	{
		fDestVolL = fVolL - v->fVolL;
		fDestVolR = fVolR - v->fVolR;

		if (status & IS_QuickVol)
		{
			v->volRampSamples = audio.quickVolRampSamples;
			v->fVolDeltaL = fDestVolL * audio.fRampQuickVolMul;
			v->fVolDeltaR = fDestVolR * audio.fRampQuickVolMul;
		}
		else
		{
			v->volRampSamples = audio.samplesPerTick;
			v->fVolDeltaL = fDestVolL * audio.fRampTickMul;
			v->fVolDeltaR = fDestVolR * audio.fRampTickMul;
		}
	}
}

static void voiceTrigger(int32_t i, sampleTyp *s, int32_t position)
{
	bool sampleIs16Bit;
	uint8_t loopType;
	int32_t length, loopStart, loopLength, loopEnd;

	voice_t *v = &voice[i];
	length = s->len;
	loopStart = s->repS;
	loopLength = s->repL;
	loopEnd = s->repS + s->repL;
	loopType = s->typ & 3;
	sampleIs16Bit = (s->typ >> 4) & 1;

	if (sampleIs16Bit)
	{
		assert(!(length & 1));
		assert(!(loopStart & 1));
		assert(!(loopLength & 1));
		assert(!(loopEnd & 1));

		length >>= 1;
		loopStart >>= 1;
		loopLength >>= 1;
		loopEnd >>= 1;
	}

	if (s->pek == NULL || length < 1)
	{
		v->active = false; // shut down voice (illegal parameters)
		return;
	}

	if (loopLength < 1) // disable loop if loopLength is below 1
		loopType = 0;

	if (sampleIs16Bit)
	{
		v->base16 = (const int16_t *)s->pek;
		v->revBase16 = &v->base16[loopStart + loopEnd]; // for pingpong loops

		// first tap [-1] sample for special case: if (hasLooped && pos == loopStart)
		if (loopType == 1)
			v->fTapFixSample = v->base16[loopEnd-1];
		else if (loopType == 2)
			v->fTapFixSample = v->base16[loopStart];
	}
	else
	{
		v->base8 = s->pek;
		v->revBase8 = &v->base8[loopStart + loopEnd]; // for pingpong loops

		// first tap [-1] sample for special case: if (hasLooped && pos == loopStart)
		if (loopType == 1)
			v->fTapFixSample = v->base8[loopEnd-1];
		else if (loopType == 2)
			v->fTapFixSample = v->base8[loopStart];
	}

	v->hasLooped = false; // for cubic interpolation special case (read fTapFixSample comment above)

	v->backwards = false;
	v->loopType = loopType;
	v->end = (loopType > 0) ? loopEnd : length;
	v->loopStart = loopStart;
	v->loopLength = loopLength;
	v->pos = position;
	v->posFrac = 0;

	// if position overflows, shut down voice (f.ex. through 9xx command)
	if (v->pos >= v->end)
	{
		v->active = false;
		return;
	}

	v->mixFuncOffset = (sampleIs16Bit * 9) + (audio.interpolationType * 3) + loopType;
	v->active = true;
}

void resetRampVolumes(void)
{
	voice_t *v = voice;
	for (int32_t i = 0; i < song.antChn; i++, v++)
	{
		v->fVolL = v->fDestVolL;
		v->fVolR = v->fDestVolR;
		v->volRampSamples = 0;
	}
}

void updateVoices(void)
{
	uint8_t status;
	stmTyp *ch;
	voice_t *v;

	ch = stm;
	v = voice;

	for (int32_t i = 0; i < song.antChn; i++, ch++, v++)
	{
		status = ch->tmpStatus = ch->status; // (tmpStatus is used for audio/video sync queue)
		if (status == 0) continue; // nothing to do
		ch->status = 0;

		if (status & IS_Vol)
			v->fVol = ch->fFinalVol;

		if (status & IS_Pan)
			v->pan = ch->finalPan;

		if (status & (IS_Vol + IS_Pan))
			voiceUpdateVolumes(i, status);

		if (status & IS_Period)
		{
			// use cached values if possible
			const uint16_t period = ch->finalPeriod;
			if (period != oldPeriod)
			{
				oldPeriod = period;
				oldDelta = getMixerDelta(period);
				oldRevDelta = getRevMixerDelta(period);
			}

			v->delta = oldDelta;
			v->revDelta = oldRevDelta;
		}

		if (status & IS_NyTon)
			voiceTrigger(i, ch->smpPtr, ch->smpStartPos);
	}
}

void resetAudioDither(void)
{
	randSeed = INITIAL_DITHER_SEED;
	fPrngStateL = 0.0f;
	fPrngStateR = 0.0f;
}

static inline int32_t random32(void)
{
	// LCG 32-bit random
	randSeed *= 134775813;
	randSeed++;

	return (int32_t)randSeed;
}

static void sendSamples16BitDitherStereo(uint8_t *stream, uint32_t sampleBlockLength, uint8_t numAudioChannels)
{
	int32_t out32;
	float fOut, fPrng;

	int16_t *streamPointer16 = (int16_t *)stream;
	for (uint32_t i = 0; i < sampleBlockLength; i++)
	{
		// left channel - 1-bit triangular dithering
		fPrng = random32() * (0.5f / INT32_MAX); // -0.5f .. 0.5f
		fOut = ((audio.fMixBufferL[i] * fAudioNormalizeMul) + fPrng) - fPrngStateL;
		fPrngStateL = fPrng;
		out32 = (int32_t)fOut;
		CLAMP16(out32);
		*streamPointer16++ = (int16_t)out32;

		// right channel - 1-bit triangular dithering
		fPrng = random32() * (0.5f / INT32_MAX); // -0.5f .. 0.5f
		fOut = ((audio.fMixBufferR[i] * fAudioNormalizeMul) + fPrng) - fPrngStateR;
		fPrngStateR = fPrng;
		out32 = (int32_t)fOut;
		CLAMP16(out32);
		*streamPointer16++ = (int16_t)out32;
	}

	(void)numAudioChannels;
}

static void sendSamples16BitDitherMultiChan(uint8_t *stream, uint32_t sampleBlockLength, uint8_t numAudioChannels)
{
	int32_t out32;
	float fOut, fPrng;

	int16_t *streamPointer16 = (int16_t *)stream;
	for (uint32_t i = 0; i < sampleBlockLength; i++)
	{
		// left channel - 1-bit triangular dithering
		fPrng = random32() * (0.5f / INT32_MAX); // -0.5f..0.5f
		fOut = ((audio.fMixBufferL[i] * fAudioNormalizeMul) + fPrng) - fPrngStateL;
		fPrngStateL = fPrng;
		out32 = (int32_t)fOut;
		CLAMP16(out32);
		*streamPointer16++ = (int16_t)out32;

		// right channel - 1-bit triangular dithering
		fPrng = random32() * (0.5f / INT32_MAX); // -0.5f..0.5f
		fOut = ((audio.fMixBufferR[i] * fAudioNormalizeMul) + fPrng) - fPrngStateR;
		fPrngStateR = fPrng;
		out32 = (int32_t)fOut;
		CLAMP16(out32);
		*streamPointer16++ = (int16_t)out32;

		// send zeroes to the rest of the channels
		for (uint32_t j = 2; j < numAudioChannels; j++)
			*streamPointer16++ = 0;
	}
}

static void sendSamples32BitStereo(uint8_t *stream, uint32_t sampleBlockLength, uint8_t numAudioChannels)
{
	float fOut;

	float *fStreamPointer32 = (float *)stream;
	for (uint32_t i = 0; i < sampleBlockLength; i++)
	{
		// left channel
		fOut = audio.fMixBufferL[i] * fAudioNormalizeMul;
		fOut = CLAMP(fOut, -1.0f, 1.0f);
		*fStreamPointer32++ = fOut;

		// right channel
		fOut = audio.fMixBufferR[i] * fAudioNormalizeMul;
		fOut = CLAMP(fOut, -1.0f, 1.0f);
		*fStreamPointer32++ = fOut;
	}

	(void)numAudioChannels;
}

static void sendSamples32BitMultiChan(uint8_t *stream, uint32_t sampleBlockLength, uint8_t numAudioChannels)
{
	float fOut;

	float *fStreamPointer32 = (float *)stream;
	for (uint32_t i = 0; i < sampleBlockLength; i++)
	{
		// left channel
		fOut = audio.fMixBufferL[i] * fAudioNormalizeMul;
		fOut = CLAMP(fOut, -1.0f, 1.0f);
		*fStreamPointer32++ = fOut;

		// right channel
		fOut = audio.fMixBufferR[i] * fAudioNormalizeMul;
		fOut = CLAMP(fOut, -1.0f, 1.0f);
		*fStreamPointer32++ = fOut;

		// send zeroes to the rest of the channels
		for (uint32_t j = 2; j < numAudioChannels; j++)
			*fStreamPointer32++ = 0.0f;
	}
}

static void doChannelMixing(int32_t samplesToMix)
{
	voice_t *v = voice; // normal voices
	voice_t *r = &voice[MAX_VOICES]; // volume ramp fadeout-voices

	for (int32_t i = 0; i < song.antChn; i++, v++, r++)
	{
		if (v->active)
		{
			bool centerMixFlag;

			const bool volRampFlag = v->volRampSamples > 0;
			if (volRampFlag)
			{
				centerMixFlag = (v->fDestVolL == v->fDestVolR) && (v->fVolDeltaL == v->fVolDeltaR);
			}
			else
			{
				if (v->fVolL == 0.0f && v->fVolR == 0.0f)
				{ 
					silenceMixRoutine(v, samplesToMix);
					continue;
				}

				centerMixFlag = v->fVolL == v->fVolR;
			}

			mixFuncTab[(centerMixFlag * 36) + (volRampFlag * 18) + v->mixFuncOffset](v, samplesToMix);
		}

		if (r->active) // volume ramp fadeout-voice
		{
			const bool centerMixFlag = (r->fDestVolL == r->fDestVolR) && (r->fVolDeltaL == r->fVolDeltaR);
			mixFuncTab[(centerMixFlag * 36) + 18 + r->mixFuncOffset](r, samplesToMix);
		}
	}
}

static void mixAudio(uint8_t *stream, uint32_t sampleBlockLength, uint8_t numAudioChannels)
{
	assert(sampleBlockLength <= MAX_WAV_RENDER_SAMPLES_PER_TICK);
	memset(audio.fMixBufferL, 0, sampleBlockLength * sizeof (int32_t));
	memset(audio.fMixBufferR, 0, sampleBlockLength * sizeof (int32_t));

	doChannelMixing(sampleBlockLength);

	// normalize mix buffer and send to audio stream
	sendAudSamplesFunc(stream, sampleBlockLength, numAudioChannels);
}

// used for song-to-WAV renderer
void mixReplayerTickToBuffer(uint32_t samplesToMix, uint8_t *stream, uint8_t bitDepth)
{
	assert(samplesToMix <= MAX_WAV_RENDER_SAMPLES_PER_TICK);
	memset(audio.fMixBufferL, 0, samplesToMix * sizeof (int32_t));
	memset(audio.fMixBufferR, 0, samplesToMix * sizeof (int32_t));

	doChannelMixing(samplesToMix);

	// normalize mix buffer and send to audio stream
	if (bitDepth == 16)
		sendSamples16BitDitherStereo(stream, samplesToMix, 2);
	else
		sendSamples32BitStereo(stream, samplesToMix, 2);
}

int32_t pattQueueReadSize(void)
{
	while (pattQueueClearing);

	if (pattSync.writePos > pattSync.readPos)
		return pattSync.writePos - pattSync.readPos;
	else if (pattSync.writePos < pattSync.readPos)
		return pattSync.writePos - pattSync.readPos + SYNC_QUEUE_LEN + 1;
	else
		return 0;
}

int32_t pattQueueWriteSize(void)
{
	int32_t size;

	if (pattSync.writePos > pattSync.readPos)
	{
		size = pattSync.readPos - pattSync.writePos + SYNC_QUEUE_LEN;
	}
	else if (pattSync.writePos < pattSync.readPos)
	{
		pattQueueClearing = true;

		/* Buffer is full, reset the read/write pos. This is actually really nasty since
		** read/write are two different threads, but because of timestamp validation it
		** shouldn't be that dangerous.
		** It will also create a small visual stutter while the buffer is getting filled,
		** though that is barely noticable on normal buffer sizes, and it takes a minute
		** or two at max BPM between each time (when queue size is default, 4095)
		*/
		pattSync.data[0].timestamp = 0;
		pattSync.readPos = 0;
		pattSync.writePos = 0;

		size = SYNC_QUEUE_LEN;

		pattQueueClearing = false;
	}
	else
	{
		size = SYNC_QUEUE_LEN;
	}

	return size;
}

bool pattQueuePush(pattSyncData_t t)
{
	if (!pattQueueWriteSize())
		return false;

	assert(pattSync.writePos <= SYNC_QUEUE_LEN);
	pattSync.data[pattSync.writePos] = t;
	pattSync.writePos = (pattSync.writePos + 1) & SYNC_QUEUE_LEN;

	return true;
}

bool pattQueuePop(void)
{
	if (!pattQueueReadSize())
		return false;

	pattSync.readPos = (pattSync.readPos + 1) & SYNC_QUEUE_LEN;
	assert(pattSync.readPos <= SYNC_QUEUE_LEN);

	return true;
}

pattSyncData_t *pattQueuePeek(void)
{
	if (!pattQueueReadSize())
		return NULL;

	assert(pattSync.readPos <= SYNC_QUEUE_LEN);
	return &pattSync.data[pattSync.readPos];
}

uint64_t getPattQueueTimestamp(void)
{
	if (!pattQueueReadSize())
		return 0;

	assert(pattSync.readPos <= SYNC_QUEUE_LEN);
	return pattSync.data[pattSync.readPos].timestamp;
}

int32_t chQueueReadSize(void)
{
	while (chQueueClearing);

	if (chSync.writePos > chSync.readPos)
		return chSync.writePos - chSync.readPos;
	else if (chSync.writePos < chSync.readPos)
		return chSync.writePos - chSync.readPos + SYNC_QUEUE_LEN + 1;
	else
		return 0;
}

int32_t chQueueWriteSize(void)
{
	int32_t size;

	if (chSync.writePos > chSync.readPos)
	{
		size = chSync.readPos - chSync.writePos + SYNC_QUEUE_LEN;
	}
	else if (chSync.writePos < chSync.readPos)
	{
		chQueueClearing = true;

		/* Buffer is full, reset the read/write pos. This is actually really nasty since
		** read/write are two different threads, but because of timestamp validation it
		** shouldn't be that dangerous.
		** It will also create a small visual stutter while the buffer is getting filled,
		** though that is barely noticable on normal buffer sizes, and it takes several
		** minutes between each time (when queue size is default, 16384)
		*/
		chSync.data[0].timestamp = 0;
		chSync.readPos = 0;
		chSync.writePos = 0;

		size = SYNC_QUEUE_LEN;

		chQueueClearing = false;
	}
	else
	{
		size = SYNC_QUEUE_LEN;
	}

	return size;
}

bool chQueuePush(chSyncData_t t)
{
	if (!chQueueWriteSize())
		return false;

	assert(chSync.writePos <= SYNC_QUEUE_LEN);
	chSync.data[chSync.writePos] = t;
	chSync.writePos = (chSync.writePos + 1) & SYNC_QUEUE_LEN;

	return true;
}

bool chQueuePop(void)
{
	if (!chQueueReadSize())
		return false;

	chSync.readPos = (chSync.readPos + 1) & SYNC_QUEUE_LEN;
	assert(chSync.readPos <= SYNC_QUEUE_LEN);

	return true;
}

chSyncData_t *chQueuePeek(void)
{
	if (!chQueueReadSize())
		return NULL;

	assert(chSync.readPos <= SYNC_QUEUE_LEN);
	return &chSync.data[chSync.readPos];
}

uint64_t getChQueueTimestamp(void)
{
	if (!chQueueReadSize())
		return 0;

	assert(chSync.readPos <= SYNC_QUEUE_LEN);
	return chSync.data[chSync.readPos].timestamp;
}

void lockAudio(void)
{
	if (audio.dev != 0)
		SDL_LockAudioDevice(audio.dev);

	audio.locked = true;
}

void unlockAudio(void)
{
	if (audio.dev != 0)
		SDL_UnlockAudioDevice(audio.dev);

	audio.locked = false;
}

static void resetSyncQueues(void)
{
	pattSync.data[0].timestamp = 0;
	pattSync.readPos = 0;
	pattSync.writePos = 0;
	
	chSync.data[0].timestamp = 0;
	chSync.writePos = 0;
	chSync.readPos = 0;
}

void lockMixerCallback(void) // lock audio + clear voices/scopes (for short operations)
{
	if (!audio.locked)
		lockAudio();

	audio.resetSyncTickTimeFlag = true;

	stopVoices(); // VERY important! prevents potential crashes by purging pointers

	// scopes, mixer and replayer are guaranteed to not be active at this point

	resetSyncQueues();
}

void unlockMixerCallback(void)
{
	stopVoices(); // VERY important! prevents potential crashes by purging pointers
	
	if (audio.locked)
		unlockAudio();
}

void pauseAudio(void) // lock audio + clear voices/scopes + render silence (for long operations)
{
	if (audioPaused)
	{
		stopVoices(); // VERY important! prevents potential crashes by purging pointers
		return;
	}

	if (audio.dev > 0)
		SDL_PauseAudioDevice(audio.dev, true);

	audio.resetSyncTickTimeFlag = true;

	stopVoices(); // VERY important! prevents potential crashes by purging pointers

	// scopes, mixer and replayer are guaranteed to not be active at this point

	resetSyncQueues();
	audioPaused = true;
}

void resumeAudio(void) // unlock audio
{
	if (!audioPaused)
		return;

	if (audio.dev > 0)
		SDL_PauseAudioDevice(audio.dev, false);

	audioPaused = false;
}

static void fillVisualsSyncBuffer(void)
{
	pattSyncData_t pattSyncData;
	chSyncData_t chSyncData;
	syncedChannel_t *c;
	stmTyp *s;

	if (audio.resetSyncTickTimeFlag)
	{
		audio.resetSyncTickTimeFlag = false;

		audio.tickTime64 = SDL_GetPerformanceCounter() + audio.audLatencyPerfValInt;
		audio.tickTime64Frac = audio.audLatencyPerfValFrac;
	}

	if (songPlaying)
	{
		// push pattern variables to sync queue
		pattSyncData.timer = song.curReplayerTimer;
		pattSyncData.patternPos = song.curReplayerPattPos;
		pattSyncData.pattern = song.curReplayerPattNr;
		pattSyncData.songPos = song.curReplayerSongPos;
		pattSyncData.speed = song.speed;
		pattSyncData.tempo = (uint8_t)song.tempo;
		pattSyncData.globalVol = (uint8_t)song.globVol;
		pattSyncData.timestamp = audio.tickTime64;
		pattQueuePush(pattSyncData);
	}

	// push channel variables to sync queue

	c = chSyncData.channels;
	s = stm;

	for (int32_t i = 0; i < song.antChn; i++, c++, s++)
	{
		c->finalPeriod = s->finalPeriod;
		c->fineTune = s->fineTune;
		c->relTonNr = s->relTonNr;
		c->instrNr = s->instrNr;
		c->sampleNr = s->sampleNr;
		c->envSustainActive = s->envSustainActive;
		c->status = s->tmpStatus;
		c->fFinalVol = s->fFinalVol;
		c->smpStartPos = s->smpStartPos;
	}

	chSyncData.timestamp = audio.tickTime64;
	chQueuePush(chSyncData);

	audio.tickTime64 += tickTimeLen;
	audio.tickTime64Frac += tickTimeLenFrac;
	if (audio.tickTime64Frac > UINT32_MAX)
	{
		audio.tickTime64Frac &= UINT32_MAX;
		audio.tickTime64++;
	}
}

static void SDLCALL audioCallback(void *userdata, Uint8 *stream, int len)
{
	if (editor.wavIsRendering)
		return;

	int32_t samplesLeft = len / pmpCountDiv;
	if (samplesLeft <= 0)
		return;

	while (samplesLeft > 0)
	{
		if (audio.dTickSampleCounter <= 0.0)
		{
			// new replayer tick

			replayerBusy = true;

			if (audio.volumeRampingFlag)
				resetRampVolumes();

			tickReplayer();
			updateVoices();
			fillVisualsSyncBuffer();

			audio.dTickSampleCounter += audio.dSamplesPerTick;

			replayerBusy = false;
		}

		const int32_t remainingTick = (int32_t)ceil(audio.dTickSampleCounter);

		int32_t samplesToMix = samplesLeft;
		if (samplesToMix > remainingTick)
			samplesToMix = remainingTick;

		mixAudio(stream, samplesToMix, pmpChannels);
		stream += samplesToMix * pmpCountDiv;

		samplesLeft -= samplesToMix;
		audio.dTickSampleCounter -= samplesToMix;
	}

	(void)userdata; // make compiler not complain
}

static bool setupAudioBuffers(void)
{
	const uint32_t sampleSize = sizeof (int32_t);

	audio.fMixBufferLUnaligned = (float *)MALLOC_PAD(MAX_WAV_RENDER_SAMPLES_PER_TICK * sampleSize, 256);
	audio.fMixBufferRUnaligned = (float *)MALLOC_PAD(MAX_WAV_RENDER_SAMPLES_PER_TICK * sampleSize, 256);

	if (audio.fMixBufferLUnaligned == NULL || audio.fMixBufferRUnaligned == NULL)
		return false;

	// make aligned main pointers
	audio.fMixBufferL = (float *)ALIGN_PTR(audio.fMixBufferLUnaligned, 256);
	audio.fMixBufferR = (float *)ALIGN_PTR(audio.fMixBufferRUnaligned, 256);

	return true;
}

static void freeAudioBuffers(void)
{
	if (audio.fMixBufferLUnaligned != NULL)
	{
		free(audio.fMixBufferLUnaligned);
		audio.fMixBufferLUnaligned = NULL;
	}

	if (audio.fMixBufferRUnaligned != NULL)
	{
		free(audio.fMixBufferRUnaligned);
		audio.fMixBufferRUnaligned = NULL;
	}

	audio.fMixBufferL = NULL;
	audio.fMixBufferR = NULL;
}

void updateSendAudSamplesRoutine(bool lockMixer)
{
	if (lockMixer)
		lockMixerCallback();

	if (config.specialFlags & BITDEPTH_16)
	{
		if (pmpChannels > 2)
			sendAudSamplesFunc = sendSamples16BitDitherMultiChan;
		else
			sendAudSamplesFunc = sendSamples16BitDitherStereo;
	}
	else
	{
		if (pmpChannels > 2)
			sendAudSamplesFunc = sendSamples32BitMultiChan;
		else
			sendAudSamplesFunc = sendSamples32BitStereo;
	}

	if (lockMixer)
		unlockMixerCallback();
}

static void calcAudioLatencyVars(int32_t audioBufferSize, int32_t audioFreq)
{
	double dInt, dFrac;

	if (audioFreq == 0)
		return;

	const double dAudioLatencySecs = audioBufferSize / (double)audioFreq;

	dFrac = modf(dAudioLatencySecs * editor.dPerfFreq, &dInt);

	// integer part
	audio.audLatencyPerfValInt = (int32_t)dInt;

	// fractional part (scaled to 0..2^32-1)
	dFrac *= UINT32_MAX+1.0;
	audio.audLatencyPerfValFrac = (uint32_t)dFrac;

	audio.dAudioLatencyMs = dAudioLatencySecs * 1000.0;
}

static void setLastWorkingAudioDevName(void)
{
	uint32_t stringLen;

	if (audio.lastWorkingAudioDeviceName != NULL)
	{
		free(audio.lastWorkingAudioDeviceName);
		audio.lastWorkingAudioDeviceName = NULL;
	}

	if (audio.currOutputDevice != NULL)
	{
		stringLen = (uint32_t)strlen(audio.currOutputDevice);

		audio.lastWorkingAudioDeviceName = (char *)malloc(stringLen + 2);
		if (audio.lastWorkingAudioDeviceName != NULL)
		{
			if (stringLen > 0)
				strcpy(audio.lastWorkingAudioDeviceName, audio.currOutputDevice);
			audio.lastWorkingAudioDeviceName[stringLen + 1] = '\0'; // UTF-8 needs double null termination
		}
	}
}

bool setupAudio(bool showErrorMsg)
{
	int8_t newBitDepth;
	uint16_t configAudioBufSize;
	SDL_AudioSpec want, have;

	closeAudio();

	if (config.audioFreq < MIN_AUDIO_FREQ || config.audioFreq > MAX_AUDIO_FREQ)
		config.audioFreq = 48000; // set default rate

	// get audio buffer size from config special flags

	configAudioBufSize = 1024;
	if (config.specialFlags & BUFFSIZE_512)
		configAudioBufSize = 512;
	else if (config.specialFlags & BUFFSIZE_2048)
		configAudioBufSize = 2048;

	audio.wantFreq = config.audioFreq;
	audio.wantSamples = configAudioBufSize;
	audio.wantChannels = 2;

	// set up audio device
	memset(&want, 0, sizeof (want));

	// these three may change after opening a device, but our mixer is dealing with it
	want.freq = config.audioFreq;
	want.format = (config.specialFlags & BITDEPTH_32) ? AUDIO_F32 : AUDIO_S16;
	want.channels = 2;
	// -------------------------------------------------------------------------------
	want.callback = audioCallback;
	want.samples  = configAudioBufSize;

	audio.dev = SDL_OpenAudioDevice(audio.currOutputDevice, 0, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE); // prevent SDL2 from resampling
	if (audio.dev == 0)
	{
		if (showErrorMsg)
			showErrorMsgBox("Couldn't open audio device:\n\"%s\"\n\nDo you have any audio device enabled and plugged in?", SDL_GetError());

		return false;
	}

	// test if the received audio format is compatible
	if (have.format != AUDIO_S16 && have.format != AUDIO_F32)
	{
		if (showErrorMsg)
			showErrorMsgBox("Couldn't open audio device:\nThe program doesn't support an SDL_AudioFormat of '%d' (not 16-bit or 32-bit float).",
				(uint32_t)have.format);

		closeAudio();
		return false;
	}

	// test if the received audio rate is compatible

	if (have.freq != 44100 && have.freq != 48000 && have.freq != 96000 && have.freq != 192000)
	{
		if (showErrorMsg)
			showErrorMsgBox("Couldn't open audio device:\nThe program doesn't support an audio output rate of %dHz. Sorry!", have.freq);

		closeAudio();
		return false;
	}

	if (!setupAudioBuffers())
	{
		if (showErrorMsg)
			showErrorMsgBox("Not enough memory!");

		closeAudio();
		return false;
	}

	// set new bit depth flag

	newBitDepth = 16;
	config.specialFlags &= ~BITDEPTH_32;
	config.specialFlags |=  BITDEPTH_16;

	if (have.format == AUDIO_F32)
	{
		newBitDepth = 24;
		config.specialFlags &= ~BITDEPTH_16;
		config.specialFlags |=  BITDEPTH_32;
	}

	audio.haveFreq = have.freq;
	audio.haveSamples = have.samples;
	audio.haveChannels = have.channels;

	// set a few variables

	config.audioFreq = have.freq;
	audio.freq = have.freq;
	smpBuffSize = have.samples;

	calcAudioLatencyVars(have.samples, have.freq);

	pmpChannels = have.channels;
	pmpCountDiv = pmpChannels * ((newBitDepth == 16) ? sizeof (int16_t) : sizeof (float));

	// make a copy of the new known working audio settings

	audio.lastWorkingAudioFreq = config.audioFreq;
	audio.lastWorkingAudioBits = config.specialFlags & (BITDEPTH_16 + BITDEPTH_32 + BUFFSIZE_512 + BUFFSIZE_1024 + BUFFSIZE_2048);
	setLastWorkingAudioDevName();

	// update config audio radio buttons if we're on that screen at the moment
	if (ui.configScreenShown && editor.currConfigScreen == CONFIG_SCREEN_IO_DEVICES)
		showConfigScreen();

	updateWavRendererSettings();
	setAudioAmp(config.boostLevel, config.masterVol, (config.specialFlags & BITDEPTH_32) ? true : false);

	// don't call stopVoices() in this routine
	for (int32_t i = 0; i < MAX_VOICES; i++)
		stopVoice(i);

	stopAllScopes();

	audio.dTickSampleCounter = 0.0; // zero tick sample counter so that it will instantly initiate a tick

	calcReplayRate(audio.freq);

	if (song.speed == 0)
		song.speed = 125;

	P_SetSpeed(song.speed); // this is important

	updateSendAudSamplesRoutine(false);
	audio.resetSyncTickTimeFlag = true;

	setWavRenderFrequency(audio.freq);
	setWavRenderBitDepth((config.specialFlags & BITDEPTH_32) ? 32 : 16);

	return true;
}

void closeAudio(void)
{
	if (audio.dev > 0)
	{
		SDL_PauseAudioDevice(audio.dev, true);
		SDL_CloseAudioDevice(audio.dev);
		audio.dev = 0;
	}

	freeAudioBuffers();
}
