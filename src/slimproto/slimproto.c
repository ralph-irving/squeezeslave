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

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef __WIN32__
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include "poll.h"
  #define CLOSESOCKET(s) closesocket(s)
  #define MSG_DONTWAIT (0)
#else
  #include <sys/poll.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h> 
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <errno.h>
  #define CLOSESOCKET(s) close(s)
#endif

#include "slimproto/slimproto.h"

#define BUF_LENGTH 4096

#ifdef SLIMPROTO_DEBUG
  bool slimproto_debug;
  #define DEBUGF(...) if (slimproto_debug) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...)
#else
  #define DEBUGF(...)
  #define VDEBUGF(...)
#endif

#define packN4(ptr, off, v) { ptr[off] = (char)(v >> 24) & 0xFF; ptr[off+1] = (v >> 16) & 0xFF; ptr[off+2] = (v >> 8) & 0xFF; ptr[off+3] = v & 0xFF; }
#define packN2(ptr, off, v) { ptr[off] = (char)(v >> 8) & 0xFF; ptr[off+1] = v & 0xFF; }
#define packC(ptr, off, v) { ptr[off] = v & 0xFF; }
#define packA4(ptr, off, v) { strncpy((char*)(&ptr[off]), v, 4); }

#define unpackN4(ptr, off) ((ptr[off] << 24) | (ptr[off+1] << 16) | (ptr[off+2] << 8) | ptr[off+3])
#define unpackN2(ptr, off) ((ptr[off] << 8) | ptr[off+1])
#define unpackC(ptr, off) (ptr[off])

static void *proto_thread(void *ptr);
static int proto_connect(slimproto_t *p);
static int proto_recv(slimproto_t *p);

int slimproto_init(slimproto_t *p) {
	memset(p, 0, sizeof(slimproto_t));
#ifdef __WIN32__
	WSADATA info; 
	if (WSAStartup(MAKEWORD(1,1), &info) != 0) {
		fprintf(stderr, "Cannot initialize WinSock");
		return -1;
	}
#endif

	gettimeofday(&p->epoch, NULL);

	pthread_mutex_init(&(p->slimproto_mutex), NULL);
	pthread_cond_init(&(p->slimproto_cond), NULL);
	p->state = PROTO_CLOSED;
	
	if (pthread_create( &p->slimproto_thread, NULL, proto_thread, (void*) p) != 0) {
		fprintf(stderr, "Error creating proto thread\n");
		return -1;
	}	
	
	return 0;
}

void slimproto_destroy(slimproto_t *p) {
	pthread_mutex_lock(&p->slimproto_mutex);

	p->state = PROTO_QUIT;

	pthread_mutex_unlock(&p->slimproto_mutex);

	pthread_cond_broadcast(&p->slimproto_cond);

#ifdef __WIN32__
	WSACleanup();
#endif
	pthread_join(p->slimproto_thread, NULL);

	p->sockfd = -1;
	
	p->num_connect_callbacks = 0;
	p->num_command_callbacks = 0;
	
	pthread_mutex_destroy(&(p->slimproto_mutex));
	pthread_cond_destroy(&(p->slimproto_cond));
}

/* PROTO_QUIT=0, PROTO_CLOSED=1, PROTO_CONNECT=2, PROTO_CONNECTED=3, PROTO_CLOSE=4 */

static void *proto_thread(void *ptr) {
	int r, i;
	bool disconnected;
	
	slimproto_t *p = (slimproto_t *) ptr;

	pthread_mutex_lock(&p->slimproto_mutex);				

	while (p->state != PROTO_QUIT) {
		DEBUGF("proto_thread: state=%i\n", p->state);
		
		switch (p->state) {
			case PROTO_CONNECT:
				pthread_mutex_unlock(&p->slimproto_mutex);
				proto_connect(p);
				pthread_mutex_lock(&p->slimproto_mutex);				
				break;
				
			case PROTO_CONNECTED:
				pthread_mutex_unlock(&p->slimproto_mutex);
				for (i=0; i<p->num_connect_callbacks; i++) {
					(p->connect_callbacks[i].callback)(p, true, p->connect_callbacks[i].user_data);
				}

				while (proto_recv(p) >= 0) {
					pthread_mutex_lock(&p->slimproto_mutex);
					disconnected = p->state != PROTO_CONNECTED;
					pthread_mutex_unlock(&p->slimproto_mutex);
					if (disconnected) {
						DEBUGF("proto_thread: disconnected state:%i\n", p->state);
						break;
					}
				}

				slimproto_close(p);

				for (i=0; i<p->num_connect_callbacks; i++) {
					(p->connect_callbacks[i].callback)(p, false, p->connect_callbacks[i].user_data);
				}
				pthread_mutex_lock(&p->slimproto_mutex);				
				break;	
				
			default:
			case PROTO_CLOSED:
				DEBUGF("proto_thread: PROTO_CLOSED cond_wait\n");
				r = pthread_cond_wait(&p->slimproto_cond, &p->slimproto_mutex);
				break;				
				
			case PROTO_QUIT:
				break;
		}
		
	}

	pthread_mutex_unlock(&p->slimproto_mutex);	
	return 0;			
}

