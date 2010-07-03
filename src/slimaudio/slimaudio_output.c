/*
 *   SlimProtoLib Copyright (c) 2004,2006 Richard Titmuss
 *
 *   This file is part of SlimProtoLib.
 *
 *   SlimProtoLib is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   SlimProtoLib is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with SlimProtoLib; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

#include <portaudio.h>
#ifdef PA_WASAPI
#include <pa_win_wasapi.h>
#endif
#ifndef PORTAUDIO_DEV
#include <portmixer.h>
#endif

#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"

#ifdef SLIMPROTO_DEBUG
  bool slimaudio_output_debug;
  bool slimaudio_output_debug_v;
  #define DEBUGF(...) if (slimaudio_output_debug) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...) if (slimaudio_output_debug_v) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
  #define VDEBUGF(...)
#endif

static void *output_thread(void *ptr);

#ifdef PORTAUDIO_DEV
static int pa_callback(  const void *inputBuffer, void *outputBuffer,
	unsigned long framesPerBuffer,
	const PaStreamCallbackTimeInfo * callbackTime,
	PaStreamCallbackFlags statusFlags, void *userData );
#else
static int pa_callback(  void *inputBuffer, void *outputBuffer,
	unsigned long framesPerBuffer,
	PaTimestamp outTime, void *userData );
#endif


static int audg_callback(slimproto_t *p, const unsigned char *buf, int buf_len, void *user_data);

/* Find audio devices which support stereo */
PaDeviceIndex GetAudioDevices(PaDeviceIndex default_device, bool output_change, bool show_list)
{
	int i;
	int err;
	bool bValidDev = false;
	const PaDeviceInfo *pdi;
	PaDeviceIndex DefaultDevice;
	PaDeviceIndex DeviceCount;

	err = Pa_Initialize();
	if (err != paNoError) {
		printf("PortAudio error4: %s Could not open any audio devices.\n", Pa_GetErrorText(err) );
		exit(-1);
        }

#ifdef PORTAUDIO_DEV
	DeviceCount = Pa_GetDeviceCount();
#else
	DeviceCount = Pa_CountDevices();
#endif

	if ( DeviceCount < 0 )
	{
		printf("PortAudio error5: %s\n", Pa_GetErrorText(DeviceCount) );
		exit(-1);
	}

	if ( output_change )
		DefaultDevice = default_device;
	else
		DefaultDevice = PA_DEFAULT_DEVICE;

	if ( DefaultDevice >= DeviceCount || ((DefaultDevice == PA_DEFAULT_DEVICE) && !output_change) )
	{
#ifdef PORTAUDIO_DEV
		DefaultDevice = Pa_GetDefaultOutputDevice();
#else
		DefaultDevice = Pa_GetDefaultOutputDeviceID();
#endif
	}

	if ( DefaultDevice == paNoDevice )
	{
		printf("PortAudio error7: No output devices found.\n" );
		exit(-1);
	}
	
	if ( show_list )
		printf("Output devices:\n");

	for ( i = 0; i < DeviceCount; i++ )
	{
		pdi = Pa_GetDeviceInfo( i );
                if ( pdi->name == NULL )
       	        {
               	        printf("PortAudio error6: GetDeviceInfo failed.\n" );
                       	exit(-1);
                }

		if ( pdi->maxOutputChannels >= 2 )
		{
               	        if ( i == DefaultDevice )
			{
				bValidDev = true;
				if ( show_list )
	                       	        printf("*%2d: %s\n", i, pdi->name);
			}
                        else
			{
				if ( show_list )
       	                	        printf(" %2d: %s\n", i, pdi->name);
			}
		}
	}

	Pa_Terminate();

	if ( !bValidDev )
		DefaultDevice = paNoDevice;

	return (DefaultDevice) ;
}

