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


#ifndef _SLIMPROTO_H_
#define _SLIMPROTO_H_

#include <stdbool.h>
#include <pthread.h>

#ifdef __WIN32__
  #include <winsock2.h>
  #include <sys/time.h>
#else
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/time.h>
#endif

typedef unsigned char u8_t;
typedef unsigned short u16_t;
typedef unsigned int u32_t;
typedef unsigned long long u64_t;
	
typedef union {	
	struct {
		u16_t length;
		char cmd[4];		
		u8_t command;		/* [1]	's' = start, 'p' = pause, 'u' = unpause, 'q' = stop, 't' = status */
		u8_t autostart;		/* [1]	'0' = don't auto-start, '1' = auto-start, '2' = direct streaming */
		u8_t mode;		/* [1]	'm' = mpeg bitstream, 'p' = PCM */
		u8_t pcm_sample_size;	/* [1]	'0' = 8, '1' = 16, '2' = 24, '3' = 32 */
		u8_t pcm_sample_rate;	/* [1]	'0' = 11kHz, '1' = 22, '2' = 32, '3' = 44.1, '4' = 48 */
		u8_t pcm_channels;	/* [1]	'1' = mono, '2' = stereo */
		u8_t pcm_endianness;	/* [1]	'0' = big, '1' = little */
		u8_t threshold;		/* [1]	Kb of input buffer data before we autostart or */
	       				/*	notify the server of buffer fullness */
		u8_t spdif_enable;	/* [1]  '0' = auto, '1' = on, '2' = off */
		u8_t transition_period;	/* [1]	seconds over which transition should happen */
		u8_t transition_type;	/* [1]	'0' = none, '1' = crossfade, '2' = fade in, '3' = fade out, */
	       				/*	'4' fade in & fade out */
		u8_t flags;		/* [1]	0x80 - loop infinitely, 0x40 - stream without restarting decoder, */
	       				/*	0x01 - polarity inversion left. 0x02 - polarity inversion right */
		u8_t output_threshold;	/* [1]	amount of output buffer data before playback starts, */
					/*	in tenths of second */
		u8_t reserved;		/* [1]	reserved */
		u32_t replay_gain;	/* [4]	replay gain in 16.16 fixed point, 0 means none */
		u16_t server_port;	/* [2]	server's port */
		u32_t server_ip;	/* [4]	server's IP */
		unsigned char http_hdr[1024];	/* HTTP headers from here */
	} strm;
	
	struct {
		u16_t length;
		char cmd[4];		
		u32_t old_left_gain;
		u32_t old_right_gain;
		u8_t digital_volume_control;
		u8_t preamp;
		u32_t left_gain;
		u32_t right_gain;
	} audg;

	struct {
		u16_t length;
		char cmd[4];
		u32_t version; /* Version encoded in 3 bytes.  E.g. 6.5.4 is 0x00060504 */
	} vers;
} slimproto_msg_t;


typedef enum { PROTO_QUIT=0, PROTO_CLOSED, PROTO_CONNECT, PROTO_CONNECTED, PROTO_CLOSE } slimproto_state_t;

typedef struct slimproto slimproto_t;


typedef int (slimproto_command_callback_t)(slimproto_t *p, const unsigned char *buf, int buf_len, void *user_data);
typedef int (slimproto_connect_callback_t)(slimproto_t *p, bool isConnected, void *user_data);


struct slimproto {
	slimproto_state_t state;
	
	int sockfd; 				/* Squeezebox Server socket */
	struct sockaddr_in serv_addr;		/* Squeezebox Server address */
	
	struct timeval epoch;
	
	int num_connect_callbacks;		/* cmd callbacks */
	struct {
		char *cmd;
		slimproto_connect_callback_t *callback;
		void *user_data;
	} connect_callbacks[20];
	
	int num_command_callbacks;		/* cmd callbacks */
	struct {
		char *cmd;
		slimproto_command_callback_t *callback;
		void *user_data;
	} command_callbacks[20];
	
	pthread_t slimproto_thread;
	pthread_mutex_t slimproto_mutex;
	pthread_cond_t slimproto_cond;
};




extern bool slimproto_debug;

int slimproto_init(slimproto_t *p);

void slimproto_destroy(slimproto_t *p);

void slimproto_add_command_callback(slimproto_t *p, const char *cmd, slimproto_command_callback_t *callback, void *user_data);

void slimproto_add_connect_callback(slimproto_t *p, slimproto_connect_callback_t *callback, void *user_data);

#ifdef __WIN32__
const char * inet_ntop(int, const void *, char *, size_t);
#endif

int slimproto_discover(char *server_addr, int server_addr_len, int port, unsigned int *jsonport, bool scan);

int slimproto_connect(slimproto_t *p, const char *server_addr, int port);

int slimproto_close(slimproto_t *p);

int slimproto_send(slimproto_t *p, unsigned char *msg);

void slimproto_parse_command(const unsigned char *buf, int buf_len, slimproto_msg_t *msg);

u32_t slimproto_set_jiffies(slimproto_t *p, unsigned char *buf, int jiffies_ptr);

/*
 * The following functions are used to send slimproto messages
 */
int slimproto_helo(slimproto_t *p, char device_id, char revision, const char *macaddress, char isGraphics, char isReconnect);

int slimproto_dsco(slimproto_t *, int);

int slimproto_goodbye(slimproto_t *, u8_t);

int slimproto_ir(slimproto_t *p, int format, int noBits, int irCode);

int slimproto_stat(slimproto_t *, const char *, int, int, u64_t, int, int, u32_t, u32_t);

int slimproto_configure_socket(int sockfd, int socktimeout);

int send_message(int sockfd, unsigned char* msg, size_t msglen, int msgflags);

/* This function configures the socket whose fd is passed in, in order
** to disable the raise of SIGPIPE when writing to a closed socket.
**
** Depending on the current platform, this may do nothing, so it is
** important that the flags passed to 'send' (when used directly) also
** include the flags returned by slimproto_get_socketsendflags (see
** below).  Note that for certain platforms there is no other way than
** to completely ignore SIGPIPE, which causes the whole process to
** ignore it.
*/
int slimproto_configure_socket_sigpipe(int sockfd);

/* This function returns the flags to be passed to the 'send' system
** call in order to disable the raise of SIGPIPE in case the socket is
** already closed.  We prefer to receive a EPIPE error, which can be
** handled like any other error instead of exiting the process.
*/
int slimproto_get_socketsendflags();

#define SLIMPROTO_MSG_SIZE	4096

#define DSCO_CLOSED		 	0
#define DSCO_RESET_LOCAL 		1
#define DSCO_RESET_REMOTE 		2
#define DSCO_UNREACHABLE 		3
#define DSCO_TIMEOUT 			4

#endif /*_SLIMPROTO_H_ */