void slimproto_add_command_callback(slimproto_t *p, const char *cmd, slimproto_command_callback_t *callback, void *user_data) {
	int i;
	pthread_mutex_lock(&p->slimproto_mutex);				

	i = p->num_command_callbacks;
	p->command_callbacks[i].cmd = strdup(cmd);
	p->command_callbacks[i].callback = (void *) callback; /* FIXME */
	p->command_callbacks[i].user_data = user_data;
	p->num_command_callbacks++;

	pthread_mutex_unlock(&p->slimproto_mutex);
}

void slimproto_add_connect_callback(slimproto_t *p, slimproto_connect_callback_t *callback, void *user_data) {
	int i;
	pthread_mutex_lock(&p->slimproto_mutex);				

	i = p->num_connect_callbacks;
	p->connect_callbacks[i].callback = (void *) callback; /* FIXME */
	p->connect_callbacks[i].user_data = user_data;
	p->num_connect_callbacks++;

	pthread_mutex_unlock(&p->slimproto_mutex);	
}

int slimproto_configure_socket(int sockfd, int socktimeout)
{
	int retcode = 0;
	struct timeval timeout;
	int flag = 1;

#ifdef __WIN32__
	timeout.tv_sec = socktimeout * 1000;
#else
	timeout.tv_sec = socktimeout;
#endif
	timeout.tv_usec = 0;

	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(flag) ) != 0)
	{
		perror("Error setting TCP_NODELAY on socket");
		retcode = -1;
	}
	else
	{
#if !defined(__SUNPRO_C) && !defined(EMPEG)
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (void *)&timeout,  sizeof(timeout)))
	{
		perror("Error setting receive socket timeout");
		retcode = -1;
	}
	else
	{
	if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (void *)&timeout,  sizeof(timeout)))
	{
		perror("Error setting send socket timeout");
		retcode = -1;
	}
	else
	{
#endif /* !__SUNPRO_C && !EMPEG */
	if (slimproto_configure_socket_sigpipe(sockfd) != 0)
	{
		fprintf(stderr, "Couldn't configure socket for SIGPIPE.\n");
		retcode = -1;
	}
#if !defined(__SUNPRO_C) && !defined(EMPEG)
	}
	}
#endif /* !__SUNPRO_C && !EMPEG */
	}

	return (retcode);
}

#ifdef __WIN32__
#ifndef EAFNOSUPPORT
# define EAFNOSUPPORT EINVAL
#endif

#ifndef NS_INADDRSZ
# define NS_INADDRSZ      4
#endif
#ifndef NS_IN6ADDRSZ
# define NS_IN6ADDRSZ    16
#endif
#ifndef NS_INT16SZ 
# define NS_INT16SZ      2
#endif

/*
 * WARNING: Don't even consider trying to compile this on a system where
 * sizeof(int) < 4.  sizeof(int) > 4 is fine; all the world's not a VAX.
 */

static const char *inet_ntop4 (const u_char *src, char *dst, size_t size);
#if HAVE_IPV6
static const char *inet_ntop6 (const u_char *src, char *dst, size_t size);
#endif

/* char *
 * inet_ntop(af, src, dst, size)
 *	convert a network format address to presentation format.
 * return:
 *	pointer to presentation format address (`dst'), or NULL (see errno).
 * author:
 *	Paul Vixie, 1996.
 */
const char *
inet_ntop(af, src, dst, size)
int af;
const void *src;
char *dst;
size_t size;
{
    switch (af) {
    case AF_INET:
        return (inet_ntop4(src, dst, size));

#if HAVE_IPV6
    case AF_INET6:
        return (inet_ntop6(src, dst, size));
#endif

    default:
        errno = EAFNOSUPPORT;
        return (NULL);
    }
    /* NOTREACHED */
}

/* const char *
 * inet_ntop4(src, dst, size)
 *	format an IPv4 address
 * return:
 *	`dst' (as a const)
 * notes:
 *	(1) uses no statics
 *	(2) takes a u_char* not an in_addr as input
 * author:
 *	Paul Vixie, 1996.
 */
static const char *
inet_ntop4(src, dst, size)
const u_char *src;
char *dst;
size_t size;
{
    static const char fmt[] = "%u.%u.%u.%u";
    char tmp[sizeof "255.255.255.255"];

    if (snprintf(tmp, min(sizeof("255.255.255.255"),size), fmt, src[0], src[1], src[2], src[3]) >= size) {
        errno = ENOSPC;
        return (NULL);
    }
    strcpy(dst, tmp);
    return (dst);
}