int slimaudio_output_init(slimaudio_t *audio, PaDeviceIndex output_device_id, bool output_change)
{
	audio->output_device_id = GetAudioDevices(output_device_id, output_change, false);

	if ( audio->output_device_id == paNoDevice )
	{
		printf("PortAudio error7: No output devices found.\n" );	
		exit(-1);
	}

#ifndef PORTAUDIO_DEV
	audio->px_mixer = NULL;
#endif
	audio->volume_control = VOLUME_DRIVER;
	audio->volume = 1.0;
	audio->vol_adjust = 1.0;
	audio->prev_volume = -1.0;	// Signals prev = volume.
	audio->output_predelay_msec = 0;
	audio->output_predelay_frames = 0;
	audio->output_predelay_amplitude = 0;
	audio->keepalive_interval = -1;
	audio->decode_num_tracks_started = 0L;	
	audio->stream_samples = 0UL;	
	audio->replay_gain = -1.0;  	// signals first start
	audio->start_replay_gain = 1.0;  // none to start

	slimproto_add_command_callback(audio->proto, "audg", &audg_callback, audio);
	
	pthread_mutex_init(&(audio->output_mutex), NULL);
	pthread_cond_init(&(audio->output_cond), NULL);

	return 0;
}

void slimaudio_output_destroy(slimaudio_t *audio) {

	pthread_mutex_destroy(&(audio->output_mutex));
	pthread_cond_destroy(&(audio->output_cond));

	// FIXME remove slimproto callback
}

int slimaudio_output_open(slimaudio_t *audio) {
	/* 
	 * We take the mutex here and release it in the output thread to prevent
	 * other threads from using the audio output information while it is
	 * being initialized.
	 */
#ifndef BSD_THREAD_LOCKING
	pthread_mutex_lock(&audio->output_mutex);
#endif

	audio->output_state = STOPPED;

	if (pthread_create( &audio->output_thread, NULL, output_thread, (void*) audio) != 0) {
		fprintf(stderr, "Error creating output thread\n");
		return -1;	
	}	
	return 0;
}

int slimaudio_output_close(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->output_mutex);

	audio->output_state = QUIT;

	pthread_mutex_unlock(&audio->output_mutex);

	pthread_cond_broadcast(&audio->output_cond);

	pthread_join(audio->output_thread, NULL);

	return 0;
}

// Wrapper to call slimaudio_stat from output_thread.  Because the
// output thread keeps the output mutex locked, this wrapper unlocks
// the mutex for the duration of the slimaudio_stat call.  The reason
// is that slimaudio_stat may find that the socket has been closed and
// will then try to stop the audio output by calling
// slimaudio_output_disconnect and this requires the mutex to be unlocked.
static void output_thread_stat(slimaudio_t* audio, char* code) {
	pthread_mutex_unlock(&audio->output_mutex);
	slimaudio_stat(audio, code, (u32_t) 0);
	pthread_mutex_lock(&audio->output_mutex);
}

