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

#if defined(TREMOR_DECODER) && defined(__BIG_ENDIAN__)
#error "TREMOR_DECODER not supported on big endian systems."
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define OV_EXCLUDE_STATIC_CALLBACKS

#ifdef TREMOR_DECODER
#include <vorbis/ivorbisfile.h>
#else
#include <vorbis/vorbisfile.h>
#endif /* TREMOR_DECODER */

#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"

#ifdef SLIMPROTO_DEBUG
  #define DEBUGF(...) if (slimaudio_decoder_debug) fprintf(stderr, __VA_ARGS__)
  #define RDEBUGF(...) if (slimaudio_decoder_debug_r) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...) if (slimaudio_decoder_debug_v) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
  #define RDEBUGF(...)
  #define VDEBUGF(...)
#endif

static size_t vorbis_read_func(void *ptr, size_t size, size_t nmemb, void *datasource);
static int vorbis_seek_func(void *datasource, ogg_int64_t offset, int whence);
static int vorbis_close_func(void *datasource);
static long vorbis_tell_func(void *datasource);

int slimaudio_decoder_vorbis_init(slimaudio_t *audio) {
	return 0;
}

void slimaudio_decoder_vorbis_free(slimaudio_t *audio) {
}

int slimaudio_decoder_vorbis_process(slimaudio_t *audio) {
	int err;
	ov_callbacks callbacks;
	int bytes_read;
	int current_bitstream;
	bool ogg_continue = true;
	char buffer[AUDIO_CHUNK_SIZE];
	
	assert(audio != NULL);

	DEBUGF("slimaudio_decoder_vorbis_process: start\n");
	
	callbacks.read_func = vorbis_read_func;
	callbacks.seek_func = vorbis_seek_func;
	callbacks.close_func = vorbis_close_func;
	callbacks.tell_func = vorbis_tell_func;	

	audio->decoder_end_of_stream = false;
	
	if ((err = ov_open_callbacks(audio, &audio->oggvorbis_file, NULL, 0, callbacks)) < 0)
	{
		DEBUGF("libvorbis: (ogg) ov_open_callbacks failed (%i)\n", err);
		return -1;
	}
	
	
	do {
#if defined(TREMOR_DECODER) /* Use Tremor fixed point vorbis decoder, little endian only */
		bytes_read = ov_read(&audio->oggvorbis_file, buffer, AUDIO_CHUNK_SIZE, &current_bitstream);
#elif defined(__BIG_ENDIAN__)
		bytes_read = ov_read(&audio->oggvorbis_file, buffer, AUDIO_CHUNK_SIZE, 1, 2, 1, &current_bitstream);
#else /* __LITTLE_ENDIAN__ */
		bytes_read = ov_read(&audio->oggvorbis_file, buffer, AUDIO_CHUNK_SIZE, 0, 2, 1, &current_bitstream);
#endif
		switch (bytes_read) {

		case OV_HOLE: /* Recoverable error in stream */
			RDEBUGF("libvorbis: (ogg) decoding error OV_HOLE (0x%04x)\n", bytes_read );
			break ;

		case OV_EBADLINK:
			DEBUGF("libvorbis: (ogg) decoding error OV_EBADLINK (0x%04x)\n", bytes_read );
			ogg_continue = false ;
			break;

		case OV_EINVAL:
			DEBUGF("libvorbis: (ogg) decoding error OV_EINVAL (0x%04x)\n", bytes_read );
			ogg_continue = false ;
			break ;
		
		case 0: /* End of file */
			ogg_continue = false ;
			break;
			
		default:
			slimaudio_buffer_write(audio->output_buffer, buffer, bytes_read);
		}

	} while ( ogg_continue );
	
	if ((err = ov_clear(&audio->oggvorbis_file)) < 0)
	{
		DEBUGF("libvorbis: (ogg) ov_clear failed (%i)\n", err);
		return -1;	
	}

	DEBUGF("slimaudio_decoder_vorbis_process: end\n");

	return 0;
}


static size_t vorbis_read_func(void *ptr, size_t size, size_t nmemb, void *datasource) {
	slimaudio_t *audio = (slimaudio_t *) datasource;
	int data_len;
	slimaudio_buffer_status ok;
	pthread_mutex_lock(&audio->decoder_mutex);

	if (audio->decoder_state != STREAM_PLAYING) {
		pthread_mutex_unlock(&audio->decoder_mutex);
		return 0;
	}

	pthread_mutex_unlock(&audio->decoder_mutex);
	
	if (audio->decoder_end_of_stream)
		return 0;
	
	data_len = nmemb;
	ok = slimaudio_buffer_read(audio->decoder_buffer, ptr, &data_len);
	if (ok == SLIMAUDIO_BUFFER_STREAM_END) {
		audio->decoder_end_of_stream = true;
	}
	
	return data_len;
}

static int vorbis_seek_func(void *datasource, ogg_int64_t offset, int whence) {
	return -1; /* stream is not seekable */
}

static int vorbis_close_func(void *datasource) {
	return 0;
}

static long vorbis_tell_func(void *datasource) {
	return 0;
}