#if HAVE_IPV6

/* const char *
 * inet_ntop6(src, dst, size)
 *	convert IPv6 binary address into presentation (printable) format
 * author:
 *	Paul Vixie, 1996.
 */
static const char *
inet_ntop6(src, dst, size)
const u_char *src;
char *dst;
size_t size;
{
    /*
     * Note that int32_t and int16_t need only be "at least" large enough
     * to contain a value of the specified size.  On some systems, like
     * Crays, there is no such thing as an integer variable with 16 bits.
     * Keep this in mind if you think this function should have been coded
     * to use pointer overlays.  All the world's not a VAX.
     */
    char tmp[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"], *tp;
    struct { int base, len; } best, cur;
    u_int words[NS_IN6ADDRSZ / NS_INT16SZ];
    int i;

    /*
     * Preprocess:
     *	Copy the input (bytewise) array into a wordwise array.
     *	Find the longest run of 0x00's in src[] for :: shorthanding.
     */
    memset(words, '\0', sizeof words);
    for (i = 0; i < NS_IN6ADDRSZ; i++)
        words[i / 2] |= (src[i] << ((1 - (i % 2)) << 3));
    best.base = -1;
    best.len = 0;
    cur.base = -1;
    cur.len = 0;
    for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
        if (words[i] == 0) {
            if (cur.base == -1)
                cur.base = i, cur.len = 1;
            else
                cur.len++;
        } else {
            if (cur.base != -1) {
                if (best.base == -1 || cur.len > best.len)
                    best = cur;
                cur.base = -1;
            }
        }
    }
    if (cur.base != -1) {
        if (best.base == -1 || cur.len > best.len)
            best = cur;
    }
    if (best.base != -1 && best.len < 2)
        best.base = -1;

    /*
     * Format the result.
     */
    tp = tmp;
    for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
        /* Are we inside the best run of 0x00's? */
        if (best.base != -1 && i >= best.base &&
                i < (best.base + best.len)) {
            if (i == best.base)
                *tp++ = ':';
            continue;
        }
        /* Are we following an initial run of 0x00s or any real hex? */
        if (i != 0)
            *tp++ = ':';
        /* Is this address an encapsulated IPv4? */
        if (i == 6 && best.base == 0 && (best.len == 6 ||
                                         (best.len == 7 && words[7] != 0x0001) ||
                                         (best.len == 5 && words[5] == 0xffff))) {
            if (!inet_ntop4(src+12, tp, sizeof tmp - (tp - tmp)))
                return (NULL);
            tp += strlen(tp);
            break;
        }
        tp += snprintf(tp, (tmp + sizeof("ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255") - tp), "%x", words[i]);
    }
    /* Was it a trailing run of 0x00's? */
    if (best.base != -1 && (best.base + best.len) ==
            (NS_IN6ADDRSZ / NS_INT16SZ))
        *tp++ = ':';
    *tp++ = '\0';

    /*
     * Check for overflow, copy, and we're done.
     */
    if ((size_t)(tp - tmp) > size) {
        errno = ENOSPC;
        return (NULL);
    }
    strcpy(dst, tmp);
    return (dst);
}
#endif
#endif /* __WIN32__ */


#define DISCOVERY_PKTSIZE	1516
#define SLIMPROTO_DISCOVERY	"eNAME\0JSON\0"

