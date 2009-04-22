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

#include <mad.h>

#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"

#ifdef SLIMPROTO_DEBUG
  #define DEBUGF(...) if (slimaudio_decoder_debug) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...) if (slimaudio_decoder_debug_v) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
  #define VDEBUGF(...)
#endif

static enum mad_flow mad_input(void *data, struct mad_stream *stream);
static enum mad_flow mad_output(void *data, struct mad_header const *header, struct mad_pcm *pcm);
static enum mad_flow mad_error(void *data, struct mad_stream *stream,struct mad_frame *frame);

struct audio_stats {
  unsigned long clipped_samples;
  mad_fixed_t peak_clipping;
  mad_fixed_t peak_sample;
};

struct audio_dither {
  mad_fixed_t error[3];
  mad_fixed_t random;
};

static struct audio_dither left_dither, right_dither;

static struct audio_stats stats;

# if defined(_MSC_VER) || defined(__SUNPRO_C)
extern  /* needed to satisfy bizarre MSVC++ interaction with inline */
# endif
inline signed long audio_linear_dither(unsigned int bits, mad_fixed_t sample,
				struct audio_dither *dither,
				struct audio_stats *stats);


int slimaudio_decoder_mad_init(slimaudio_t *audio) {
	return 0;
}

void slimaudio_decoder_mad_free(slimaudio_t *audio) {
}

int slimaudio_decoder_mad_process(slimaudio_t *audio) {
	/* configure input, output, and error functions */
    mad_decoder_init(&audio->mad_decoder, audio,
		mad_input, 0 /* header */, 0 /* filter */, mad_output,
		mad_error, 0 /* message */);

	/* start decoding */
	audio->decoder_end_of_stream = false;
	int result = mad_decoder_run(&audio->mad_decoder, MAD_DECODER_MODE_SYNC);
	if (result != 0)
		return -1;

	/* release the decoder */
	mad_decoder_finish(&audio->mad_decoder);	
	return 0;
}



/*
 * This is the input callback. The purpose of this callback is to (re)fill
 * the stream buffer which is to be decoded. In this example, an entire file
 * has been mapped into memory, so we just call mad_stream_buffer() with the
 * address and length of the mapping. When this callback is called a second
 * time, we are finished decoding.
 */

static
enum mad_flow mad_input(void *data,
		    struct mad_stream *stream)
{
	slimaudio_t *audio = (slimaudio_t *) data;

	/* keep partial frame from last decode ... */
	int remainder = stream->bufend - stream->next_frame;
	memcpy (audio->decoder_data, stream->this_frame, remainder);
	
	pthread_mutex_lock(&audio->decoder_mutex);

	DEBUGF("decode_input state=%i remainder=%i\n", audio->decoder_state, remainder);
	if (audio->decoder_state != STREAM_PLAYING) {
		pthread_mutex_unlock(&audio->decoder_mutex);
		return MAD_FLOW_STOP;
	}

	pthread_mutex_unlock(&audio->decoder_mutex);

	if (audio->decoder_end_of_stream)
		return MAD_FLOW_STOP;
	
	int data_len = AUDIO_CHUNK_SIZE-MAD_BUFFER_GUARD-remainder;
	slimaudio_buffer_status ok = slimaudio_buffer_read(audio->decoder_buffer, audio->decoder_data + remainder, &data_len);
	if (ok == SLIMAUDIO_BUFFER_STREAM_END) {
		memset(audio->decoder_data + remainder + data_len, 0, MAD_BUFFER_GUARD);
		audio->decoder_end_of_stream = true;
	}
	
	mad_stream_buffer(stream, (const unsigned char *)audio->decoder_data, data_len + remainder);	
	return MAD_FLOW_CONTINUE;
}

/*
 * The following utility routine performs simple rounding, clipping, and
 * scaling of MAD's high-resolution samples down to 16 bits. It does not
 * perform any dithering or noise shaping, which would be recommended to
 * obtain any exceptional audio quality. It is therefore not recommended to
 * use this routine if high-quality output is desired.
 */

static inline
signed int scale(mad_fixed_t sample)
{
  /* round */
  sample += (1L << (MAD_F_FRACBITS - 16));

  /* clip */
  if (sample >= MAD_F_ONE)
    sample = MAD_F_ONE - 1;
  else if (sample < -MAD_F_ONE)
    sample = -MAD_F_ONE;

  /* quantize */
  return sample >> (MAD_F_FRACBITS + 1 - 16);
}

/*
 * This is the output callback function. It is called after each frame of
 * MPEG audio data has been completely decoded. The purpose of this callback
 * is to output (or play) the decoded PCM audio.
 */

static
enum mad_flow mad_output(void *data,
		     struct mad_header const *header,
		     struct mad_pcm *pcm)
{
	unsigned int nchannels, nsamples;
	mad_fixed_t const *left_ch, *right_ch;
	char *buf, *ptr;
	int i;

