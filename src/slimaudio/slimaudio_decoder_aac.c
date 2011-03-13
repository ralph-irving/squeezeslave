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
#include <libavformat/avformat.h>

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

#define AUDIO_INBUF_SIZE (AUDIO_CHUNK_SIZE*2)

static int av_read_unblock(void)
{
	return 1;
}

#ifndef WMA_DECODER
static void av_err_callback(void *ptr, int level, const char *fmt, va_list vargs)
{
	fprintf(stderr, fmt, vargs);
}
#endif

int slimaudio_decoder_aac_init(slimaudio_t *audio) {

	// url_set_interrupt_cb(av_read_unblock);

#ifndef WMA_DECODER
	/* Setup error message capture */
	av_log_set_callback(av_err_callback);
	av_log_set_level(AV_LOG_VERBOSE);

	/* Register all the codecs */
	av_register_all();
	DEBUGF("aac: av_register_all\n");

        AVInputFormat *p = NULL;
	p = av_iformat_next(p);
        while (p)
	{
                VDEBUGF("aac: %s: %s:\n", p->name, p->long_name);
		p = av_iformat_next(p);
	};

	VDEBUGF("aac: %s\n", avformat_configuration() );
#endif /* WMA_DECODER */

	return 0;
}

void slimaudio_decoder_aac_free(slimaudio_t *audio) {
}

static int av_read_data(void *opaque, char *buffer, int buf_size)
{
	slimaudio_t *audio = (slimaudio_t *) opaque;

	pthread_mutex_lock(&audio->decoder_mutex);

	VDEBUGF("av_read_data state=%i\n", audio->decoder_state);
	if (audio->decoder_state != STREAM_PLAYING) {
		pthread_mutex_unlock(&audio->decoder_mutex);
		DEBUGF("slimaudio_decoder_aac_process: STREAM_NOT_PLAYING\n");
		return -1;
        }

	pthread_mutex_unlock(&audio->decoder_mutex);

        if (audio->decoder_end_of_stream) {
                audio->decoder_end_of_stream = false;
		return 0;
        }
#if 0
	int avail = 0;
	do
	{
		Pa_Sleep(100);
		avail = slimaudio_buffer_available(audio->decoder_buffer);
	}
	while ( (avail < buf_size) && (avail != 0) );
#endif
        int data_len = buf_size;
	VDEBUGF("aac: read ask: %d\n", data_len);
        slimaudio_buffer_status ok = slimaudio_buffer_read(audio->decoder_buffer, (char*) buffer, &data_len);
	VDEBUGF("aac: read actual: %d\n", data_len);

        if (ok == SLIMAUDIO_BUFFER_STREAM_END) {
                DEBUGF("slimaudio_decoder_aac_process: EOS\n");
                audio->decoder_end_of_stream = true;
        }

	return data_len;
}