int slimproto_discover(char *server_addr, int server_addr_len, int port, unsigned int *jsonport, bool scan)
{
	int sockfd;
	int try;
	char *packet;
	int pktlen;
	int pktidx;
	char *t;
	unsigned int l;
	char *v;
	char *server_name;
	char *server_json;
	struct pollfd pollfd;
	struct sockaddr_in sendaddr;
	struct sockaddr_in recvaddr;
#ifdef __WIN32__
        WSADATA info;
#endif

	socklen_t sockaddr_len = sizeof(sendaddr);

	int broadcast=1;
	int serveraddr_len = -1;

#ifdef __WIN32__
	/* Need to initialize winsock if scanning on windows as slimproto_init has not been called */
	if ( scan )
	{
        	if (WSAStartup(MAKEWORD(1,1), &info) != 0)
		{
	                fprintf(stderr, "Cannot initialize WinSock");
	                return -1;
		}
        }
#endif

        if((sockfd = socket(PF_INET, SOCK_DGRAM, 0)) == -1)
	{
                perror("sockfd");
                return -1;
        }

	pollfd.fd = sockfd;
	pollfd.events = POLLIN;

       if((setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, (const  void*) &broadcast, sizeof broadcast)) == -1)
	{
		perror("setsockopt - SO_BROADCAST");
                return -1;
        }

        sendaddr.sin_family = AF_INET;
        sendaddr.sin_port = htons(0);
        sendaddr.sin_addr.s_addr = INADDR_ANY;
        memset(sendaddr.sin_zero,'\0',sizeof sendaddr.sin_zero);

        if(bind(sockfd, (struct sockaddr*) &sendaddr, sizeof sendaddr) == -1)
	{
		perror("bind");
		return -1;
        }

	recvaddr.sin_family = AF_INET;
	recvaddr.sin_port = htons(port);
	recvaddr.sin_addr.s_addr = INADDR_BROADCAST;

	memset(recvaddr.sin_zero,'\0',sizeof recvaddr.sin_zero);

	packet = malloc ( sizeof ( char ) * DISCOVERY_PKTSIZE );
	v = malloc ( sizeof ( char ) * 256 );
	t = malloc ( sizeof ( char ) * 256 );
	server_name = malloc ( sizeof ( char ) * 256 );
	server_json = malloc ( sizeof ( char ) * 256 );
	if ( (packet == NULL) ||
		(v == NULL) ||
		(t == NULL) ||
		(server_name == NULL) ||
		(server_json == NULL) )
	{
		perror("malloc");
		return -1;
	}

	for (try = 0; try < 5; try ++)
	{
		if (sendto(sockfd, SLIMPROTO_DISCOVERY, sizeof(SLIMPROTO_DISCOVERY), 0,
			(struct sockaddr *)&recvaddr, sizeof(recvaddr)) == -1)
		{
			CLOSESOCKET(sockfd);
			perror("sendto");
			return -1;
		}

		DEBUGF("slimproto_discover: discovery packet sent\n");

		/* Wait up to 1 second for response */
		while (poll(&pollfd, 1, 1000))
		{
			memset(packet,0,sizeof(packet));

			pktlen = recvfrom(sockfd, packet, DISCOVERY_PKTSIZE, MSG_DONTWAIT,
				(struct sockaddr *)&sendaddr, &sockaddr_len);

			if ( pktlen == -1 ) continue;

			/* Invalid response packet, try again */
			if ( packet[0] != 'E') continue;

			memset(server_name,0,sizeof(server_name));
			memset(server_json,0,sizeof(server_json));

			VDEBUGF("slimproto_discover: pktlen:%d\n",pktlen);

			/* Skip the E */
			pktidx = 1;

			while ( pktidx < (pktlen - 5) )
			{
				strncpy ( t, &packet[pktidx], pktidx + 3 );
				t[4] = '\0';
				l = (unsigned int) ( packet[pktidx + 4] );
				strncpy ( v, &packet[pktidx + 5], pktidx + 4 + l);
				v[l] = '\0';
				pktidx = pktidx + 5 + l;

				if ( memcmp ( t, "NAME", 4 ) == 0 )
				{
					strncpy ( server_name, v, l );
					server_name[l] = '\0';
				}
				else if ( memcmp ( t, "JSON", 4 ) == 0 )
				{
					strncpy ( server_json, v, l );
					server_json[l] = '\0';
				}

				VDEBUGF("slimproto_discover: key: %s len: %d value: %s pktidx: %d\n",
					t, l, v, pktidx);
			}

			inet_ntop(AF_INET, &sendaddr.sin_addr.s_addr, server_addr, server_addr_len);

			*jsonport = (unsigned int) strtoul(server_json, NULL, 10);

			DEBUGF("slimproto_discover: discovered %s:%u (%s)\n",
				server_name, *jsonport, server_addr);

			serveraddr_len = strlen(server_addr);

			/* Server(s) responded, so don't try again */
			try = 5;

			if ( scan )
				printf("%s:%u (%s)\n", server_name, *jsonport, server_addr);
			else
				break ; /* Return first server that replied */
		}
	}

	CLOSESOCKET(sockfd);

	if ( scan )
	{
		strcpy ( server_addr, "0.0.0.0" );
		*jsonport = 0;
		serveraddr_len = -1;
#ifdef __WIN32__
		WSACleanup();
#endif
	}

	if ( server_json != NULL )
		free (server_json);

	if ( server_name != NULL )
		free (server_name);

	if ( t != NULL )
		free (t);

	if ( v != NULL )
		free (v);

	if ( packet != NULL )
		free (packet);

	DEBUGF("slimproto_discover: end\n");
	
	return serveraddr_len ;
}

