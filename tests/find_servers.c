/*
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

#define BUF_LENGTH 4096

/* fprintf(stderr, __VA_ARGS__) */
#define DEBUGF(...)
#define VDEBUGF(...)

#define packN4(ptr, off, v) { ptr[off] = (char)(v >> 24) & 0xFF; ptr[off+1] = (v >> 16) & 0xFF; ptr[off+2] = (v >> 8) & 0xFF; ptr[off+3] = v & 0xFF; }
#define packN2(ptr, off, v) { ptr[off] = (char)(v >> 8) & 0xFF; ptr[off+1] = v & 0xFF; }
#define packC(ptr, off, v) { ptr[off] = v & 0xFF; }
#define packA4(ptr, off, v) { strncpy((char*)(&ptr[off]), v, 4); }

#define unpackN4(ptr, off) ((ptr[off] << 24) | (ptr[off+1] << 16) | (ptr[off+2] << 8) | ptr[off+3])
#define unpackN2(ptr, off) ((ptr[off] << 8) | ptr[off+1])
#define unpackC(ptr, off) (ptr[off])

#define	bool	int
#define	true	1
#define	false	0

#define SLIMPROTO_DISCOVERY "eNAME\0JSON\0"

int slimproto_discover(char *server_addr, int server_addr_len, int port, bool scan)
{
	int sockfd;
	int try;
	char packet[100];
	int pktlen;
	int pktidx;
	char t[5];
	unsigned int l;
	char v[256];
	char server_name[17];
	char server_web[6];
	struct pollfd pollfd;
	struct sockaddr_in sendaddr;
	struct sockaddr_in recvaddr;
	socklen_t sockaddr_len = sizeof(sendaddr);
	int broadcast=1;
	int serveraddr_len = -1;

        if((sockfd = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
                perror("sockfd");
                return -1;
        }

	pollfd.fd = sockfd;
	pollfd.events = POLLIN;

       if((setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, (const  void*) &broadcast, sizeof broadcast)) == -1) {
		perror("setsockopt - SO_BROADCAST");
                return -1;
        }

        sendaddr.sin_family = AF_INET;
        sendaddr.sin_port = htons(0);
        sendaddr.sin_addr.s_addr = INADDR_ANY;
        memset(sendaddr.sin_zero,'\0',sizeof sendaddr.sin_zero);

        if(bind(sockfd, (struct sockaddr*) &sendaddr, sizeof sendaddr) == -1) {
            perror("bind");
            return -1;
        }
	recvaddr.sin_family = AF_INET;
	recvaddr.sin_port = htons(port);
	recvaddr.sin_addr.s_addr = INADDR_BROADCAST;

	memset(recvaddr.sin_zero,'\0',sizeof recvaddr.sin_zero);

	for (try = 0; try < 5; try ++) {
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

			if (recvfrom(sockfd, packet, sizeof(packet), MSG_DONTWAIT, (struct sockaddr *)&sendaddr,
				&sockaddr_len) == -1) continue;

			/* Invalid response packet, try again */
			if ( packet[0] != 'E') continue;

			memset(server_name,0,sizeof(server_name));
			memset(server_web,0,sizeof(server_web));

			pktlen = strlen (packet);

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
					strncpy ( server_web, v, l );
					server_web[l] = '\0';
				}

				VDEBUGF("slimproto_discover: key: %s len: %d value: %s pktidx: %d\n",
					t, l, v, pktidx);
			}

			inet_ntop(AF_INET, &sendaddr.sin_addr.s_addr, server_addr, server_addr_len);

			DEBUGF("slimproto_discover: discovered %s:%s (%s)\n",
				server_name, server_web, server_addr);

			serveraddr_len = strlen(server_addr);

			/* Server(s) responded, so don't try again */
			try = 5;

			if ( scan )
				printf("%s:%s (%s)\n", server_name, server_web, server_addr);
			else
				break ; /* Return first server that replied */
		}
	}

	CLOSESOCKET(sockfd);

	if ( scan )
	{
		strcpy ( server_addr, "0.0.0.0" );
		serveraddr_len = -1;
	}

	DEBUGF("slimproto_discover: end\n");
	
	return serveraddr_len ;
}

int main (void)
{
	char slimserver_address[256] = "127.0.0.1";
	int port = 3483;
	int len ;

	/* Scan */
	len = slimproto_discover(slimserver_address, sizeof (slimserver_address), port, true);

	VDEBUGF("main: slimproto_discover_scan: address:%s len:%d\n", slimserver_address, len );

#if 0
	/* Discovery */
	len = slimproto_discover(slimserver_address, sizeof (slimserver_address), port, false);

	VDEBUGF("main: slimproto_discover: address:%s len:%d\n", slimserver_address, len );
#endif
	return 0;
}