static void *output_thread(void *ptr) {
	int err;
	struct timeval  now;
	struct timespec timeout;
	
	slimaudio_t *audio = (slimaudio_t *) ptr;
	audio->output_STMd = false;
	audio->output_STMs = false;
	audio->output_STMu = false;
	audio->output_EoS  = false;

        err = Pa_Initialize();
        if (err != paNoError) {
                printf("PortAudio error4: %s Could not open any audio devices.\n", Pa_GetErrorText(err) );
                exit(-1);
        }

        DEBUGF("output_thread: PortAudio initialized\n");
	
#ifndef PORTAUDIO_DEV
	err = Pa_OpenStream(	&audio->pa_stream,	// stream
				paNoDevice,		// input device
				0,			// input channels
				0,			// input sample format
				NULL,			// input driver info
				audio->output_device_id,// output device				
				2,			// output channels
				paInt16,		// output sample format
				NULL,			// output driver info
				44100.0,		// sample rate
				256,			// frames per buffer
				0,			// number of buffers
				0,			// stream flags
				pa_callback,		// callback
				audio);			// user data
#else
	PaStreamParameters outputParameters;
	const PaDeviceInfo * paDeviceInfo;

#ifdef PA_WASAPI
	PaWasapiStreamInfo streamInfo;
#endif

	paDeviceInfo = Pa_GetDeviceInfo(audio->output_device_id);
	/* Device is not stereo or better, abort */
	if (paDeviceInfo->maxOutputChannels < 2)
	{
		printf("output_thread: PortAudio device does not support 44.1KHz, 16-bit, stereo audio.\n");
		printf("output_thread: Use -L for a list of supported audio devices, then use -o followed\n");
		printf("output_thread: by the device number listed before the colon.  See -h for details.\n");
		exit(-2);
	}

	outputParameters.device = audio->output_device_id;
	outputParameters.channelCount = 2;
	outputParameters.sampleFormat = paInt16;
	outputParameters.suggestedLatency = paDeviceInfo->defaultHighOutputLatency;
#ifdef PA_WASAPI
	streamInfo.size = sizeof(PaWasapiStreamInfo);
	streamInfo.hostApiType = paWASAPI;
	streamInfo.version = 1;
	streamInfo.flags = paWinWasapiExclusive;
	outputParameters.hostApiSpecificStreamInfo = &streamInfo;
#else
	outputParameters.hostApiSpecificStreamInfo = NULL;
#endif

	DEBUGF("paDeviceInfo->deviceid %d\n", outputParameters.device);
	DEBUGF("paDeviceInfo->maxOutputChannels %i\n", paDeviceInfo->maxOutputChannels);
	DEBUGF("paDeviceInfo->defaultHighOutputLatency %f\n", (float) paDeviceInfo->defaultHighOutputLatency);
	DEBUGF("paDeviceInfo->defaultLowhOutputLatency %f\n", (float) paDeviceInfo->defaultLowOutputLatency);
	DEBUGF("paDeviceInfo->defaultSampleRate %f\n", paDeviceInfo->defaultSampleRate);

	err = Pa_OpenStream (	&audio->pa_stream,				// stream
				NULL,						// inputParameters
				&outputParameters,				// outputParameters
				44100.0,					// sample rate
				0,						// framesPerBuffer
				paPrimeOutputBuffersUsingStreamCallback,	// streamFlags
				pa_callback,					// streamCallback
				audio);						// userData
#endif

#ifdef BSD_THREAD_LOCKING
	pthread_mutex_lock(&audio->output_mutex);
#endif

	if (err != paNoError) {
		printf("output_thread: PortAudio error1: %s\n", Pa_GetErrorText(err) );	
		exit(-1);
	}

#ifndef PORTAUDIO_DEV
	int num_mixers = Px_GetNumMixers(audio->pa_stream);
	while (--num_mixers >= 0) {
		DEBUGF("Mixer: %s\n", Px_GetMixerName(audio->pa_stream, num_mixers));
	}
	
	if (audio->volume_control == VOLUME_DRIVER) {
		DEBUGF("Opening mixer.\n" );
		audio->px_mixer = Px_OpenMixer(audio->pa_stream, 0);
	}
	else {
		DEBUGF("Mixer explicitly disabled.\n" );
	}
	DEBUGF("Px_mixer = %p\n", audio->px_mixer);

	if (audio->px_mixer != NULL) {
		DEBUGF("PCM volume supported: %d.\n", 
		       Px_SupportsPCMOutputVolume(audio->px_mixer));

		const int nbVolumes = Px_GetNumOutputVolumes(audio->px_mixer);
		DEBUGF("Nb volumes supported: %d.\n", nbVolumes);
		int volumeIdx;
		for (volumeIdx=0; volumeIdx<nbVolumes; ++volumeIdx) {
			DEBUGF("Volume %d: %s\n", volumeIdx, 
				Px_GetOutputVolumeName(audio->px_mixer, volumeIdx));
		}
	}
#endif

	while (audio->output_state != QUIT) {

		switch (audio->output_state) {
			case STOPPED:
				audio->decode_num_tracks_started = 0L;
				audio->stream_samples = 0UL;
				audio->pa_streamtime_offset = audio->stream_samples;

				DEBUGF("output_thread STOPPED: %llu\n",audio->pa_streamtime_offset);

				slimaudio_buffer_set_readopt(audio->output_buffer, BUFFER_BLOCKING);

			case PAUSED:
				// We report ourselves to the server every few seconds
				// as a keep-alive.  This is required for Squeezebox Server
				// v6.5.x although technically, "stat" is not a valid event 
				// code for the STAT Client->Server message.  This was 
				// lifted by observing how a Squeezebox3 reports itself to 
				// the server using Squeezebox Server's d_slimproto and 
				// d_slimproto_v tracing services.  Note that Squeezebox3
			  	// seems to report every 1 second or so, but the server only
			  	// drops the connection after 15-20 seconds of inactivity.

				DEBUGF("output_thread PAUSED: %llu\n",audio->pa_streamtime_offset);

			  	if (audio->keepalive_interval <= 0) {
					pthread_cond_wait(&audio->output_cond, &audio->output_mutex);
				}
				else {
					gettimeofday(&now, NULL);
					timeout.tv_sec = now.tv_sec + audio->keepalive_interval;
					timeout.tv_nsec = now.tv_usec * 1000;				
					err = pthread_cond_timedwait(&audio->output_cond,
								     &audio->output_mutex, &timeout);
					if (err == ETIMEDOUT) {
						DEBUGF("Sending keepalive. Interval=%ds.\n", audio->keepalive_interval);
						output_thread_stat(audio, "stat");
					}
				}

				break;

			case PLAY:
/*
 * TODO I am not sure if it's best to start the stream in the play or buffering
 * state ... which has less latency for sync? If the stream is started in 
 * BUFFERING then pause/unpause breaks.
 */
		   
				audio->output_predelay_frames = audio->output_predelay_msec * 44.100;
				err = Pa_StartStream(audio->pa_stream);
				if (err != paNoError) {
					printf("output_thread: PortAudio error2: %s\n", Pa_GetErrorText(err) );	
					exit(-1);
				}

				DEBUGF("output_thread PLAY: %llu\n",audio->pa_streamtime_offset);

				slimaudio_buffer_set_readopt(audio->output_buffer, BUFFER_NONBLOCKING);

				audio->output_state = PLAYING;

				pthread_cond_broadcast(&audio->output_cond);

				break;

			case BUFFERING:
				DEBUGF("output_thread BUFFERING: %llu\n",audio->pa_streamtime_offset);

			case PLAYING:			
				gettimeofday(&now, NULL);
				timeout.tv_sec = now.tv_sec + 1;
				timeout.tv_nsec = now.tv_usec * 1000;
				err = pthread_cond_timedwait(&audio->output_cond, &audio->output_mutex, &timeout);

				if (err == ETIMEDOUT)
				{
					DEBUGF("output_thread ETIMEDOUT-PLAYING: %llu\n",audio->pa_streamtime_offset);
					output_thread_stat(audio, "STMt");
				}

				/* Track started */				
				if (audio->output_STMs)
				{
					audio->output_STMs = false;

					DEBUGF("output_thread STMs-PLAYING: %llu\n",audio->pa_streamtime_offset);
					output_thread_stat(audio, "STMs");
				}

				/* Data underrun
				** On buffer underrun causes the server to switch to the next track.
				*/
				if (audio->output_STMu)
				{
					audio->output_STMu = false;

					audio->output_state = STOP;

					DEBUGF("output_thread STMu-PLAYING: %llu\n",audio->pa_streamtime_offset);
					output_thread_stat(audio, "STMu");

					pthread_cond_broadcast(&audio->output_cond);
				}

				if (audio->output_EoS)
				{
					audio->output_EoS = false;

					DEBUGF("output_thread EoS-PLAYING:%llu\n",audio->pa_streamtime_offset);
				}

				break;
		
			case STOP:
#ifndef PORTAUDIO_DEV
				err = Pa_AbortStream(audio->pa_stream);
				if (err != paNoError) {
					printf("output_thread: PortAudio error3: %s\n", Pa_GetErrorText(err) );	
					exit(-1);
				}
#else
				if ( (err = Pa_IsStreamActive(audio->pa_stream)) > 0) {
					err = Pa_AbortStream(audio->pa_stream);
					if (err != paNoError) {
						printf("output_thread[STOP]: PortAudio error3: %s\n",
									Pa_GetErrorText(err) );
						exit(-1);
					}
				} else if ( err != paNoError) {
					printf("output_thread[STOP ISACTIVE]: PortAudio error3: %s\n",
									Pa_GetErrorText(err) );
					exit(-1);
				}
#endif
				audio->output_state = STOPPED;

				DEBUGF("output_thread STOP: %llu\n",audio->pa_streamtime_offset);
				pthread_cond_broadcast(&audio->output_cond);

				break;
				
			case PAUSE:
#ifndef PORTAUDIO_DEV
				err = Pa_StopStream(audio->pa_stream);
				if (err != paNoError) {
					printf("output_thread: PortAudio error3: %s\n", Pa_GetErrorText(err) );	
					exit(-1);
				}
#else
				if ( (err = Pa_IsStreamActive(audio->pa_stream)) > 0) {
					err = Pa_StopStream(audio->pa_stream);
					if (err != paNoError) {
						printf("output_thread[PAUSE]: PortAudio error3: %s\n",
								Pa_GetErrorText(err) );
						exit(-1);
					}
				} else if ( err != paNoError) {
					printf("output_thread[PAUSE ISACTIVE]: PortAudio error3: %s\n",
							Pa_GetErrorText(err) );
					exit(-1);
				}
#endif
				audio->output_state = PAUSED;

				DEBUGF("output_thread PAUSE: %llu\n",audio->pa_streamtime_offset);
				pthread_cond_broadcast(&audio->output_cond);

				break;

			case QUIT:
				DEBUGF("output_thread QUIT: %llu\n",audio->pa_streamtime_offset);
				break;

		}		
	}
	pthread_mutex_unlock(&audio->output_mutex);

#ifndef PORTAUDIO_DEV	
	if (audio->px_mixer != NULL) {
		Px_CloseMixer(audio->px_mixer);
		audio->px_mixer = NULL;
	}
#endif

	err = Pa_CloseStream(audio->pa_stream);

	if (err != paNoError) {
		printf("output_thread[exit]: PortAudio error3: %s\n", Pa_GetErrorText(err) );
		exit(-1);
	}

	audio->pa_stream = NULL;
	Pa_Terminate();

	DEBUGF("output_thread: PortAudio terminated\n");

	return 0;
}