int slimproto_connect(slimproto_t *p, const char *server_addr, int port) {
	struct hostent *server;
	int return_value = 0; 

	DEBUGF("slimproto_connect: (%s, %i)\n", server_addr, port);

	server = gethostbyname(server_addr);	
	if (server == NULL) {
		fprintf(stderr, "Error no such host: %s\n", server_addr);
		return -1;
	}

	slimproto_close(p);

	pthread_mutex_lock(&p->slimproto_mutex);				

	memset(&p->serv_addr, 0, sizeof(p->serv_addr));	
	memcpy((char *)&p->serv_addr.sin_addr.s_addr,
		(char *)server->h_addr, 
		server->h_length);
	p->serv_addr.sin_family = server->h_addrtype;
	p->serv_addr.sin_port = htons(port);
	
	p->state = PROTO_CONNECT;

	pthread_cond_broadcast(&p->slimproto_cond);
	
	/* Wait for confirmation that the connection opens correctly.  This
	** will fail, for example, if Squeezebox Server is not running when the
	** connection attempt happens.
	*/
	while (p->state != PROTO_CONNECTED) {
		pthread_cond_wait(&p->slimproto_cond, &p->slimproto_mutex);
		if (p->state == PROTO_CLOSED) {
			return_value = -1;
			break;
		}
	}

	pthread_mutex_unlock(&p->slimproto_mutex);

	return return_value;
}	

static int proto_connect(slimproto_t *p) {
	pthread_mutex_lock(&p->slimproto_mutex);					

	p->sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (p->sockfd < 0)
	{
		perror("Error creating socket");
			goto proto_connect_err;
	}

	if ( slimproto_configure_socket (p->sockfd, 30) != 0 )
	{
		CLOSESOCKET(p->sockfd);
		goto proto_connect_err;
	}

	if (connect(p->sockfd, (struct sockaddr *)&p->serv_addr, sizeof(p->serv_addr)) != 0)
	{
		fprintf(stderr, "Error connecting to %s:%i\n", inet_ntoa(p->serv_addr.sin_addr), \
			ntohs(p->serv_addr.sin_port));
		CLOSESOCKET(p->sockfd);
		goto proto_connect_err;
	}
	
	DEBUGF("proto_connect: connected to %s\n", inet_ntoa(p->serv_addr.sin_addr));

	p->state = PROTO_CONNECTED;

	pthread_mutex_unlock(&p->slimproto_mutex);						
	pthread_cond_broadcast(&p->slimproto_cond);

	return 0;
	
proto_connect_err:
	p->state = PROTO_CLOSED;
	p->sockfd = -1;

	pthread_mutex_unlock(&p->slimproto_mutex);

	pthread_cond_broadcast(&p->slimproto_cond);
	DEBUGF("proto_connect: broadcast.\n" );

	return -1;
}

int slimproto_close(slimproto_t *p) {
	pthread_mutex_lock(&p->slimproto_mutex);					
	DEBUGF("proto_close: state %i\n", p->state);

	if ( p->state != PROTO_CONNECTED ) {
		pthread_mutex_unlock(&p->slimproto_mutex);
		DEBUGF("proto_close: not connected\n");
		return 0;
	}
	
	CLOSESOCKET(p->sockfd);

	p->sockfd = -1;
	p->state = PROTO_CLOSED;

	pthread_mutex_unlock(&p->slimproto_mutex);

	pthread_cond_broadcast(&p->slimproto_cond);

	return 0;
}

static int proto_recv(slimproto_t *p) {
	short len;
	unsigned char buf[BUF_LENGTH];
	int r, n, i;

        /* Fix receive error on quiting */
	if (p->state != PROTO_CONNECTED) return -1;

	n = recv(p->sockfd, buf, 2, 0);

	if (n <= 0)
	{
#ifdef __WIN32__
		/* Use WSAGetLastError instead of errno for WIN32 */
		DEBUGF("proto_recv: (1) n=%i WSAGetLastError=(%i)\n", n, WSAGetLastError());
#else
		DEBUGF("proto_recv: (1) n=%i msg=%s(%i)\n", n, strerror(errno), errno);
#endif
		return -1;	
	}

	len = ntohs(*((u16_t *)buf)) + 2;

        /* Fix receive error on quiting */
	if (p->state != PROTO_CONNECTED) return -1;

	r = 2;
	while (r < len)
	{
		n = recv(p->sockfd, buf+r, len-r, 0);
		if (n <= 0)
		{
#ifdef __WIN32__
			/* Use WSAGetLastError instead of errno for WIN32 */
			DEBUGF("proto_recv: (2) n=%i WSAGetLastError=(%i)\n", n, WSAGetLastError());
#else
			DEBUGF("proto_recv: (2) n=%i  msg=%s(%i)\n", n, strerror(errno), errno);
#endif
			return -1;	
		}	
		r += n;
	}
	
	DEBUGF("proto_recv: cmd=%4.4s len=%i\n", buf+2, len);

	buf[len]=0;
	for (i=0; i<p->num_command_callbacks; i++) {
		if (strncmp(p->command_callbacks[i].cmd, (char*)(buf+2), 4) == 0) {
			int ok = (p->command_callbacks[i].callback)(p, buf, len, p->command_callbacks[i].user_data);
			if (ok < 0) {
				fprintf(stderr, "Error in callback");
				return ok;	
			}
			
			break;	
		}
	}

	return 0;
}

