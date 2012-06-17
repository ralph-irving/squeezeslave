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
#include <string.h>
#include <pthread.h>
#include <sched.h>

#ifdef __WIN32__
  #include <winsock2.h>
  typedef SOCKET socket_t;
  #define CLOSESOCKET(s) closesocket(s)
  #define SOCKETERROR WSAGetLastError()
#else
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <errno.h>
  typedef int socket_t;
  #define CLOSESOCKET(s) close(s)
  #define SOCKETERROR errno
#endif

#include "slimaudio/slimaudio.h"

#define HTTP_HEADER_LENGTH 1024

extern bool threshold_override;

#ifdef SLIMPROTO_DEBUG
  bool slimaudio_http_debug;
  bool slimaudio_http_debug_v;
  #define DEBUGF(...) if (slimaudio_http_debug) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...) if (slimaudio_http_debug_v) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
  #define VDEBUGF(...)
#endif

static void *http_thread(void *ptr);
static void http_recv(slimaudio_t *a);
static void http_close(slimaudio_t *a);

int slimaudio_http_open(slimaudio_t *audio) {
	pthread_mutex_init(&(audio->http_mutex), NULL);
	pthread_cond_init(&(audio->http_cond), NULL);
	/* 
	 * We lock the mutex right here, knowing that http_thread will
	 * release it once it enters pthread_cond_wait inside its STOPPED
	 * state.
	 */
#ifndef BSD_THREAD_LOCKING
	pthread_mutex_lock(&audio->http_mutex);
#endif

	if (pthread_create( &audio->http_thread, NULL, http_thread, (void*) audio) != 0) {
		fprintf(stderr, "Error creating http thread\n");
#ifndef BSD_THREAD_LOCKING	
		pthread_mutex_unlock(&audio->http_mutex);
#endif		
		return -1;
	}

	return 0;
}


int slimaudio_http_close(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->http_mutex);

	audio->http_state = STREAM_QUIT;	

	pthread_mutex_unlock(&audio->http_mutex);
	
	pthread_cond_broadcast(&audio->http_cond);
	
	pthread_join(audio->http_thread, NULL);	

	pthread_mutex_destroy(&(audio->http_mutex));
	pthread_cond_destroy(&(audio->http_cond));
	return 0;
}


static void *http_thread(void *ptr) {
	slimaudio_t *audio = (slimaudio_t *) ptr;
#ifdef SLIMPROTO_DEBUG				
	int last_state = 0;
#endif
#ifdef RENICE
	if ( renice )
		if ( renice_thread (5) ) /* Lower thread priority to give precedence to the decoder */
			fprintf(stderr, "http_thread: renice failed.\n");
#endif
#ifdef BSD_THREAD_LOCKING
	pthread_mutex_lock(&audio->http_mutex);
#endif
	audio->http_state = STREAM_STOPPED;
	
	while ( audio->http_state != STREAM_QUIT )
	{
#ifdef SLIMPROTO_DEBUG				
		if (last_state == audio->http_state) {
			VDEBUGF("http_thread state %i\n", audio->http_state);
		}
		else {
			DEBUGF("http_thread state %i\n", audio->http_state);
		}
			
		last_state = audio->http_state;
#endif

		switch (audio->http_state) {
			case STREAM_STOP:
				CLOSESOCKET(audio->streamfd);
				slimproto_dsco(audio->proto, DSCO_CLOSED);

				slimaudio_buffer_close(audio->decoder_buffer);
				
				audio->http_state = STREAM_STOPPED;

				pthread_cond_broadcast(&audio->http_cond);

				break;
				
			case STREAM_STOPPED:
				pthread_cond_wait(&audio->http_cond, &audio->http_mutex);
				break;
			
			case STREAM_PLAYING:
				pthread_mutex_unlock(&audio->http_mutex);
				
				http_recv(audio);
				
				pthread_mutex_lock(&audio->http_mutex);

				break;
				
			case STREAM_QUIT:
				break;
		}
	}

	pthread_mutex_unlock(&audio->http_mutex);

	return 0;
}


