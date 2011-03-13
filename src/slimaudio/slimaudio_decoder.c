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
#include <string.h>
#include <assert.h>

#include <mad.h>

#if defined(WMA_DECODER) || defined(AAC_DECODER)
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#endif /* WMA_DECODER || AAC_DECODER */

#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"

#ifdef SLIMPROTO_DEBUG
  bool slimaudio_decoder_debug;
  bool slimaudio_decoder_debug_r;
  bool slimaudio_decoder_debug_v;
  #define DEBUGF(...) if (slimaudio_decoder_debug) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...) if (slimaudio_decoder_debug_v) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
  #define VDEBUGF(...)
#endif

static void *decoder_thread(void *ptr);

#if defined(WMA_DECODER) || defined(AAC_DECODER)

bool av_lib_init = false; /* Only initialize ffmpeg library once */

static void av_err_callback(void *ptr, int level, const char *fmt, va_list vargs)
{
	VDEBUGF(fmt, vargs);
}

void av_lib_setup (void)
{
	if ( !av_lib_init )
	{
		/* Setup error message capture */
		av_log_set_callback(av_err_callback);
		av_log_set_level(AV_LOG_VERBOSE);

		/* Register all the codecs */
		av_register_all();
		DEBUGF("decoder_thread: av_register_all\n");

		AVInputFormat *p = NULL;
		p = av_iformat_next(p);
		while (p)
		{
			VDEBUGF("decoder_thread: %s: %s:\n", p->name, p->long_name);
			p = av_iformat_next(p);
		};

		VDEBUGF("decoder_thread: %s\n", avformat_configuration() );

		av_lib_init = true;
	}
}
#endif /* WMA_DECODER || AAC_DECODER */

int slimaudio_decoder_open(slimaudio_t *audio) {
	pthread_mutex_init(&(audio->decoder_mutex), NULL);
	pthread_cond_init(&(audio->decoder_cond), NULL);
	audio->decoder_data = (char *) malloc(AUDIO_CHUNK_SIZE);

	if (slimaudio_decoder_mad_init(audio) != 0)
		return -1;		
	if (slimaudio_decoder_flac_init(audio) != 0)
		return -1;
	if (slimaudio_decoder_vorbis_init(audio) != 0)
		return -1;
	if (slimaudio_decoder_pcm_init(audio) != 0)
		return -1;
#ifdef AAC_DECODER	
	if (slimaudio_decoder_aac_init(audio) != 0)
		return -1;
#endif
#ifdef WMA_DECODER	
	if (slimaudio_decoder_wma_init(audio) != 0)
		return -1;
#endif
	
	/* 
	 * Acquire the decoder mutex before the thread is started, to make sure
	 * no other thread can acquire it before.  This would lead to a deadlock
	 * as the decoder thread needs to enter its cond-variable wait before
	 * any other thread starts interacting with it.
	 */
#ifndef BSD_THREAD_LOCKING
	pthread_mutex_lock(&audio->decoder_mutex);
#endif

	if (pthread_create(&audio->decoder_thread, NULL, decoder_thread, (void*) audio) != 0) {
		fprintf(stderr, "Error creating decoder thread\n");
#ifndef BSD_THREAD_LOCKING
		pthread_mutex_unlock(&audio->decoder_mutex);
#endif
		return -1;		
	}
	
	return 0;
}

int slimaudio_decoder_close(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->decoder_mutex);
	
	audio->decoder_state = STREAM_QUIT;
	
	pthread_mutex_unlock(&audio->decoder_mutex);
	
	pthread_cond_broadcast(&audio->decoder_cond);
	
	pthread_join(audio->decoder_thread, NULL);	
	slimaudio_decoder_mad_free(audio);
	slimaudio_decoder_flac_free(audio);
	slimaudio_decoder_vorbis_free(audio);
	slimaudio_decoder_pcm_free(audio);
#ifdef AAC_DECODER
	slimaudio_decoder_aac_free(audio);
#endif
#ifdef WMA_DECODER
	slimaudio_decoder_wma_free(audio);
#endif
	
	pthread_mutex_destroy(&(audio->decoder_mutex));
	pthread_cond_destroy(&(audio->decoder_cond));
	return 0;
}


