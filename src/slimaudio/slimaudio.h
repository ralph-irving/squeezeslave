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


#ifndef _SLIMAUDIO_H_
#define _SLIMAUDIO_H_

#include <stdbool.h>
#include <pthread.h>
#include <portaudio.h>
#ifndef PORTAUDIO_DEV
#include <portmixer.h>
#endif

#include <mad.h>
#include <FLAC/stream_decoder.h>

#define OV_EXCLUDE_STATIC_CALLBACKS

#ifdef TREMOR_DECODER
#include <vorbis/ivorbisfile.h>
#else
#include <vorbis/vorbisfile.h>
#endif /* TREMOR_DECODER */

#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio_buffer.h"

#define DECODER_BUFFER_SIZE	(2048*1024)
#define OUTPUT_BUFFER_SIZE	(8*2*44100*4)

#ifdef ZONES
#define MAX_ZONES		(4)
#endif

#define PA_DEFAULT_DEVICE	(-1)
#ifndef PORTAUDIO_DEV
typedef int	PaDeviceIndex;
extern unsigned long pa_framesPerBuffer;
extern unsigned long pa_numberOfBuffers;
#endif /* ! PORTAUDIO_DEV */

#define AUDIO_CHUNK_SIZE 8192

#ifdef SLIMPROTO_DEBUG
  extern bool slimaudio_debug;
  extern bool slimaudio_buffer_debug;
  extern bool slimaudio_buffer_debug_v;
  extern bool slimaudio_decoder_debug;
  extern bool slimaudio_decoder_debug_r;
  extern bool slimaudio_decoder_debug_v;
  extern bool slimaudio_http_debug;
  extern bool slimaudio_http_debug_v;
  extern bool slimaudio_output_debug;
  extern bool slimaudio_output_debug_v;
#endif

#ifdef RENICE
extern bool renice;
bool renice_thread ( int );
#endif

typedef enum { STREAM_QUIT=0, STREAM_STOP, STREAM_STOPPED, STREAM_PLAYING } slimaudio_stream_state_t;

typedef enum { QUIT=0, PLAY, BUFFERING, PLAYING, PAUSE, PAUSED, STOP, STOPPED } slimaudio_output_state_t;

typedef enum { VOLUME_NONE, VOLUME_SOFTWARE, VOLUME_DRIVER } slimaudio_volume_t;

typedef struct {
	slimproto_t *proto;				/* slimproto connection */
	
	slimaudio_buffer_t *decoder_buffer;		/* decoder buffer */
	slimaudio_buffer_t *output_buffer;		/* output buffer */
	
	/* http state */
	pthread_t http_thread;
	pthread_mutex_t http_mutex;
	pthread_cond_t http_cond;
	
	slimaudio_stream_state_t http_state;
	int streamfd;
	u64_t http_total_bytes;
	u32_t http_stream_bytes;
	u8_t autostart_mode;
	bool autostart_threshold_reached;
	u32_t autostart_threshold;
	u32_t output_threshold;
	float replay_gain;
	float start_replay_gain;

	/* decode state */
	pthread_t decoder_thread;
	pthread_mutex_t decoder_mutex;
	pthread_cond_t decoder_cond;
				
	slimaudio_stream_state_t decoder_state;
	char decoder_mode;
	u8_t decoder_endianness;
	bool decoder_end_of_stream;
		
	/* output state */
	pthread_t output_thread;
	pthread_mutex_t output_mutex;
	pthread_cond_t output_cond;
	
	slimaudio_output_state_t output_state;
#ifdef PORTAUDIO_DEV
	PaStream *pa_stream;
#else
	PortAudioStream *pa_stream;
	PxMixer *px_mixer;
#endif
	slimaudio_volume_t volume_control;
	float volume;
	float prev_volume;
	float vol_adjust;
	unsigned int output_predelay_msec;
	unsigned int output_predelay_frames;
	unsigned int output_predelay_amplitude;
#ifdef ZONES
	u8_t output_num_zones;
	u8_t output_zone;
#endif
	u64_t pa_streamtime_offset;
	u64_t stream_samples;

	volatile bool output_STMs;
	volatile bool output_STMu;

	int keepalive_interval;
	
	PaDeviceIndex output_device_id;

	u32_t decode_num_tracks_started;

	/* mad decoder */
	struct mad_decoder mad_decoder;
	char *decoder_data;
	
	/* flac decoder */
	FLAC__StreamDecoder *flac_decoder;

	/* ogg decoder */
	OggVorbis_File oggvorbis_file;

#ifdef WMA_DECODER	
	/* WMA decoder */
	u8_t wma_chunking;
	u8_t wma_playstream;
	u8_t wma_metadatastream;
#endif

#ifdef AAC_DECODER
	/* AAC decoder */
	u8_t aac_format;
#endif

} slimaudio_t;