/*
 * Callback for the 'audg' command
 */
static int audg_callback(slimproto_t *proto, const unsigned char *buf, int buf_len, void *user_data) {
	slimproto_msg_t msg;
	
	slimaudio_t* const audio = (slimaudio_t *) user_data;
	slimproto_parse_command(buf, buf_len, &msg);

	audio->vol_adjust = (float) (msg.audg.left_gain) / 65536.0;

	if ( audio->vol_adjust > 1.0 )
		audio->vol_adjust = 1.0;

	audio->volume = audio->vol_adjust * audio->replay_gain;

	if ( ( audio->volume == -1.0 ) || ( audio->volume > 1.0 ) )
		audio->volume = 1.0;

	DEBUGF("audg cmd: left_gain:%u right_gain:%u volume:%f old_left_gain:%u old_right_gain:%u",
			msg.audg.left_gain, msg.audg.right_gain, audio->volume,
			msg.audg.old_left_gain, msg.audg.old_right_gain);
	DEBUGF(" vol_adjust:%f replay_gain:%f start_replay_gain:%f",
			audio->vol_adjust, audio->replay_gain, audio->start_replay_gain);
	VDEBUGF(" preamp:%hhu digital_volume_control:%hhu", msg.audg.preamp, msg.audg.digital_volume_control);
	DEBUGF("\n");

#ifndef PORTAUDIO_DEV
	if (audio->px_mixer != NULL) {
		Px_SetPCMOutputVolume(audio->px_mixer, (PxVolume)audio->volume);
		DEBUGF("pcm volume %f\n", Px_GetPCMOutputVolume(audio->px_mixer));	
	}
#endif		
	return 0;
}


