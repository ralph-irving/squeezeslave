/*
 *   SlimProtoLib Copyright (c) 2010 Duane Paddock
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

#ifdef AAC_DECODER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

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

#define INBUF_SIZE 4096
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

int slimaudio_decoder_aac_init(slimaudio_t *audio) {

	/* Must be called before using avcodec library */
	avcodec_init();

	/* Register all the codecs */
	avcodec_register_all();

	return 0;
}

void slimaudio_decoder_aac_free(slimaudio_t *audio) {
}

int slimaudio_decoder_aac_process(slimaudio_t *audio) {
//	unsigned char data[AUDIO_CHUNK_SIZE];
//	int buffer[AUDIO_CHUNK_SIZE/2];
//	int i;
	
	int data_len = 0;
//	unsigned char *ptr = data;
	slimaudio_buffer_status ok = SLIMAUDIO_BUFFER_STREAM_START;

	AVCodec *codec;
	AVCodecContext *ctxt = NULL;
	int out_size;
	int len = 0;
	u8_t *outbuf;
	u8_t *inbuf;
	AVPacket avpkt;
	static u8_t extradata[] = { 19, 144 };

	DEBUGF("aac: decoder_format:%d '%c'\n", audio->aac_format, audio->aac_format);

	av_init_packet(&avpkt);

	/* Find the AAC audio decoder */
	codec = avcodec_find_decoder(CODEC_ID_AAC);
	if (!codec)
	{
		DEBUGF("aac: codec not found.\n");
		return -1;
	} 
	
	ctxt = avcodec_alloc_context();
	ctxt->extradata = extradata;
	ctxt->extradata_size = 2;

	/* Open codec */
	if (avcodec_open(ctxt, codec) < 0)
	{
		DEBUGF("aac: could not open codec\n");
		return -1;
	}

	inbuf = av_malloc(AUDIO_INBUF_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);
	outbuf = av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);

	while (ok != SLIMAUDIO_BUFFER_STREAM_END)
	{
		/* Keep partial samples from last iteration */
//		int remainder = data_len;
//		if (remainder > 0) {
//			memcpy(data, ptr, remainder);
//		}			
		
		data_len = AUDIO_INBUF_SIZE;
		ok = slimaudio_buffer_read(audio->decoder_buffer, (char*)(inbuf), &data_len);

//		int nsamples = data_len / 2;

		avpkt.data = inbuf;
		avpkt.size = data_len;

		while (avpkt.size > 0)
		{
			out_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
			len = avcodec_decode_audio3(ctxt, (int16_t *)outbuf, &out_size, &avpkt);
			if (len < 0)
			{
				DEBUGF("aac: Error while decoding\n");
				ok = SLIMAUDIO_BUFFER_STREAM_END;
				break;
			}

			if (out_size > 0)
			{
				/* if a frame has been decoded, output it */
				slimaudio_buffer_write(audio->output_buffer, (char*)outbuf, out_size);
			}

			avpkt.size -= len;
			avpkt.data += len;

			if (avpkt.size < AUDIO_REFILL_THRESH)
			{
				/* Refill the input buffer, to avoid trying to decode
				 * incomplete frames. Instead of this, one could also use
				 * a parser, or use a proper container format through
				 * libavformat.
				 */
				memmove(inbuf, avpkt.data, avpkt.size);

				avpkt.data = inbuf;
				data_len =  AUDIO_INBUF_SIZE - avpkt.size;

				ok = slimaudio_buffer_read(audio->decoder_buffer,(char *) (avpkt.data + avpkt.size), &data_len);

				len = data_len;

				if (len > 0)
					avpkt.size += len;
			}
		}

	}

	if ( inbuf != NULL )
		av_free(inbuf);
	if ( outbuf != NULL )
		av_free(outbuf);

	avcodec_close(ctxt);

	if ( ctxt != NULL )
		av_free(ctxt);
	
	if ( len < 0 )
		return -1;
	else
		return 0;
}

#endif /* AAC_DECODER */
