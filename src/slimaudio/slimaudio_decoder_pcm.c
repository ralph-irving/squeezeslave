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

#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"

#ifdef SLIMPROTO_DEBUG
  #define DEBUGF(...) if (slimaudio_decoder_debug) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...) if (slimaudio_decoder_debug_v) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
  #define VDEBUGF(...)
#endif

int slimaudio_decoder_pcm_init(slimaudio_t *audio) {
	return 0;
}

void slimaudio_decoder_pcm_free(slimaudio_t *audio) {
}

int slimaudio_decoder_pcm_process(slimaudio_t *audio) {
	unsigned char data[AUDIO_CHUNK_SIZE];
	int buffer[AUDIO_CHUNK_SIZE/2];
	int i;
	
	int data_len = 0;
	unsigned char *ptr = data;
	slimaudio_buffer_status ok = SLIMAUDIO_BUFFER_STREAM_START;

	while (ok != SLIMAUDIO_BUFFER_STREAM_END) {
		/* keep partial samples from last iteration */
		int remainder = data_len;
		if (remainder > 0) {
			memcpy(data, ptr, remainder);
		}			
		
		data_len = AUDIO_CHUNK_SIZE-remainder;
		ok = slimaudio_buffer_read(audio->decoder_buffer, (char*)(data+remainder), &data_len);
		int nsamples = data_len / 2;

		/* convert buffer into samples */
		ptr = data;
		if (audio->decoder_endianness == '1') {
			for (i=0; i<nsamples; i++) {
				buffer[i] = *ptr++;
				buffer[i] |= (*ptr++) << 8;
			}
		}
		else {
			for (i=0; i<nsamples; i++) {
				buffer[i] = (*ptr++) << 8;
				buffer[i] |= *ptr++;
			}			
		}

		/* can perform processing here ... */

#ifdef __BIG_ENDIAN__
	ptr = data;
	for (i=0; i<nsamples; i++) {
		int sample;

	    sample = buffer[i];
	    *ptr++ = (sample >> 8) & 0xff;
	    *ptr++ = (sample >> 0) & 0xff;	    
	}
#else /* __LITTLE_ENDIAN__ */
	ptr = data;
    for (i=0; i<nsamples; i++) {
		int sample;

		sample = buffer[i];
		*ptr++ = (sample >> 0) & 0xff;
		*ptr++ = (sample >> 8) & 0xff;
	}
#endif
		
		slimaudio_buffer_write(audio->output_buffer, (char*)data, nsamples * 2);
		data_len -= nsamples * 2;
	}
	
	return 0;
}
