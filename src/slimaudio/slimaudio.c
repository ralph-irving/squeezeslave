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
#include <strings.h>
#include <sys/types.h>

#ifdef __WIN32__
  #include <winsock.h>
#else
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <string.h>
#endif


#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"


#ifdef SLIMPROTO_DEBUG
  bool slimaudio_debug;
  #define DEBUGF(...) if (slimaudio_debug) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
#endif

static int strm_callback(slimproto_t *p, const unsigned char *buf, int buf_len, void *user_data);
static int vers_callback(slimproto_t *p, const unsigned char *buf, int buf_len, void *user_data);
static int connect_callback(slimproto_t *p, bool isConnected, void *user_data);
static void audio_stop(slimaudio_t *audio);


/*
 * Init the slimaudio layer:
 * - allocate the internal state
 * - register callbacks with slimproto
 */
int slimaudio_init(slimaudio_t *audio, slimproto_t *proto) {
	memset(audio, 0, sizeof(slimaudio_t));
	
	audio->proto = proto;
	audio->decoder_buffer = slimaudio_buffer_init(DECODER_BUFFER_SIZE);
	audio->output_buffer = slimaudio_buffer_init(OUTPUT_BUFFER_SIZE);
	
	if (slimaudio_output_init(audio) != 0)
		return -1;
	
	slimproto_add_command_callback(proto, "strm", &strm_callback, audio);
	slimproto_add_command_callback(proto, "vers", &vers_callback, audio);
	slimproto_add_connect_callback(proto, connect_callback, audio);
 	return 0;
}

/*
 * Frees slimaudio resources.
 */
void slimaudio_destroy(slimaudio_t *audio) {
	// FIXME remove slimproto callback		
	slimaudio_output_destroy(audio);	
	slimaudio_buffer_close(audio->output_buffer);
	slimaudio_buffer_free(audio->output_buffer);
	audio->output_buffer = NULL;
	slimaudio_buffer_close(audio->decoder_buffer);
	slimaudio_buffer_free(audio->decoder_buffer);
	audio->decoder_buffer = NULL;
}

/*
 * Open audio resources
 */
int slimaudio_open(slimaudio_t *audio) {
	if (slimaudio_http_open(audio) != 0)
		return -1;		
	if (slimaudio_decoder_open(audio) != 0)
		return -1;
	if (slimaudio_output_open(audio) != 0)
		return -1;
		
	return 0;	
}

void slimaudio_set_keepalive_interval(slimaudio_t *audio, int seconds) {
	pthread_mutex_lock(&audio->output_mutex);
	audio->keepalive_interval = seconds;
	pthread_cond_broadcast(&audio->output_cond);
	pthread_mutex_unlock(&audio->output_mutex);
}

void slimaudio_set_volume_control(slimaudio_t *audio, slimaudio_volume_t vol) {
	pthread_mutex_lock(&audio->output_mutex);
	audio->volume_control = vol;
	pthread_cond_broadcast(&audio->output_cond);
	pthread_mutex_unlock(&audio->output_mutex);
}

void slimaudio_set_output_predelay(slimaudio_t *audio, unsigned int msec, unsigned int amplitude) {
	pthread_mutex_lock(&audio->output_mutex);
	audio->output_predelay_msec = msec;
	audio->output_predelay_amplitude = amplitude;
	pthread_cond_broadcast(&audio->output_cond);
	pthread_mutex_unlock(&audio->output_mutex);
}

/*
 * Close audio resources
 */
int slimaudio_close(slimaudio_t *audio) {
	audio_stop(audio);
	
	if (slimaudio_http_close(audio) != 0)
		return -1;
	if (slimaudio_decoder_close(audio) != 0)
		return -1;
	if (slimaudio_output_close(audio) != 0)
		return -1;
		
	return 0;
}

int slimaudio_stat(slimaudio_t *audio, const char *code, u32_t interval) {
	int decoder_available = slimaudio_buffer_available(audio->decoder_buffer);
	int output_available = slimaudio_buffer_available(audio->output_buffer);
        u32_t msec =
                (u32_t) ((audio->stream_samples - audio->pa_streamtime_offset) / 44.100)
			+ audio->output_predelay_msec;

	return slimproto_stat(audio->proto, code, DECODER_BUFFER_SIZE, decoder_available,
			audio->http_total_bytes, OUTPUT_BUFFER_SIZE, output_available,
			msec < 0 ? 0 : msec, interval );
}

/*
 * Callback for 'strm' command.
 */