void slimproto_parse_command(const unsigned char *buf, int buf_len, slimproto_msg_t *msg) {
	int http_len, val, i;
	char str[2] = { '\0', '\0' };
	
	memset(msg, 0, sizeof(slimproto_msg_t));
	
	if (strncmp((char*)(buf+2), "strm", 4) == 0) {
		msg->strm.length = unpackN2(buf, 0);
		memcpy(msg->strm.cmd, buf+2, 4);
		msg->strm.command = unpackC(buf, 6);
		msg->strm.autostart = unpackC(buf, 7);
		msg->strm.mode = unpackC(buf, 8);
		msg->strm.pcm_sample_size = unpackC(buf, 9);
		msg->strm.pcm_sample_rate = unpackC(buf, 10);
		msg->strm.pcm_channels = unpackC(buf, 11);
		msg->strm.pcm_endianness = unpackC(buf, 12);
		msg->strm.threshold = unpackC(buf, 13);
		msg->strm.spdif_enable = unpackC(buf, 14);
		msg->strm.transition_period = unpackC(buf, 15);
		msg->strm.transition_type = unpackC(buf, 16);
		msg->strm.flags = unpackC(buf, 17);
		msg->strm.output_threshold = unpackC(buf, 18);
		msg->strm.reserved = unpackC(buf, 19);
		msg->strm.replay_gain = unpackN4(buf, 20);
		msg->strm.server_port = unpackN2(buf, 24);
		msg->strm.server_ip = unpackN4(buf, 26);
		http_len = msg->strm.length-28;

		if (http_len > 0) {
			assert(http_len+1 < sizeof(msg->strm.http_hdr));
			memcpy(msg->strm.http_hdr, buf+30, http_len);
		}
		*(msg->strm.http_hdr + http_len) = '\0';
	}
	else if (strncmp((char*)(buf+2), "audg", 4) == 0) {
		msg->audg.length = unpackN2(buf, 0);
		memcpy(msg->audg.cmd, buf+2, 4);
		msg->audg.old_left_gain = unpackN4(buf, 6);
		msg->audg.old_right_gain = unpackN4(buf, 10);
		msg->audg.digital_volume_control = unpackC(buf, 14);
		msg->audg.preamp = unpackC(buf, 15);
		msg->audg.left_gain = unpackN4(buf, 16);
		msg->audg.right_gain = unpackN4(buf, 20);
	}
	else if (strncmp((char*)(buf+2), "vers", 4) == 0) {
		msg->vers.length = unpackN2(buf, 0);
		memcpy(msg->vers.cmd, buf+2, 4);

		val =  0;
		/* This assumes the version format is a.b.c, where
		** a, b, c are numbers of at most 2 digits.
		*/
		for( i = 6; i <= buf_len; ++i ) {
			if(buf[i]=='.' || i == buf_len) {
				msg->vers.version <<= 8;
				msg->vers.version += val;
				val = 0;
			}
			else if (isdigit(buf[i])) {
				val <<= 4;
				str[0] = buf[i];
				val += atoi(str);
			}
		}

	}
	else {
		DEBUGF("proto_parse: cannot parse %4.4s\n", buf+2);
	}	
}

int slimproto_dsco(slimproto_t *p, int dscoCode) {
	unsigned char msg[SLIMPROTO_MSG_SIZE];

	pthread_mutex_lock(&p->slimproto_mutex);
	if (p->state != PROTO_CONNECTED) {
  		pthread_mutex_unlock(&p->slimproto_mutex);
		return 0;
	}
	pthread_mutex_unlock(&p->slimproto_mutex);

	memset(&msg, 0, SLIMPROTO_MSG_SIZE);

	packA4(msg, 0, "DSCO");
	packN4(msg, 4, 1);
	packC(msg, 8, dscoCode);
		
	return slimproto_send(p, msg);
}

/*
 * upgrade = 0x00 -> always zero for squeezeslave
 * upgrade = 0x01 -> player is goint out for firmware upgrade
 */
int slimproto_goodbye(slimproto_t *p, u8_t upgrade) {
	unsigned char msg[SLIMPROTO_MSG_SIZE];

	pthread_mutex_lock(&p->slimproto_mutex);
	if (p->state != PROTO_CONNECTED) {
  		pthread_mutex_unlock(&p->slimproto_mutex);
		return 0;
	}
	pthread_mutex_unlock(&p->slimproto_mutex);

	memset(&msg, 0, SLIMPROTO_MSG_SIZE);

        packA4(msg, 0, "BYE!");
        packN4(msg, 4, 1);
        packC(msg, 8, upgrade);

	return slimproto_send(p, msg);
}

