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

#ifdef WMA_DECODER

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

void av_err_callback(void *ptr, int level, const char *fmt, va_list vargs)
{
	fprintf(stderr, fmt, vargs);
}

int slimaudio_decoder_wma_init(slimaudio_t *audio) {

	/* Setup error message capture */
	av_log_set_callback(av_err_callback);
	av_log_set_level(AV_LOG_VERBOSE);

	/* Register all the codecs */
	av_register_all();
	DEBUGF("wma: av_register_all\n");

        AVInputFormat *p = NULL;
	p = av_iformat_next(p);
        while (p)
	{
                VDEBUGF("wma: %s: %s:\n", p->name, p->long_name);
		p = av_iformat_next(p);
	};

	VDEBUGF("wma: %s\n", avformat_configuration() );

	return 0;
}

void slimaudio_decoder_wma_free(slimaudio_t *audio) {
}

int av_read_data(void *opaque, char *buffer, int buf_size)
{
	slimaudio_t *audio = (slimaudio_t *) opaque;

	pthread_mutex_lock(&audio->decoder_mutex);

	VDEBUGF("av_read_data state=%i\n", audio->decoder_state);
	if (audio->decoder_state != STREAM_PLAYING) {
		pthread_mutex_unlock(&audio->decoder_mutex);
		DEBUGF("slimaudio_decoder_wma_process: STREAM_NOT_PLAYING\n");
		return -1;
        }

	pthread_mutex_unlock(&audio->decoder_mutex);

        if (audio->decoder_end_of_stream) {
                audio->decoder_end_of_stream = false;
        }

        int data_len = buf_size;
	VDEBUGF("wma: read ask: %d\n", data_len);
        slimaudio_buffer_status ok = slimaudio_buffer_read(audio->decoder_buffer, (char*) buffer, &data_len);
	VDEBUGF("wma: read actual: %d\n", data_len);

        if (ok == SLIMAUDIO_BUFFER_STREAM_END) {
                DEBUGF("slimaudio_decoder_wma_process: EOS\n");
                audio->decoder_end_of_stream = true;
        }

	return data_len;
}

int slimaudio_decoder_wma_process(slimaudio_t *audio) {
//	unsigned char data[AUDIO_CHUNK_SIZE];
//	int buffer[AUDIO_CHUNK_SIZE/2];
//	int i;
	
//	unsigned char *ptr = data;
	char streamformat[] = "asf";
	int out_size;
	int len = 0;
	u8_t *outbuf;
	u8_t *inbuf;

        /* Check WMA metadata to see if this remote stream is being served from a
	* Windows Media server or a normal HTTP server.  WM servers will use MMS chunking
	* and need a pcmsamplesize value of 1, whereas HTTP servers need pcmsamplesize of 0.
	* 0 = asf - default
	* 1 = mms
	*/
	/* FIXME: Use wma_chunking in av_find_input_format call */

	DEBUGF("wma: wma_chunking:%d '%c' wma_playstream:%d '%c' wma_metadatastream:%d '%c'\n",
		audio->wma_chunking, audio->wma_chunking,
	       	audio->wma_playstream, audio->wma_playstream,
		audio->wma_metadatastream, audio->wma_metadatastream );

	/* FIXME: need lock? */
	/* Squeezebox server counts streams from 1, ffmpeg starts at 0.
	 * We need to revert the stream number back to a value which we
	 * change while reading the http headers and further subtract 1
	 */ 
	int audioStream = audio->wma_playstream-49;

	/* FIXME: change asf, or remove completely */
	if ( audio->wma_chunking == '1' )
	{
		strncpy ( streamformat, "asf", sizeof (streamformat) );
	}

	DEBUGF ("wma: play audioStream: %d\n", audioStream);

	AVFormatContext* pFormatCtx;
	AVInputFormat* pAVInputFormat = av_find_input_format(streamformat);
	if( !pAVInputFormat )
	{
		DEBUGF("wma: probe failed\n");
	}
	else
	{
		DEBUGF("wma: probe ok name:%s lname:%s\n", pAVInputFormat->name, pAVInputFormat->long_name);
		pAVInputFormat->flags |= AVFMT_NOFILE;
	}

	ByteIOContext ByteIOCtx;

	inbuf = av_malloc(AUDIO_INBUF_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);

	if( init_put_byte( &ByteIOCtx, inbuf, AUDIO_CHUNK_SIZE, 0, audio, av_read_data, NULL, NULL ) < 0)
	{
		DEBUGF("wma: init_put_byte failed\n");
	}

	ByteIOCtx.is_streamed = 1;

	int ires = av_open_input_stream(&pFormatCtx, &ByteIOCtx, "", pAVInputFormat, NULL);

	if (ires < 0)
	{
		DEBUGF("wma: input stream open failed:%d\n",ires);
	}

	if ( av_find_stream_info(pFormatCtx) < 0 )
	{
		DEBUGF("wma: find stream info failed.\n");
	}

	if ( pFormatCtx->nb_streams < audioStream )
		DEBUGF("wma: invalid stream.\n");

	if ( pFormatCtx->streams[audioStream]->codec->codec_type != CODEC_TYPE_AUDIO )
	{
		DEBUGF("wma: stream: %d is not audio.\n", audioStream );
	}

	AVCodecContext *pCodecCtx;

	pCodecCtx = pFormatCtx->streams[audioStream]->codec;

	AVCodec *pCodec;

	/* Find the WMA audio decoder */
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if (pCodec == NULL )
	{
		DEBUGF("wma: codec not found.\n");
	} 
	
	/* Open codec */
	if (avcodec_open(pCodecCtx, pCodec) < 0)
	{
		DEBUGF("wma: could not open codec\n");
	}

	int iRC;
	bool eos = false;
	AVPacket avpkt;

	outbuf = av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);

	while ( ! eos )
	{
		iRC = av_read_frame (pFormatCtx, &avpkt);

		/* Some decoders fail to read the last packet so additional handling is required */
	        if (iRC < 0)
		{
			DEBUGF("wma: av_read_frame error: %d\n", iRC);
			if ( (iRC == AVERROR_EOF) && audio->decoder_end_of_stream )
			{
				DEBUGF("wma: AVERROR_EOF\n");
				eos=true;
			}
#if 0
			if ( url_feof(pFormatCtx->pb) )
			{
				DEBUGF("wma: url_feof\n");
				eos=true;
			}
			if ( url_ferror(pFormatCtx->pb) )
			{
				DEBUGF("wma: url_ferror\n");
		                break;
			}
#endif
		}

		if ( avpkt.stream_index == audioStream )
		{
			out_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
			len = avcodec_decode_audio3(pCodecCtx, (int16_t *)outbuf, &out_size, &avpkt);
			if (len < 0)
			{
				DEBUGF("wma: no audio to decode\n");
				av_free_packet (&avpkt);
				break;
			}

			if (out_size > 0)
			{
				/* if a frame has been decoded, output it */
				slimaudio_buffer_write(audio->output_buffer, (char*)outbuf, out_size);
			}
		}
		else
		{
			av_free_packet (&avpkt);
		}
	}

	if ( inbuf != NULL )
		av_free(inbuf);

	if ( outbuf != NULL )
		av_free(outbuf);

	DEBUGF ("wma: avcodec_close\n");
	avcodec_close(pCodecCtx);

	/* Close the stream */
	DEBUGF ("wma: av_close_input_stream\n");
	av_close_input_stream(pFormatCtx);

	return 0;
}

#endif /* WMA_DECODER */