void slimaudio_http_connect(slimaudio_t *audio, slimproto_msg_t *msg) {
	int n;
	struct sockaddr_in serv_addr = audio->proto->serv_addr;
	const socket_t fd = socket(AF_INET, SOCK_STREAM, 0);

	char http_hdr[HTTP_HEADER_LENGTH];
	int pos = 0;
	int crlf = 0;

	slimaudio_http_disconnect(audio);
	
	if (msg->strm.server_ip != 0) {
		serv_addr.sin_addr.s_addr = htonl(msg->strm.server_ip);
	}
	if (msg->strm.server_port != 0) {
		serv_addr.sin_port = htons(msg->strm.server_port);
	}
	
	DEBUGF("slimaudio_http_connect: http connect %s:%i\n", 
	       inet_ntoa(serv_addr.sin_addr), msg->strm.server_port);
	
	if (fd < 0) {
		perror("slimaudio_http_connect: Error opening socket");
		return;
	}

        if ( slimproto_configure_socket (fd, 0) != 0 )
        {
		perror("slimaudio_http_connect: error configuring socket");
                CLOSESOCKET(fd);
                return;
        }

	if (connect(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
		perror("slimaudio_http_connect: error connecting to server");
		CLOSESOCKET(fd);
		return;
	}

	slimaudio_stat(audio, "STMe", (u32_t) 0); /* Stream connection established */

	/* send http request to server */
	DEBUGF("slimaudio_http_connect: http request %s\n", msg->strm.http_hdr);

	n = send_message(fd, msg->strm.http_hdr, strlen((const char *)msg->strm.http_hdr),
		slimproto_get_socketsendflags());

	if (n < 0)
	{
		DEBUGF("http_send: (1) n=%i  msg=%s(%i)\n", n, strerror(SOCKETERROR), SOCKETERROR);
		CLOSESOCKET(fd);
		return;
	}

	/* read http header */
	
	do {
		n = recv(fd, http_hdr+pos, 1, 0);
		if (n < 0)
		{
			DEBUGF("http_recv: (1) n=%i  msg=%s(%i)\n", n, strerror(SOCKETERROR), SOCKETERROR);
			CLOSESOCKET(fd);
			return;
		}

		switch (crlf) {
			case 0:	
			case 2:	
				if (http_hdr[pos] == 13) {
					crlf++;
				}
				else {
					crlf = 0;
				}
				break;
			case 1:
			case 3:
				if (http_hdr[pos] == 10) {
					crlf++;
				}
				else {
					crlf = 0;
				}
				break;
			default:
				crlf = 0;
				break;			
		}
		
		pos++;
	} while (crlf < 4 && pos < HTTP_HEADER_LENGTH -1);
	http_hdr[pos+1] = '\0';
		
	DEBUGF("slimaudio_http_connect: http connected hdr %s\n", http_hdr);
	
	pthread_mutex_lock(&audio->http_mutex);
	slimaudio_stat(audio, "STMh", (u32_t) 0); /* acknowledge HTTP headers have been received */

	slimaudio_buffer_open(audio->decoder_buffer, NULL);	
	
	audio->streamfd = fd;
	audio->http_stream_bytes = 0;
	audio->autostart_mode = msg->strm.autostart ;
	audio->autostart_threshold_reached = false;
	audio->autostart_threshold = (msg->strm.threshold & 0xFF) * 1024;

#ifdef AAC_DECODER
	/* AAC container type and bitstream format */
	audio->aac_format = msg->strm.pcm_sample_size ;
#endif
#ifdef WMA_DECODER
	/* WMA stream details */
	audio->wma_chunking = msg->strm.pcm_sample_size ;
	audio->wma_playstream = msg->strm.pcm_sample_rate + 48 ; /* Squeezebox.pm doesn't use char for this field */
	audio->wma_metadatastream =  msg->strm.pcm_channels ;
#endif

	DEBUGF("slimaudio_http_connect: pcm_sample_size:%d '%c' pcm_sample_rate:%d '%c' pcm_channels:%d '%c'\n",
		msg->strm.pcm_sample_size, msg->strm.pcm_sample_size,
		msg->strm.pcm_sample_rate, msg->strm.pcm_sample_rate,
		msg->strm.pcm_channels, msg->strm.pcm_channels);

	/* XXX FIXME Hard coded sample rate calculation */
	/* (Sample Rate * Sample Size * Channels / 8 bits/byte) / tenths of a second) */
	audio->output_threshold = (((44100*16*2)/8)/10) * msg->strm.output_threshold; /* Stored in bytes */

	DEBUGF("slimaudio_http_connect: autostart_mode=%c autostart_threshold=%i output_threshold=%i replay_gain=%f\n",
		audio->autostart_mode, audio->autostart_threshold, audio->output_threshold, audio->replay_gain);
	
	audio->http_state = STREAM_PLAYING;

	pthread_mutex_unlock(&audio->http_mutex);	

	pthread_cond_broadcast(&audio->http_cond);
}

void slimaudio_http_disconnect(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->http_mutex);

	if (audio->http_state == STREAM_PLAYING)
	{
		DEBUGF("slimaudio_http_disconnect: state=%i\n", audio->http_state);

		audio->http_state = STREAM_STOP;

		/* closing socket and buffer will wake the http thread */
		CLOSESOCKET(audio->streamfd);
		slimaudio_buffer_close(audio->decoder_buffer);

		pthread_cond_broadcast(&audio->http_cond);

		while (audio->http_state == STREAM_STOP)
		{
			pthread_cond_wait(&audio->http_cond, &audio->http_mutex);
		}
	}

	pthread_mutex_unlock(&audio->http_mutex);
}