void slimaudio_output_connect(slimaudio_t *audio, slimproto_msg_t *msg) {
	pthread_mutex_lock(&audio->output_mutex);

	if (audio->output_state == PLAYING) {
		pthread_mutex_unlock(&audio->output_mutex);
		return;
	}
	
	DEBUGF("slimaudio_output_connect: state=%i\n", audio->output_state);

	audio->output_state = BUFFERING;

	pthread_mutex_unlock(&audio->output_mutex);	

	pthread_cond_broadcast(&audio->output_cond);
}


int slimaudio_output_disconnect(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->output_mutex);

	DEBUGF("slimaudio_output_disconnect: state=%i\n", audio->output_state);

	if (audio->output_state == STOPPED || audio->output_state == QUIT) {
		pthread_mutex_unlock(&audio->output_mutex);	
		return -1;	
	}	

	audio->output_state = STOP;

	pthread_cond_broadcast(&audio->output_cond);

	while (audio->output_state == STOP) {				
		pthread_cond_wait(&audio->output_cond, &audio->output_mutex);
	}

	pthread_mutex_unlock(&audio->output_mutex);

	return 0;
}


void slimaudio_output_pause(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->output_mutex);
	
	DEBUGF("slimaudio_output_pause: state=%i\n", audio->output_state);

	audio->output_state = PAUSE;

	pthread_cond_broadcast(&audio->output_cond);

	while (audio->output_state == PAUSE) {				
		pthread_cond_wait(&audio->output_cond, &audio->output_mutex);
	}

	pthread_mutex_unlock(&audio->output_mutex);
}