	slimaudio_t *audio = (slimaudio_t *) data;

/*	pthread_mutex_lock(&audio->decoder_mutex);

	if (audio->decoder_state != STREAM_PLAYING) {
		pthread_mutex_unlock(&audio->decoder_mutex);
		return MAD_FLOW_STOP;
	}

	pthread_mutex_unlock(&audio->decoder_mutex);
*/
	/* pcm->samplerate contains the sampling frequency */
	nchannels = pcm->channels;
	nsamples  = pcm->length;
	left_ch   = pcm->samples[0];
	right_ch  = pcm->samples[1];

	VDEBUGF("decode_output state=%i nchannels=%i nsamples=%i\n", audio->decoder_state, nchannels, nsamples);

	buf = (char *) malloc(nsamples * 2 * 2 ); /* always stereo output */
	ptr = buf;

#ifdef __BIG_ENDIAN__
	for (i=0; i<nsamples; i++) {
		signed int sample;

		/* left */
		sample = audio_linear_dither(16, *left_ch++,
					&left_dither, &stats);
	    *ptr++ = (sample >> 8) & 0xff;
	    *ptr++ = (sample >> 0) & 0xff;
	    
	    /* right */
	    if (nchannels == 2) {
			sample = audio_linear_dither(16, *right_ch++,
				     	&right_dither, &stats);
	    }	    
		*ptr++ = (sample >> 8) & 0xff;
		*ptr++ = (sample >> 0) & 0xff;
	}
#else /* __LITTLE_ENDIAN__ */
        for (i=0; i<nsamples; i++) {
                signed int sample;

                /* left */
                sample = audio_linear_dither(16, *left_ch++,
                                        &left_dither, &stats);
            *ptr++ = (sample >> 0) & 0xff;
            *ptr++ = (sample >> 8) & 0xff;

            /* right */
            if (nchannels == 2) {
                        sample = audio_linear_dither(16, *right_ch++,
                                        &right_dither, &stats);
            }
                *ptr++ = (sample >> 0) & 0xff;
                *ptr++ = (sample >> 8) & 0xff;
        }
#endif

	slimaudio_buffer_write(audio->output_buffer, buf, nsamples * 2 * 2 /* always stereo output */);

	free(buf);
	
	return MAD_FLOW_CONTINUE;
}

/*
 * This is the error callback function. It is called whenever a decoding
 * error occurs. The error is indicated by stream->error; the list of
 * possible MAD_ERROR_* errors can be found in the mad.h (or stream.h)
 * header file.
 */

static
enum mad_flow mad_error(void *data,
		    struct mad_stream *stream,
		    struct mad_frame *frame)
{

  fprintf(stderr, "libmad: (mp3) decoding error (0x%04x)\n", stream->error); //FIXME

  /* return MAD_FLOW_BREAK here to stop decoding (and propagate an error) */

  return MAD_FLOW_CONTINUE;
}




/*
 * madplay - MPEG audio decoder and player
 * Copyright (C) 2000-2004 Robert Leslie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id: audio.c,v 1.36 2004/01/23 09:41:31 rob Exp $
 */


/*
 * NAME:	prng()
 * DESCRIPTION:	32-bit pseudo-random number generator
 */
static inline
unsigned long prng(unsigned long state)
{
  return (state * 0x0019660dL + 0x3c6ef35fL) & 0xffffffffL;
}

/*
 * NAME:	audio_linear_dither()
 * DESCRIPTION:	generic linear sample quantize and dither routine
 */
# if defined(_MSC_VER) || defined(__SUNPRO_C)
extern  /* needed to satisfy bizarre MSVC++ interaction with inline */
# endif
inline
signed long audio_linear_dither(unsigned int bits, mad_fixed_t sample,
				struct audio_dither *dither,
				struct audio_stats *stats)
{
  unsigned int scalebits;
  mad_fixed_t output, mask, random;

  enum {
    MIN = -MAD_F_ONE,
    MAX =  MAD_F_ONE - 1
  };

  /* noise shape */
  sample += dither->error[0] - dither->error[1] + dither->error[2];

  dither->error[2] = dither->error[1];
  dither->error[1] = dither->error[0] / 2;

  /* bias */
  output = sample + (1L << (MAD_F_FRACBITS + 1 - bits - 1));

  scalebits = MAD_F_FRACBITS + 1 - bits;
  mask = (1L << scalebits) - 1;

  /* dither */
  random  = prng(dither->random);
  output += (random & mask) - (dither->random & mask);

  dither->random = random;

  /* clip */
  if (output >= stats->peak_sample) {
    if (output > MAX) {
      ++stats->clipped_samples;
      if (output - MAX > stats->peak_clipping)
	stats->peak_clipping = output - MAX;

      output = MAX;

      if (sample > MAX)
	sample = MAX;
    }
    stats->peak_sample = output;
  }
  else if (output < -stats->peak_sample) {
    if (output < MIN) {
      ++stats->clipped_samples;
      if (MIN - output > stats->peak_clipping)
	stats->peak_clipping = MIN - output;

      output = MIN;

      if (sample < MIN)
	sample = MIN;
    }
    stats->peak_sample = -output;
  }

  /* quantize */
  output &= ~mask;

  /* error feedback */
  dither->error[0] = sample - output;

  /* scale */
  return output >> scalebits;
}