static void http_recv(slimaudio_t *audio) {
	char buf[AUDIO_CHUNK_SIZE];
	struct timeval timeOut; 
	int n;
	
	fd_set fdread;
	u32_t decode_num_tracks_started;
	u32_t autostart_threshold;

	timeOut.tv_sec  = 0; 
	timeOut.tv_usec = 100*1000; /* wait for up to 100ms */

	FD_ZERO(&fdread); 
	FD_SET(audio->streamfd, &fdread);  

	if (select(audio->streamfd + 1, &fdread, NULL, &fdread, &timeOut) == 0)
	{
		return;
	}

	while (slimaudio_buffer_available(audio->output_buffer) < AUDIO_CHUNK_SIZE * 2 &&
		slimaudio_buffer_available(audio->decoder_buffer) >= AUDIO_CHUNK_SIZE * 8)
	{
		DEBUGF("http_recv: output_buffer %i below %i\n",
				slimaudio_buffer_available(audio->output_buffer), AUDIO_CHUNK_SIZE * 2);
		DEBUGF("http_recv: output_decoder_available %i above %i\n",
				slimaudio_buffer_available(audio->decoder_buffer), AUDIO_CHUNK_SIZE * 8);
		sched_yield();
	}

	n = recv(audio->streamfd, buf, AUDIO_CHUNK_SIZE, 0);

	/* n == 0 http stream closed by server */
	if (n <= 0)
	{
		DEBUGF("http_recv: (2) n=%i msg=%s(%i)\n", n, strerror(SOCKETERROR), SOCKETERROR);
		http_close(audio);
		return;
	}

	VDEBUGF("http_recv: audio n=%i\n", n);

	slimaudio_buffer_write(audio->decoder_buffer, buf, n);

	pthread_mutex_lock(&audio->output_mutex);
	decode_num_tracks_started = audio->decode_num_tracks_started;
	pthread_mutex_unlock(&audio->output_mutex);

	pthread_mutex_lock(&audio->http_mutex);
	
	audio->http_total_bytes += n;
	audio->http_stream_bytes += n;

	autostart_threshold = audio->autostart_threshold;

	if ( !decode_num_tracks_started )
	{
		switch ( audio->autostart_mode )
		{
			case '1': /* Modify threshold for autostart modes, and not sync modes */
			case '3':
				switch (audio->decoder_mode)
				{
					case 'o':
					case 'm':
						if (threshold_override)
							autostart_threshold = 40000L;
						break;
					default:
						break;
				}
				break;
			default:
				break;
		}
	}

	VDEBUGF("http_recv: decode_num_tracks_started %u decode_bytes_available %u\n",
		decode_num_tracks_started, audio->http_stream_bytes );

	if ( !audio->autostart_threshold_reached && ( audio->http_stream_bytes >= autostart_threshold ))
	{
		audio->autostart_threshold_reached = true;

		switch ( audio->autostart_mode )
		{
			case '0':
			case '2':
				DEBUGF("http_recv: AUTOSTART mode %c at %u threshold %u\n",
					audio->autostart_mode, audio->http_stream_bytes, autostart_threshold);

				slimaudio_stat(audio, "STMl", (u32_t) 0);

				pthread_mutex_unlock(&audio->http_mutex);
				pthread_cond_broadcast(&audio->http_cond);

				break;

			case '1':
			case '3':
				DEBUGF("http_recv: AUTOSTART mode %c at %u threshold %u\n",
					audio->autostart_mode, audio->http_stream_bytes, autostart_threshold);

				pthread_mutex_unlock(&audio->http_mutex);
				pthread_cond_broadcast(&audio->http_cond);

				slimaudio_output_unpause(audio);
				break;

			default:
				break;
		}


	}
	else
	{
		pthread_mutex_unlock(&audio->http_mutex);
		pthread_cond_broadcast(&audio->http_cond);
	}
}

static void http_close(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->http_mutex);
	
	audio->http_state = STREAM_STOP;		
	
	pthread_mutex_unlock(&audio->http_mutex);
	
	pthread_cond_broadcast(&audio->http_cond);				
}