void slimaudio_output_unpause(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->output_mutex);

	if (audio->output_state == PLAYING) {
		pthread_mutex_unlock(&audio->output_mutex);	
		return;	
	}

	DEBUGF("slimaudio_output_unpause: state=%i\n", audio->output_state);

	audio->output_state = PLAY;

	pthread_cond_broadcast(&audio->output_cond);

	while (audio->output_state == PLAY) {
		pthread_cond_wait(&audio->output_cond, &audio->output_mutex);
	}

	pthread_mutex_unlock(&audio->output_mutex);
}

// Applies software volume to buffers being sent to the output device.  It is
// important we apply volume changes here rather than, for example, in the 
// decoder, to avoid latency between volume modification and audible change.
//
// FIXME:
// There are a couple of shortcuts taken in the name of simplicity here.
// 1. We assume 16-bits stereo.
// 2. We perform this in a separate loop, which will consume more 
//    frontside bus bandwidth than if we were performing this task as
//    part of, for example, the buffer copy.  But this would imply
//    changing audio buffers so they are format-aware.
static void apply_software_volume(slimaudio_t* const audio, void* outputBuffer,
				  int nbFrames) {
	if (audio->prev_volume == -1.0) {
		// A value of -1 indicates it's the first time we pass here.
		// Copy volume into prev_volume to start immediatly at the right
		// volume.
		audio->prev_volume = audio->volume;
	}

	if ( (audio->volume == 1.0) &&
		(audio->prev_volume == 1.0) &&
		(audio->replay_gain == 1.0) ) {
		VDEBUGF("volume: replay_gain: %f start_replay_gain: %f not applied\n",
			audio->replay_gain, audio->start_replay_gain);
		return;
	}

	// Deglitch volume changes by going from the old to the new value over
	// the course of the whole buffer being sent to the output device.  We
	// read 'audio->volume' exactly once (assuming this operation is atomic)
	// to make sure that volume changes performed while we are here will not
	// cause any glitches.
	const float newVolume = audio->volume;
	float curVolume = audio->prev_volume;
	audio->prev_volume = newVolume;
	const float volumeIncr = (newVolume - curVolume) / (float)nbFrames;
	short* const samples = (short*)outputBuffer;
	const int nbSamples = nbFrames * 2;
	int i;
	for (i = 0; i < nbSamples; i += 2, curVolume += volumeIncr) {
		samples[i]     = ((float)samples[i])     * curVolume;
		samples[i + 1] = ((float)samples[i + 1]) * curVolume;
	}

	VDEBUGF("volume: %f applied\n",newVolume);
}

// Writes pre-delay sample-frames into the output buffer passed in.
// Pre-delay will most often be silence but can also be a tone at
// SamplingFrequency/2 in case the output device needs some non-silent
// samples to turn on.
static int produce_predelay_frames(slimaudio_t* audio, void* outputBuffer, unsigned int nbBytes)
{
	// FIXME: Asuming 2 channels, 16 bit samples (i.e. 2 bytes)
	const int frameSize = 2 * 2;
	unsigned int predelayFrames = audio->output_predelay_frames;
	const unsigned int maxFrames = nbBytes / frameSize;
	
	if (predelayFrames > maxFrames) {
	  	predelayFrames = maxFrames;
	}
	
	DEBUGF("slimaudio_output: producing %i predelay frames\n",
	       predelayFrames);
	
	const unsigned int predelayBytes = predelayFrames * frameSize;
	if (audio->output_predelay_amplitude == 0) {
		memset((char*)outputBuffer, 0, predelayBytes);
	}
	else {
		// Producing a low-volume high-frequency tone that will wake up
		// stubborn DACs.  Not very dog-friendly, but some DACs will 
		// only turn on (too late) at the presence of sound of a certain
		// level.
		// FIXME: This code assumes stereo signed 16-bit sample frames.
		short* const samples = (short*)outputBuffer;
		short val = audio->output_predelay_amplitude <= SHRT_MAX ? 
			audio->output_predelay_amplitude : SHRT_MAX;
		if ( val & 1 ) {
			// Make sure we alternate positive and negative values
			// even if the pre-delay is broken-down over multiple
			// calls.
			val = -val;
		}
		int i;
		for ( i = 0; i < predelayFrames; ++i, val = -val ) {
			samples[ 2 * i ] = samples[ 2 * i + 1] = val;
		}
	}
	
	audio->output_predelay_frames -= predelayFrames;
	return predelayBytes;
}