static int strm_callback(slimproto_t *proto, const unsigned char *buf, int buf_len, void *user_data) {
	slimproto_msg_t msg;
	float replay_gain;	
	slimaudio_t *audio = (slimaudio_t *) user_data;
	slimproto_parse_command(buf, buf_len, &msg);

	DEBUGF("strm cmd %c strm.replay_gain:%u ", msg.strm.command, msg.strm.replay_gain);

	switch (msg.strm.command) {
		case 's': /* start */
			replay_gain = (float) (msg.strm.replay_gain) / 65536.0;
			audio->start_replay_gain = replay_gain == 0.0 ? 1.0 : replay_gain;

			if (audio->replay_gain == -1.0)
				audio->replay_gain = audio->start_replay_gain;

			DEBUGF("start_replay_gain:%f\n", audio->start_replay_gain);

			slimaudio_stat(audio, "STMc", (u32_t) 0); /* connect, acknowledge strm-s */

			slimaudio_http_connect(audio, &msg);
			slimaudio_decoder_connect(audio, &msg);
			slimaudio_output_connect(audio, &msg);
			break;
			
		case 'p': /* pause */
			DEBUGF("\n");
			slimaudio_output_pause(audio);

			/* Only send STMp if interval is zero */
			if (! msg.strm.replay_gain)
				slimaudio_stat(audio, "STMp", (u32_t) 0); /* pause */
			break;	
		
		case 'u': /* unpause */
			DEBUGF("\n");
			slimaudio_output_unpause(audio);
			slimaudio_stat(audio, "STMr", (u32_t) 0); /* resume */
			break;	
		
		case 'q': /* stop */
			DEBUGF("\n");
			audio_stop(audio);
			slimaudio_stat(audio, "STMf", (u32_t) 0); /* acknowledge stop cmd */
			break;	
		
		case 'f': /* flush */
			DEBUGF("\n");
			slimaudio_buffer_flush(audio->decoder_buffer);
			slimaudio_buffer_flush(audio->output_buffer);
			slimaudio_stat(audio, "STMf", (u32_t) 0); /* acknowledge flush cmd */
			break;			

		case 'a': /* skip ahead */
			DEBUGF("\n");
			break;

		case 't': /* status */
			DEBUGF("\n");
			slimaudio_stat(audio, "STMt", msg.strm.replay_gain);
			break;			
	}
	
	DEBUGF("DONE strm cmd %c\n", msg.strm.command);
	return 0;
}

// Called by slimproto when the "vers" command is received from the
// server to signal the version.
static int vers_callback(slimproto_t *p, const unsigned char *buf,
			 int buf_len, void *user_data) {

	slimaudio_t* const audio = (slimaudio_t*)user_data;

	if (audio->keepalive_interval != -1) {
		// Keepalive interval has been set explicitly, don't overwrite.
		DEBUGF("Explicit keepalive interval: %d s.  "
		       "Will not override.\n", audio->keepalive_interval);
		return 0;
	}

	slimproto_msg_t msg;
	slimproto_parse_command(buf, buf_len, &msg);
	DEBUGF("Server version: %x\n", msg.vers.version);

	if ((msg.vers.version >= 0x00060500) && (msg.vers.version < 0x00070200)) {
		// 6.5.0<->7.2.0 Squeezebox Server needs a player to send
		// a keepalive message every 10 seconds or so before it declares
		// it down.
		slimaudio_set_keepalive_interval(audio, 10);
		DEBUGF("Using 6.5.x default keepalive interval: %d s.\n", 
		       audio->keepalive_interval);
	}

	return 0;
}

static void audio_stop(slimaudio_t *audio) {
	if (slimaudio_output_disconnect(audio) == -1) {
		DEBUGF("audio_stop early out.\n");
		return;
	}

	/*
	 * To prevent deadlocks we must stop the decoder and http 
	 * stream together.
	 */
	pthread_mutex_lock(&audio->decoder_mutex);
	pthread_mutex_lock(&audio->http_mutex);
	
	audio->decoder_state = STREAM_STOP;
	audio->http_state = STREAM_STOP;
	
	pthread_cond_broadcast(&audio->decoder_cond);
	pthread_cond_broadcast(&audio->http_cond);
	
	slimaudio_buffer_flush(audio->output_buffer);
	slimaudio_buffer_flush(audio->decoder_buffer);

	while (audio->decoder_state != STREAM_STOPPED) {
		pthread_cond_wait(&audio->decoder_cond, &audio->decoder_mutex);
	}
	while (audio->http_state != STREAM_STOPPED) {
		pthread_cond_wait(&audio->http_cond, &audio->http_mutex);
	}
	
	pthread_mutex_unlock(&audio->http_mutex);	
	pthread_mutex_unlock(&audio->decoder_mutex);	
}

static int connect_callback(slimproto_t *p, bool isConnected, void *user_data) {
	if (!isConnected) {
		DEBUGF("Stopping audio because of disconnection.\n");
		slimaudio_t *audio = (slimaudio_t*)user_data;
		audio_stop(audio);
	}
	
	return 0;
}