int slimproto_helo(slimproto_t *p, char device_id, char revision, const char *macaddress, char isGraphics, char isReconnect) {	
	unsigned char msg[SLIMPROTO_MSG_SIZE];
#if 0
	const char *capabilites = \
		 "Model=squeezeslave,ModelName=SqueezeSlave,Firmware=7,ogg,flc,pcm,mp3,SampleRate=44100,HasPreAmp";
#endif
	int caplen = 0;
	int channelList = 0;

	if (isGraphics)
		channelList |= 0x8000;

	if (isReconnect)
		channelList |= 0x4000;
#if 0
	caplen = strlen(capabilites);
#endif
	memset(&msg, 0, SLIMPROTO_MSG_SIZE);

	packA4(msg, 0, "HELO");
	packN4(msg, 4, (36+caplen));	/* Packet Length */
	packC(msg, 8, device_id);	/* Device Type */
	packC(msg, 9, revision);	/* Revision */
	memcpy(msg+10, macaddress, 6);	/* MAC */
	memcpy(msg+26, macaddress, 6);	/* UID */
	packN2(msg, 32, channelList);	/* WLan Channel List */
	packN4(msg, 34, 0);		/* Bytes Received H */
	packN4(msg, 38, 0);		/* Bytes Received L */
	packC(msg, 42, 'E');		/* Language */
	packC(msg, 43, 'N');
#if 0
	memcpy(msg+44,capabilites,caplen);
#endif
	return slimproto_send(p, msg);
}

int slimproto_ir(slimproto_t *p, int format, int noBits, int irCode) {
	unsigned char msg[SLIMPROTO_MSG_SIZE];
	memset(&msg, 0, SLIMPROTO_MSG_SIZE);

	packA4(msg, 0, "IR  ");
	packN4(msg, 4, 10);
	slimproto_set_jiffies(p, msg, 8);
	packC(msg, 12, format);
	packC(msg, 13, noBits);
	packN4(msg, 14, irCode);
	
	return slimproto_send(p, msg);
}

int slimproto_stat(slimproto_t *p, const char *code, int decoder_buffer_size, int decoder_buffer_fullness, u64_t bytes_rx, int output_buffer_size, int output_buffer_fullness, u32_t elapsed_milliseconds, u32_t server_timestamp)
{
	u32_t client_timestamp;
	u32_t rbytes_low, rbytes_high;
	unsigned char msg[SLIMPROTO_MSG_SIZE];

	u32_t elapsed_seconds = elapsed_milliseconds/1000;
	rbytes_high = (u32_t) (bytes_rx >> 32);
	rbytes_low = (u32_t) bytes_rx;

	memset(&msg, 0, SLIMPROTO_MSG_SIZE);

/* STAT structure from 7.4r25910
struct status_struct {
u32_t event;
u8_t num_crlf;          number of consecutive cr|lf received while parsing headers
u8_t mas_initialized;   'm' or 'p'
u8_t mas_mode;          serdes mode
u32_t rptr;
u32_t wptr;
u64_t bytes_received;
u16_t signal_strength;
u32_t jiffies;
u32_t output_buffer_size;
u32_t output_buffer_fullness;
u32_t elapsed_seconds;
u16_t voltage;
u32_t elapsed_milliseconds;
u32_t server_timestamp;
u16_t error_code;
}
*/
	packA4(msg, 0, "STAT");
	packN4(msg, 4, 53);
	packA4(msg, 8, code);
	packC(msg, 12, 0);
	packC(msg, 13, 0);
	packC(msg, 14, 0);
	packN4(msg, 15, decoder_buffer_size);
	packN4(msg, 19, decoder_buffer_fullness);
	packN4(msg, 23, rbytes_high );
	packN4(msg, 27, rbytes_low );
	packN2(msg, 31, 65534); 				/* signal strength */
	client_timestamp = slimproto_set_jiffies(p, msg, 33);	/* Keep both values close, not used */
	packN4(msg, 37, output_buffer_size);
	packN4(msg, 41, output_buffer_fullness);
	packN4(msg, 45, elapsed_seconds);
	packN2(msg, 49, 0);					/* voltage */
	packN4(msg, 51, elapsed_milliseconds);
	packN4(msg, 55, server_timestamp);
	packN2(msg, 59, 0);					/* error code */

	DEBUGF("proto_stat: code=%4.4s decoder_buffer_size=%u decoder_buffer_fullness=%u ",
		code, decoder_buffer_size, decoder_buffer_fullness);

	DEBUGF("rbytes_high=%u rbytes_low=%u output_buffer_size=%u output_buffer_fullness=%u ",
       		rbytes_high, rbytes_low, output_buffer_size, output_buffer_fullness);

	DEBUGF("elapsed_seconds=%u elapsed_milliseconds=%u server_timestamp=%u\n",
       		elapsed_seconds, elapsed_milliseconds, server_timestamp);

	return slimproto_send(p, msg);	
}