int slimaudio_decoder_aac_process(slimaudio_t *audio) {
//	unsigned char data[AUDIO_CHUNK_SIZE];
//	int buffer[AUDIO_CHUNK_SIZE/2];
//	int i;
	
//	unsigned char *ptr = data;
	char streamformat[16];
	int out_size;
	int len = 0;
	int iRC;
	u8_t *outbuf;
	u8_t *inbuf;

	/* It is not really correct to assume that all MP4 files (which were not
	 * otherwise recognized as ALAC or MOV by the scanner) are AAC, but that
	 * is the current server side status.
	 *
	 * Container type and bitstream format:
	 *
	 * '1' (adif),
	 * '2' (adts),
	 * '3' (latm within loas),
	 * '4' (rawpkts),
	 * '5' (mp4ff),
	 * '6' (latm within rawpkts)
	 * 
	 * This is a hack that assumes:
	 * (1) If the original content-type of the track is MP4 or SLS then we
	 *     are streaming an MP4 file (without any transcoding);
	 * (2) All other AAC streams will be adts.
	 *
	 * So the server will only set aac_format to '2' or '5'.
	 */

	DEBUGF ("aac: decoder_format:%d '%c'\n", audio->aac_format, audio->aac_format);

	int audioStream = 0; /* Always zero for aac decoder */

	switch ( audio->aac_format )
	{
		case '2':
			strncpy ( streamformat, "aac", sizeof (streamformat) );
			break;
		case '5':
			strncpy ( streamformat, "m4a", sizeof (streamformat) );
			break;
		default:
			fprintf (stderr, "aac: unknown container type: %c\n" ,audio->aac_format );
			return -1;
	}

	DEBUGF ("aac: play audioStream: %d\n", audioStream);

	AVInputFormat* pAVInputFormat = av_find_input_format(streamformat);
	if( !pAVInputFormat )
	{
		DEBUGF("aac: probe failed\n");
		return -1;
	}
	else
	{
		DEBUGF("aac: probe ok name:%s lname:%s\n", pAVInputFormat->name, pAVInputFormat->long_name);
		pAVInputFormat->flags |= AVFMT_NOFILE;
	}

	inbuf = av_malloc(AUDIO_INBUF_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);
	if ( !inbuf )
	{
		DEBUGF("aac: inbuf alloc failed.\n");
		return -1;
	}

	ByteIOContext ByteIOCtx;

	iRC = init_put_byte( &ByteIOCtx, inbuf, AUDIO_CHUNK_SIZE, 0, audio, av_read_data, NULL, NULL ) ;
	if( iRC < 0)
	{
		DEBUGF("aac: init_put_byte failed:%d\n", iRC);
		return -1;
	}
	else
	{
		ByteIOCtx.is_streamed = 1;
	}

	AVFormatContext* pFormatCtx;
	AVCodecContext *pCodecCtx;

	iRC = av_open_input_stream(&pFormatCtx, &ByteIOCtx, "", pAVInputFormat, NULL);

	if (iRC < 0)
	{
		DEBUGF("aac: input stream open failed:%d\n", iRC);
		return -1;
	}
	else
	{
		iRC = av_find_stream_info(pFormatCtx);
		if ( iRC < 0 )
		{
			DEBUGF("aac: find stream info failed:%d\n", iRC);
			return -1;
		}
		else
		{
			if ( pFormatCtx->nb_streams < audioStream )
			{
				DEBUGF("aac: invalid stream.\n");
				return -1;
			}

			if ( pFormatCtx->streams[audioStream]->codec->codec_type != CODEC_TYPE_AUDIO )
			{
				DEBUGF("aac: stream: %d is not audio.\n", audioStream );
				return -1;
			}
			else
			{
				pCodecCtx = pFormatCtx->streams[audioStream]->codec;
			}
		}
	}

	AVCodec *pCodec;

	/* Find the WMA audio decoder */
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if ( !pCodec )
	{
		DEBUGF("aac: codec not found.\n");
		return -1;
	} 
	
	/* Open codec */
	iRC = avcodec_open(pCodecCtx, pCodec);
	if ( iRC < 0)
	{
		DEBUGF("aac: could not open codec:%d\n", iRC);
		return -1;
	}

	outbuf = av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);
	if ( !outbuf )
	{
		DEBUGF("aac: outbuf alloc failed.\n");
		return -1;
	}

	bool eos = false;
	AVPacket avpkt;

	while ( ! eos )
	{
		iRC = av_read_frame (pFormatCtx, &avpkt);

		/* Some decoders fail to read the last packet so additional handling is required */
	        if (iRC < 0)
		{
			DEBUGF("aac: av_read_frame error: %d\n", iRC);

			if ( (iRC == AVERROR_EOF) )
			{
				DEBUGF("aac: AVERROR_EOF\n");
				eos=true;
			}

			if ( url_feof(pFormatCtx->pb) )
			{
				DEBUGF("aac: url_feof\n");
				eos=true;
			}

			if ( url_ferror(pFormatCtx->pb) )
			{
				DEBUGF("aac: url_ferror\n");
#if 0
		                break;
#endif
			}
		}

		out_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
		len = avcodec_decode_audio3(pCodecCtx, (int16_t *)outbuf, &out_size, &avpkt);
		if (len < 0)
		{
			DEBUGF("aac: no audio to decode\n");
			av_free_packet (&avpkt);
			break;
		}

		if (out_size > 0)
		{
			/* if a frame has been decoded, output it */
			slimaudio_buffer_write(audio->output_buffer, (char*)outbuf, out_size);
		}

		av_free_packet (&avpkt);
	}

	if ( inbuf != NULL )
		av_free(inbuf);

	if ( outbuf != NULL )
		av_free(outbuf);

	DEBUGF ("aac: avcodec_close\n");
	avcodec_close(pCodecCtx);

	/* Close the stream */
	DEBUGF ("aac: av_close_input_stream\n");
	av_close_input_stream(pFormatCtx);

	return 0;
}

#endif /* AAC_DECODER */