static void *decoder_thread(void *ptr) {
	slimaudio_t *audio = (slimaudio_t *) ptr;
#ifdef BSD_THREAD_LOCKING
	pthread_mutex_lock(&audio->decoder_mutex);
#endif
	
	audio->decoder_state = STREAM_STOPPED;

	bool decoder_failed = false;	
	unsigned char first_time = 1;

	while (true) {				
		switch (audio->decoder_state) {
			case STREAM_STOPPED:
				DEBUGF("decoder_thread: STREAM_STOPPED first_time:%d\n", first_time);

				if (first_time == 1) {
					/* 
					 * The first time in, the mutex has already been
					 * acquired in the function that starts the thread.
					 */
					first_time = 0;
				}
				else {
					pthread_mutex_lock(&audio->decoder_mutex);
				}

				if ( decoder_failed )
				{
					decoder_failed = false;

					slimaudio_stat(audio, "STMn", (u32_t) 0); // decoder does not support format

					DEBUGF("decoder_thread: decoder %c failed\n", audio->decoder_mode);
				}

				pthread_cond_wait(&audio->decoder_cond, &audio->decoder_mutex);
				pthread_mutex_unlock(&audio->decoder_mutex);
				break;
				
			case STREAM_PLAYING:
				DEBUGF("decoder_thread: STREAM_PLAYING type %c\n", audio->decoder_mode);

				switch (audio->decoder_mode) {
				case 'm': // mp3
					slimaudio_decoder_mad_process(audio);
					break;
					
				case 'f': // flac
					slimaudio_decoder_flac_process(audio);
					break;
					
				case 'o': // ogg vorbis
					slimaudio_decoder_vorbis_process(audio);
					break;

				case 'p': // wav
					slimaudio_decoder_pcm_process(audio);
					break;
#ifdef AAC_DECODER					
				case 'a': // aac
					if ( slimaudio_decoder_aac_process(audio) < 0 )
					{
						decoder_failed = true ;
					}

					break;
#endif					
#ifdef WMA_DECODER					
				case 'w': // wma
					if ( slimaudio_decoder_wma_process(audio) < 0 )
					{
						decoder_failed = true ;
					}

					break;
#endif					
				default:
					fprintf(stderr, "Cannot decode unknown format: %c\n", audio->decoder_mode);
					slimaudio_stat(audio, "STMn", (u32_t) 0); // decoder does not support format
					break;
				}

				DEBUGF("decoder_thread: STREAM_PLAY (before STMd) previous state: %i\n",
						audio->decoder_state);

				if ( audio->decoder_state == STREAM_PLAYING )
				{
					slimaudio_stat(audio, "STMd", (u32_t) 0);
					DEBUGF("decoder_thread: STREAM_PLAY (after STMd) previous state: %i\n",
							audio->decoder_state);
				}

			case STREAM_STOP:
				DEBUGF("decoder_thread: STREAM_STOP previous state: %i\n", audio->decoder_state);
				pthread_mutex_lock(&audio->decoder_mutex);
				
				audio->decoder_state = STREAM_STOPPED;

				slimaudio_buffer_close(audio->output_buffer);

				pthread_mutex_unlock(&audio->decoder_mutex);

				pthread_cond_broadcast(&audio->decoder_cond);

				break;
			
			case STREAM_QUIT:
				DEBUGF("decoder_thread: STREAM_QUIT\n");
				return 0;
		}		
	}
}


void slimaudio_decoder_connect(slimaudio_t *audio, slimproto_msg_t *msg) {
	DEBUGF("slimaudio_decoder_connect\n");
	slimaudio_decoder_disconnect(audio);

	pthread_mutex_lock(&audio->decoder_mutex);

	audio->decoder_mode = msg->strm.mode;
	audio->decoder_endianness = msg->strm.pcm_endianness;

	slimaudio_buffer_open(audio->output_buffer, NULL);

	audio->decoder_state = STREAM_PLAYING;

	pthread_mutex_unlock(&audio->decoder_mutex);

	pthread_cond_broadcast(&audio->decoder_cond);
}


void slimaudio_decoder_disconnect(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->decoder_mutex);

	if (audio->decoder_state == STREAM_STOPPED) {
		pthread_mutex_unlock(&audio->decoder_mutex);	
		return;
	}

	audio->decoder_state = STREAM_STOP;
	pthread_cond_broadcast(&audio->decoder_cond);

	/* closing buffer will wake the decoder thread */
	slimaudio_buffer_flush(audio->output_buffer);

	while (audio->decoder_state == STREAM_STOP) {
		pthread_cond_wait(&audio->decoder_cond, &audio->decoder_mutex);
	}

	pthread_mutex_unlock(&audio->decoder_mutex);
}