u32_t slimproto_set_jiffies(slimproto_t *p, unsigned char *buf, int jiffies_ptr) {
	struct timeval tnow;
	u32_t jiffies;
	u32_t timestamp;

	gettimeofday(&tnow, NULL);

	timestamp = tnow.tv_sec * 1000 + tnow.tv_usec / 1000;
	jiffies = timestamp- p->epoch.tv_sec * 1000 + p->epoch.tv_usec / 1000;

	packN4(buf, jiffies_ptr, jiffies);

	return timestamp;
}

/* send a complete message or fail */
int send_message(int sockfd, unsigned char* msg, size_t msglen, int msgflags) {
	size_t nsent = 0;
	size_t n;

	do {
		n = send(sockfd, &msg[nsent], msglen - nsent, msgflags);
		if (n < 0) {
                return n;
		}
		nsent += n;
	} while (nsent < msglen);
	return nsent;
}

int slimproto_send(slimproto_t *p, unsigned char *msg) {
	int n;

	DEBUGF("proto_send: cmd=%4.4s len=%i\n", msg, unpackN4(msg, 4));

	pthread_mutex_lock(&p->slimproto_mutex);

	if (p->state != PROTO_CONNECTED) {
		pthread_mutex_unlock(&p->slimproto_mutex);
		return -1;		
	}

	n = send_message(p->sockfd, msg, unpackN4(msg, 4) + 8, slimproto_get_socketsendflags());

	if (n < 0)
	{
		int i;
#ifdef __WIN32__
                /* Use WSAGetLastError instead of errno for WIN32 */
                DEBUGF("proto_send: (1) n=%i WSAGetLastError=(%i)\n", n, WSAGetLastError());
#else
                DEBUGF("proto_send: (1) n=%i msg=%s(%i)\n", n, strerror(errno), errno);
#endif

		pthread_mutex_unlock(&p->slimproto_mutex);
		slimproto_close(p);

		for (i=0; i<p->num_connect_callbacks; ++i) {
			(p->connect_callbacks[i].callback)(p, false, p->connect_callbacks[i].user_data);
		}

		return -1;
	}

	pthread_mutex_unlock(&p->slimproto_mutex);
	return 0;
}

int slimproto_configure_socket_sigpipe(int fd) {
#if defined(MSG_NOSIGNAL)
	/* This platform has MSG_NOSIGNAL (Linux has it for sure, not sure about
	** others).  So we'll let the send() call deal with the SIGPIPE
	** avoidance.
	*/
	DEBUGF("proto_sigpipe: MSG_NOSIGNAL\n");
	return 0;
#elif defined(SO_NOSIGPIPE)
	/* This platform doesn't have MSG_NOSIGNAL but has a similar
	** configuration option that lets one change the SIGPIPE behavior for
	** the socket instead of for each send() call.  BSD-based OSes are said
	** to have this flag, including OSX and Solaris.
	*/
	DEBUGF("proto_sigpipe: SO_NOSIGPIPE\n");
	int enable = 1;
	return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&enable, 
			  sizeof(enable));
#elif !defined(SIGPIPE)
	/* Some platforms, such as win32+mingw, don't even have SIGPIPE, so
	** there is nothing to deal with in terms of signals here.
	*/
	DEBUGF("proto_sigpipe: No SIGPIPE\n");
	return 0;
#else
	/* This platform has no mechanism to prevent SIGPIPE from being emitted
	** when writing to a closed socket.  We have no other way than
	** installing a no-op signal handler for SIGPIPE.  That's too bad
	** because the whole process looses the possibility of receiving this
	** signal, should they wish to.
	*/
	static bool first_time = true;
	if (first_time) {
		first_time = false;
		return signal(SIGPIPE, SIG_IGN) == SIG_ERR ? -1 : 0;
	}
	DEBUGF("proto_sigpipe: SIGPIPE no-op signal handler\n");
	return 0;
#endif
}

int slimproto_get_socketsendflags() {

#ifdef MSG_NOSIGNAL
	/* This platorm defines MSG_NOSIGNAL, so we'll give this flag for the
	** caller to pass to the send() system call, thus avoiding receiving
	** SIGPIPE if the socket has been closed by the other end.
	*/
	return MSG_NOSIGNAL;
#else
	return 0;
#endif
}