#ifndef PORTAUDIO_DEV
static int pa_callback(void *inputBuffer, void *outputBuffer,
			unsigned long framesPerBuffer,
			PaTimestamp outTime, void *userData)
#else
static int pa_callback(  const void *inputBuffer, void *outputBuffer,
		unsigned long framesPerBuffer,
		const PaStreamCallbackTimeInfo * callbackTime,
		PaStreamCallbackFlags statusFlags, void *userData )
#endif
{
	slimaudio_t * const audio = (slimaudio_t *) userData;

	// FIXME: Asuming 2 channels, 16 bit samples (i.e. 2 bytes)
	const int frameSize = 2 * 2;
	const int len = framesPerBuffer * frameSize; 
	
	int off = 0;

	while ( (audio->output_state == PLAYING) && ((len - off) > 0) )
	{
		if (audio->output_predelay_frames > 0) {
			off += produce_predelay_frames(audio, (char*)outputBuffer + off, len - off);
			continue;
		}

		int data_len = len - off;

		const slimaudio_buffer_status ok = 
			slimaudio_buffer_read( audio->output_buffer, (char *) outputBuffer+off, &data_len);

		if (ok == SLIMAUDIO_BUFFER_STREAM_END) {
			/* stream closed */
			if (slimaudio_buffer_available(audio->output_buffer) == 0) {
				pthread_mutex_lock(&audio->output_mutex);

				// Send buffer underrun to Squeezebox Server. During
				// normal play this indicates the end of the
				// playlist. During sync this starts the next 
				// track.
				audio->output_STMu = true;

				DEBUGF("pa_callback: STREAM_END:output_STMu:%i\n",audio->output_STMu);

				pthread_mutex_unlock(&audio->output_mutex);
				pthread_cond_broadcast(&audio->output_cond);
			}
		}
		else if (ok == SLIMAUDIO_BUFFER_STREAM_START) {
			pthread_mutex_lock(&audio->output_mutex);

			/* Send track start to Squeezebox Server. During normal play
			** this advances the playlist.
			*/
			audio->output_STMs = true;

			audio->decode_num_tracks_started++;

			audio->replay_gain = audio->start_replay_gain;
			audio->volume = audio->vol_adjust * audio->replay_gain;

			if ( ( audio->volume == -1.0 ) || ( audio->volume > 1.0 ) )
				audio->volume = 1.0;

			audio->pa_streamtime_offset = audio->stream_samples;

			DEBUGF("pa_callback: STREAM_START:output_STMs:%i\n",audio->output_STMs);

			pthread_mutex_unlock(&audio->output_mutex);
			pthread_cond_broadcast(&audio->output_cond);

		}
		else if (ok == SLIMAUDIO_BUFFER_STREAM_CONTINUE) {
			if (slimaudio_buffer_available(audio->output_buffer) == 0) {
				pthread_mutex_lock(&audio->output_mutex);

				/*
				** Decoder blocked in buffer_read.
				*/
				audio->output_EoS = true;

				DEBUGF("pa_callback: STREAM_CONTINUE:output_EoS:%i\n",audio->output_EoS);

				pthread_mutex_unlock(&audio->output_mutex);
				pthread_cond_broadcast(&audio->output_cond);
			}
		}

		audio->stream_samples += framesPerBuffer;

		off += data_len;

		/* if we have underrun fill remaining buffer with silence */
		if (data_len == 0) {
			DEBUGF("pa_callback: DATA_LEN0:off=len\n");

			/* Clear any remaining buffer so we don't hear it played out */
			memset((char *)outputBuffer+off, 0, len-off);

			off = len;
		}

	}

	const int uninitSize = len - off;
	if (uninitSize > 0) {
	 	DEBUGF("slimaudio_output: pa_callback uninitialized bytes: %i\n", uninitSize);
		memset((char *)outputBuffer+off, 0, uninitSize);
	}

	if (audio->volume_control == VOLUME_SOFTWARE) {
		apply_software_volume(audio, outputBuffer, framesPerBuffer);
	}

	VDEBUGF("pa_callback complete framesPerBuffer=%lu\n", framesPerBuffer);

	return 0;
}