#ifdef ZONES
int slimaudio_init(slimaudio_t *audio, slimproto_t *proto, PaDeviceIndex, char *, char *, bool, int, int);
#else
int slimaudio_init(slimaudio_t *audio, slimproto_t *proto, PaDeviceIndex, char *, char *, bool);
#endif

void slimaudio_destroy(slimaudio_t *audio);
int slimaudio_open(slimaudio_t *audio);
int slimaudio_close(slimaudio_t *audio);
int slimaudio_stat(slimaudio_t *, const char *, u32_t);
/* Sets the interval between keepalive signals sent to the server
** while playback is stopped.  Defaults to -1, which means auto-select
** based on server version.
*/
void slimaudio_set_keepalive_interval(slimaudio_t *audio, int seconds);

/* Enables/disables volume control from Squeezebox Server.  Off means the
** volume will not be touched by squeezeslave.  This must be called
** before alimaudio_open.
*/
void slimaudio_set_volume_control(slimaudio_t *audio, slimaudio_volume_t vol);

/* Sets an output silence pre-delay to help avoid DACs that are slow
** to lock to drop the first few audio samples.  The amplitude, if
** greater than 0, sets the amplitude of a high-frequency tone used
** as pre-delay filler to wake-up DACS that absolutely require
** non-silent samples.
*/
void slimaudio_set_output_predelay(slimaudio_t *audio, unsigned int msec, unsigned int amplitude);

int slimaudio_http_open(slimaudio_t *a);
int slimaudio_http_close(slimaudio_t *a);
void slimaudio_http_connect(slimaudio_t *a, slimproto_msg_t *msg);
void slimaudio_http_disconnect(slimaudio_t *a);


int slimaudio_decoder_open(slimaudio_t *audio);
int slimaudio_decoder_close(slimaudio_t *audio);
void slimaudio_decoder_connect(slimaudio_t *a, slimproto_msg_t *msg);
void slimaudio_decoder_disconnect(slimaudio_t *a);


int slimaudio_output_init(slimaudio_t *a, PaDeviceIndex, char *, char *, bool);
void slimaudio_output_destroy(slimaudio_t *a);
int slimaudio_output_open(slimaudio_t *a);
int slimaudio_output_close(slimaudio_t *audio);
void slimaudio_output_connect(slimaudio_t *a, slimproto_msg_t *msg);
/* Returns -1 if audio output was alreeady disconnected. */
int slimaudio_output_disconnect(slimaudio_t *a);
void slimaudio_output_pause(slimaudio_t *audio);
void slimaudio_output_unpause(slimaudio_t *audio);
/* Returns milliseconds since track began playing */
u32_t slimaudio_output_streamtime(slimaudio_t *audio);


int slimaudio_decoder_mad_init(slimaudio_t *audio);
void slimaudio_decoder_mad_free(slimaudio_t *audio);
int slimaudio_decoder_mad_process(slimaudio_t *audio);


int slimaudio_decoder_flac_init(slimaudio_t *audio);
void slimaudio_decoder_flac_free(slimaudio_t *audio);
int slimaudio_decoder_flac_process(slimaudio_t *audio);


int slimaudio_decoder_vorbis_init(slimaudio_t *audio);
void slimaudio_decoder_vorbis_free(slimaudio_t *audio);
int slimaudio_decoder_vorbis_process(slimaudio_t *audio);


int slimaudio_decoder_pcm_init(slimaudio_t *audio);
void slimaudio_decoder_pcm_free(slimaudio_t *audio);
int slimaudio_decoder_pcm_process(slimaudio_t *audio);

#if defined(WMA_DECODER) || defined(AAC_DECODER)
void av_lib_setup (void);
#endif /* WMA_DECODER || AAC_DECODER */

#ifdef AAC_DECODER
int slimaudio_decoder_aac_init(slimaudio_t *audio);
void slimaudio_decoder_aac_free(slimaudio_t *audio);
int slimaudio_decoder_aac_process(slimaudio_t *audio);
#endif /* AAC_DECODER */

#ifdef WMA_DECODER
int slimaudio_decoder_wma_init(slimaudio_t *audio);
void slimaudio_decoder_wma_free(slimaudio_t *audio);
int slimaudio_decoder_wma_process(slimaudio_t *audio);
#endif /* WMA_DECODER */

#endif /* _SLIMAUDIO_H_ */
