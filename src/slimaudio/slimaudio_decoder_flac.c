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

#include <FLAC/stream_decoder.h>


#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"


#ifdef SLIMPROTO_DEBUG
  #define DEBUGF(...) if (slimaudio_decoder_debug) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...) if (slimaudio_decoder_debug_v) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
  #define VDEBUGF(...)
#endif
static FLAC__StreamDecoderReadStatus flac_read_callback(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data);
static FLAC__StreamDecoderWriteStatus flac_write_callback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data);
static void flac_metadata_callback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data);
static void flac_error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data);



int slimaudio_decoder_flac_init(slimaudio_t *audio) {
	assert(audio != NULL);

	audio->flac_decoder = FLAC__stream_decoder_new();
	if (audio->flac_decoder == NULL)
		return -1;
	
	return 0;
}

void slimaudio_decoder_flac_free(slimaudio_t *audio) {
	assert(audio != NULL);
	
	if (audio->flac_decoder != NULL) {
		FLAC__stream_decoder_delete(audio->flac_decoder);
		audio->flac_decoder = NULL;
	}		
}

int slimaudio_decoder_flac_process(slimaudio_t *audio) {
	assert(audio != NULL);

	DEBUGF("slimaudio_decoder_flac_process: start\n");

#if !defined(FLAC_API_VERSION_CURRENT) || FLAC_API_VERSION_CURRENT < 8
	FLAC__stream_decoder_set_client_data(audio->flac_decoder, audio);
 	FLAC__stream_decoder_set_read_callback(audio->flac_decoder, flac_read_callback);
	FLAC__stream_decoder_set_write_callback(audio->flac_decoder, flac_write_callback);
	FLAC__stream_decoder_set_metadata_callback(audio->flac_decoder, flac_metadata_callback);
	FLAC__stream_decoder_set_error_callback(audio->flac_decoder, flac_error_callback);
	FLAC__StreamDecoderState s = FLAC__stream_decoder_init(audio->flac_decoder);
#else
	FLAC__StreamDecoderInitStatus s = FLAC__stream_decoder_init_stream(audio->flac_decoder, flac_read_callback, NULL, NULL, NULL, NULL, flac_write_callback, flac_metadata_callback, flac_error_callback, audio);
#endif
	if (s != FLAC__STREAM_DECODER_SEARCH_FOR_METADATA) {
		DEBUGF("slimaudio_decoder_flac_process: init failed %i\n", s);

		FLAC__stream_decoder_finish(audio->flac_decoder);
		return -1;
	}

	audio->decoder_end_of_stream = false;

	FLAC__bool b = FLAC__stream_decoder_process_until_end_of_stream(audio->flac_decoder);
	FLAC__stream_decoder_finish(audio->flac_decoder);

	DEBUGF("slimaudio_decoder_flac_process: end\n");
	
	return (b == true ? 0 : -1);
}

static FLAC__StreamDecoderReadStatus flac_read_callback(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data) {
	slimaudio_t *audio = (slimaudio_t *) client_data;
	
	pthread_mutex_lock(&audio->decoder_mutex);
	
	VDEBUGF("flac_read_callback state=%i\n", audio->decoder_state);
	if (audio->decoder_state != STREAM_PLAYING) {
		pthread_mutex_unlock(&audio->decoder_mutex);
				
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}
	
	pthread_mutex_unlock(&audio->decoder_mutex);
	
	if (audio->decoder_end_of_stream) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;		
	}
	
	int data_len = *bytes;
	slimaudio_buffer_status ok = slimaudio_buffer_read(audio->decoder_buffer, (char*) buffer, &data_len);
	if (ok == SLIMAUDIO_BUFFER_STREAM_END) {
		audio->decoder_end_of_stream = true;
	}

	*bytes = data_len;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus flac_write_callback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data) {
	slimaudio_t *audio = (slimaudio_t *) client_data;
	int i;
	
	int nsamples = frame->header.blocksize;
	int nchannels = frame->header.channels;
	int bits_per_sample = frame->header.bits_per_sample;
	
	char * buf = (char *) malloc(nsamples * 2 * nchannels);
	char * ptr = buf;

#ifdef __BIG_ENDIAN__
	for (i=0; i<nsamples; i++) {
		FLAC__int32 sample;

		/* left */
	    sample = buffer[0][i] >> (bits_per_sample - 16);	    
	    *ptr++ = (sample >> 8) & 0xff;
	    *ptr++ = (sample >> 0) & 0xff;
	    
	    /* right */
	    if (nchannels == 2) {
	    	sample = buffer[1][i] >> (bits_per_sample - 16);
	    }
	    *ptr++ = (sample >> 8) & 0xff;
	    *ptr++ = (sample >> 0) & 0xff;
	}
#else /* __LITTLE_ENDIAN__ */
        for (i=0; i<nsamples; i++) {
                FLAC__int32 sample;

                /* left */
            sample = buffer[0][i] >> (bits_per_sample - 16);
            *ptr++ = (sample >> 0) & 0xff;
            *ptr++ = (sample >> 8) & 0xff;

            /* right */
            if (nchannels == 2) {
                sample = buffer[1][i] >> (bits_per_sample - 16);
            }
            *ptr++ = (sample >> 0) & 0xff;
            *ptr++ = (sample >> 8) & 0xff;
        }
#endif

	slimaudio_buffer_write(audio->output_buffer, buf, nsamples * 2 * nchannels);
	free(buf);
	
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_metadata_callback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data) {
	if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		DEBUGF("flac channels:        %i\n", metadata->data.stream_info.channels);
		DEBUGF("flac sample_rate:     %i\n", metadata->data.stream_info.sample_rate);
		DEBUGF("flac bits_per_sample: %i\n", metadata->data.stream_info.bits_per_sample);
	}
}

static void flac_error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data) {
	fprintf(stderr, "flac decoder error %i\n", /*FLAC__StreamDecoderErrorStatusString[status]*/ status);
}


