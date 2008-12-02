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
 *   Modified 18 November 2008 by Graham Chapman
 *   Changes:
 *      - Supportvariable size text display and/or LCDd display
 *      - Support keyboard control and/or lircd control
 *      - Use signal to exit default (option removed)
 *        In curses mode (--display) Esc and Q also quit
 *      - USR2 signal can be used to toggle LCDd/lircd support on and off
 *        while squeezeslave is running.
 *        Useful to continue music while display/ir is used for something else.
 *      - Audio device list is now available as a command line option (--list)
 *      - Command line options have changed to support the above, see --help
 *      - Fixup USR1 handler to allow graceful quit if retry is not enabled
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>

#ifdef INTERACTIVE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <locale.h>
#include <ctype.h>
#include <curses.h>
#include <fcntl.h>
#include <lirc/lirc_client.h>
#include <netinet/tcp.h> 
#endif

#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"

#define RETRY_DEFAULT	5
#define LINE_COUNT 2

#ifdef INTERACTIVE
#define packN4(ptr, off, v) { ptr[off] = (char)(v >> 24) & 0xFF; ptr[off+1] = (v >> 16) & 0xFF; ptr[off+2] = (v >> 8) & 0xFF; ptr[off+3] = v & 0xFF; }
#define packC(ptr, off, v) { ptr[off] = v & 0xFF; }
#define packA4(ptr, off, v) { strncpy((char*)(&ptr[off]), v, 4); }
struct lirc_config *lircconfig;
#endif

static int connect_callback(slimproto_t *p, bool isConnected, void *user_data);
static int parse_macaddress(char *macaddress, const char *str);
static void print_version();
static void print_help();
static void exit_handler(int signal_number);
static void restart_handler(int signal_number);

static volatile bool signal_exit_flag = false;
static volatile bool signal_restart_flag = false;
static const char* version = "0.8-23";

static int player_type = 8;

#ifdef INTERACTIVE
static void toggle_handler(int signal_number);
static int linelen = 40;
static int vfd_callback(slimproto_t *p, const unsigned char * buf, int len , void *user_data);
#endif

// There is not enough support in Windows+mingw to use signals in the
// implementation of the 'restart' feature.  So we support two 
// implementations:
// . one that is based on signals and is more responsive when a restart is 
//   needed (USE_SIGNALS_FOR_RESTART defined).
// . one that polls the restart flag once in a while: less responsive but
//   portable  (USE_SIGNALS_FOR_RESTART not defined).

#ifdef USE_SIGNALS_FOR_RESTART

static void install_restart_handler() {
	signal(SIGUSR1, &restart_handler);
}


static void wait_for_restart_signal() {
	pause();
}

static void send_restart_signal() {
	kill(getpid(), SIGUSR1);
}

#else

static void install_restart_handler() {
}

static void wait_for_restart_signal() {
	Pa_Sleep(5000);
}

static void send_restart_signal() {
	restart_handler(0);
}
#endif

// For retry support
bool retry_connection = false;

struct timeval uptime; /* time we started */


#ifdef INTERACTIVE
// For curses display
SCREEN *term = NULL;
WINDOW *slimwin = NULL;
WINDOW *errwin = NULL;
WINDOW *msgwin = NULL;

// Used to toggle IR/LCD support on and off
static void install_toggle_handler() {
	signal(SIGUSR2, &toggle_handler);
}

int using_curses = 0;
// For LCDd support
int lcd_fd = 0; 
struct sockaddr_in *lcd_addr;
bool use_lcdd_menu = false;

// For lircd support
bool using_lirc = false;
int lirc_fd = 0;
char * lircrc;

// Close LCDd connection (if open)
static void close_lcd() {
   if (use_lcdd_menu) {
      free (lcd_addr);
      close (lcd_fd);
      use_lcdd_menu = false;
   }
}

// Close lircd connection (if open)
void close_lirc(){
   if (using_lirc) {
      lirc_freeconfig(lircconfig);
      lirc_deinit();
      using_lirc = false;
   }
}

// Set fd to non-blocking mode
int setNonblocking(int fd) {
    int flags;

    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
       flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
} 

// Read response from LCDd and update our line length is one is supplied
static bool read_lcd() {
  char buf[1024];
  int res;
  long int num=linelen;
  char *pos;

  res = recv(lcd_fd, buf, 1024, MSG_DONTWAIT);
  if (res>9) {
     buf[res-1]=0; // Null terminate before calling strstr
     if ((pos= strstr(buf,"wid "))) {
        num=strtol(pos+3,NULL,10);
	if (num<100 && num>10)
	   linelen=num;
     }
  }
  return ((res > 0)?true:false);
}

// Send data to lcdd
static void send_lcd(char* data, int len) {
  int sent = 0;
  int res;
  while(sent < len) {
     res = send(lcd_fd, data+sent, len-sent, 0);
     if (res == -1){
	fprintf(stderr,"LCD Send Failed\n");
	return;
     }
     sent += res;
   }
}

// Try to open a connection to lircd if suppor is enabled
// if it fails, disable support, print a message and continue
void init_lirc() {
   if (using_lirc) {
      using_lirc = false;
      if ((lirc_fd = lirc_init("squeezeslave",1)) > 0) {
         if (lirc_readconfig(lircrc, &lircconfig, NULL)==0){
            using_lirc = true;
            setNonblocking(lcd_fd);
         } else {
	    using_lirc = true;
            close_lirc();
	 }
      }
      if (!using_lirc ) fprintf(stderr, "Failed to init LIRC\n");
   }
}

// Try to open a connection to LCDd if support is enabled
// If it succeeeds configure our screen
// If it fails, print a message, disable support and continue
void init_lcd () {
     if (!use_lcdd_menu) return;
     use_lcdd_menu = false;
     lcd_fd = socket(AF_INET, SOCK_STREAM, 0);
     if (lcd_fd > 0) {
        lcd_addr = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in *));
        lcd_addr->sin_family = AF_INET;
	if (inet_pton(AF_INET, "127.0.0.1", (void *)(&(lcd_addr->sin_addr.s_addr))) >0) {
	   lcd_addr->sin_port = htons(13666);
           if (connect(lcd_fd, (struct sockaddr *)lcd_addr, sizeof(struct sockaddr)) >= 0){ 
              int flag = 1;
              if (setsockopt(lcd_fd, IPPROTO_TCP, TCP_NODELAY, (void*)&flag, sizeof(flag) ) == 0)   {
                 fprintf(stderr,"Connected to LCDd!\n");
                 use_lcdd_menu = true;
	         send_lcd("hello\n",6);
                 while(!read_lcd());  // wait for display info
	         send_lcd("client_set name {squeeze}\n",26);
	         send_lcd("screen_add main\n",16);
	         send_lcd("screen_set main name {main}\n",28);
	         send_lcd("screen_set main heartbeat off\n",30);
	         send_lcd("widget_add main one string\n",27);
                 send_lcd("widget_add main two string\n",27);
	         send_lcd("screen_set main -priority 256\n",30);
	         setNonblocking(lcd_fd);
	      }
	   }
	} 
     }
     // If connect failed
     if (!use_lcdd_menu) {
        use_lcdd_menu = true;
	close_lcd();
	fprintf(stderr,"Connect to LCDd failed!\n");
     }
}

// Called by our USR2 signal handler
// to toggle IR/LCD support on and off
static void toggle_handler(int signal_number) {
   if (use_lcdd_menu) {
      close_lcd();
      close_lirc();
   } else {
      use_lcdd_menu = true;
      using_lirc = true;
      init_lcd();
      init_lirc();
   }
}

// Read a key code from lircd
static int read_lirc() {
  char *code;
  char *c;
  int key;

  if (lirc_nextcode(&code)==0) {
     if (code!=NULL) {
        while((lirc_code2char(lircconfig,code,&c)==0) && (c != NULL)) {
	   key = (unsigned char)c[0];
	}
	free(code);
	return key;
     }
     return -1;
  }
  return 0;
}

// Set up a curses window for our display
void initcurses(void) {
    if (!using_curses)
       return;
    term = newterm(NULL, stdout, stdin);
    if (term != NULL) {
	int screen_width, screen_height;
	int window_width = linelen+2;
	int window_height = LINE_COUNT+2;
	int org_x, org_y;

	using_curses = 1;

	cbreak();
	noecho();
	nonl();

	nodelay(stdscr, TRUE);
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);
	leaveok(stdscr, TRUE);
	curs_set(0);

	getmaxyx(stdscr, screen_height, screen_width);
	org_x = (screen_width - window_width) / 2;
        org_y = (screen_height - window_height - 1) / 2;

	slimwin = subwin(stdscr, window_height, window_width, org_y, org_x);
	box(slimwin, 0, 0);

	if (screen_height > LINE_COUNT+2) {
	    /* create one-line error message subwindow */
	    errwin = subwin(stdscr, 1, screen_width, org_y+window_height, 0);
	}

	wrefresh(curscr);
    } else {
	using_curses = 0;
    }
}

// Shut down curses and put our terminal back to normal
void exitcurses(void) {
    if (using_curses) {
       endwin();
       delscreen(term);
    }
}

// Translate keys and lirc inputs
// to squeezecener IR codes
unsigned long getircode(int key) {
    unsigned long ir = 0;
    
    switch(key) {
    case '0': ir = 0x76899867; break;
    case '1': ir = 0x7689f00f; break;
    case '2': ir = 0x7689f00f; break;
    case '3': ir = 0x76898877; break;
    case '4': ir = 0x768948b7; break;
    case '5': ir = 0x7689c837; break;
    case '6': ir = 0x768928d7; break;
    case '7': ir = 0x7689a857; break;
    case '8': ir = 0x76896897; break;
    case '9': ir = 0x7689e817; break;
    case KEY_IC: ir = 0x7689609f; break; /* add */
    case 0x01: ir = 0x7689609f; break; /* add IR */
    case KEY_DOWN: ir = 0x7689b04f; break; /* arrow_down */
    case 0x02: ir = 0x7689b04f; break; /* arrow_down IR */
    case KEY_LEFT: ir = 0x7689906f; break; /* arrow_left */
    case 0x03: ir = 0x7689906f; break; /* arrow_left IR */
    case KEY_RIGHT: ir = 0x7689d02f; break; /* arrow_right */
    case 0x04: ir = 0x7689d02f; break; /* arrow_right IR */
    case KEY_UP: ir = 0x7689e01f; break; /* arrow_up */
    case 0x05: ir = 0x7689e01f; break; /* arrow_up IR*/
    case '<': ir = 0x7689c03f; break; /* rew */
    case ',': ir = 0x7689c03f; break; /* rew */
    case '>': ir = 0x7689a05f; break; /* fwd */
    case '.': ir = 0x7689a05f; break; /* fwd */
    case KEY_HOME: ir = 0x768922DD; break; /* home */
    case 0x06: ir = 0x768922DD; break; /* home IR*/
    case KEY_END: ir = 0x76897887; break; /* now_playing */
    case 0x07: ir = 0x76897887; break; /* now_playing IR*/
    case ' ': ir = 0x768920df; break; /* pause */
    case 'p': ir = 0x768920df; break; /* pause */
    case 'P': ir = 0x768920df; break; /* pause */
    case '\r': ir = 0x768910ef; break; /* play */
    case 'r': ir = 0x768938c7; break; /* repeat */
    case 'R': ir = 0x768938c7; break; /* repeat */
    case 's': ir = 0x7689d827; break; /* shuffle */
    case 'S': ir = 0x7689d827; break; /* shuffle */
    case '?': ir = 0x768958a7; break; /* search */
    case '/': ir = 0x768958a7; break; /* search */
    case 'b': ir = 0x7689708f; break; /* browse */
    case 'B': ir = 0x7689708f; break; /* browse */
    case 'f': ir = 0x768918e7; break; /* favourites */
    case 'F': ir = 0x768918e7; break; /* favourites */
    case '%': ir = 0x7689f807; break; /* size */
    case 'z': ir = 0x7689b847; break; /* sleep */
    case 'Z': ir = 0x7689b847; break; /* sleep */
    case '-': ir = 0x768900ff; break; /* voldown */
    case '+': ir = 0x7689807f; break; /* volup */
    /* non-IR key actions */
    case '\f': wrefresh(curscr); break; /* repaint screen */
    case 'q': ir=0x01 ;/* quit */
    case 'Q': ir=0x01 ;/* quit */
    case '\e': ir=0x01 ;/* quit */
  }

  return (unsigned long)ir;
}
#endif

// Print list of available audio devices
void listAudioDevices(slimaudio_t * slimaudio, int output_device_id) {
	int num_devices;
	char **devices;
	slimaudio_get_output_devices(slimaudio, &devices, &num_devices);


	printf("Output devices:\n");	
	int i;
	for (i=0; i<num_devices; i++) {
		if ( i == output_device_id  )
			printf("*%2d: %s\n", i, devices[i]);
		else
			printf(" %2d: %s\n", i, devices[i]);
	}
}

int main(int argc, char *argv[]) {
	slimproto_t slimproto;
	slimaudio_t slimaudio;
	char macaddress[6] = { 0, 0, 0, 0, 0, 1 };
#ifndef PORTAUDIO_ALSA
	int output_device_id = -1;
#else
	PaDeviceIndex output_device_id = 0;
#endif
	slimaudio_volume_t volume_control = VOLUME_SOFTWARE;
	unsigned int output_predelay = 0;
	unsigned int output_predelay_amplitude = 0;
	unsigned int retry_interval = RETRY_DEFAULT;
	int keepalive_interval = -1;
	bool listdevs = false;

#ifdef INTERACTIVE
        fd_set read_fds;
        fd_set write_fds;
        int key = 0;
        unsigned long ir = 0;
	int maxfd = 0;
	char * home;

        // default lircrc file ($HOME/.lircrc)
	home = getenv("HOME");
	if (home == NULL) home = "";
	lircrc = (char *)malloc((strlen(home) + strlen("/.lircrc") + 1) * sizeof(char));
	strcpy(lircrc,home);
	strcat(lircrc,"/.lircrc");
#endif

	while (true) {
		static struct option long_options[] = {
			{"predelay_amplitude", required_argument, 0, 'a'},
			{"debug",              required_argument, 0, 'd'},
			{"help",               no_argument,       0, 'h'},
			{"keepalive",          required_argument, 0, 'k'},
			{"list",               no_argument,       0, 'L'},
			{"mac",	               required_argument, 0, 'm'},
			{"oldplayer",          no_argument,       0, 'O'},
			{"output",             required_argument, 0, 'o'},
			{"predelay",           required_argument, 0, 'p'},
			{"retry",              no_argument,       0, 'R'},
			{"intretry",           required_argument, 0, 'r'},
			{"version",            no_argument,       0, 'V'},
			{"volume",             required_argument, 0, 'v'},
#ifdef INTERACTIVE
			{"lircrc",             required_argument, 0, 'c'},
			{"display",            no_argument,       0, 'D'},
			{"lirc",               no_argument,       0, 'i'},
			{"lcd",                no_argument,       0, 'l'},
			{"width",              required_argument, 0, 'w'},
#endif
			{0, 0, 0, 0}
		};
	
#ifdef INTERACTIVE
		const char shortopt =
			getopt_long_only(argc, argv, "a:d:hk:Lm:Oo:p:Rr:Vv:c:Dilw:",
					 long_options, NULL);
#else
		const char shortopt =
			getopt_long_only(argc, argv, "a:d:hk:Lm:Oo:p:Rr:Vv:",
					 long_options, NULL);

#endif

		if (shortopt == -1) {
			break;
		}

		switch (shortopt) {
		case 'a':
			output_predelay_amplitude = strtoul(optarg, NULL, 0);
			break;
		case 'd':
#ifdef SLIMPROTO_DEBUG        
			if (strcmp(optarg, "slimproto") == 0)
				slimproto_debug = true;
			else if (strcmp(optarg, "slimaudio") == 0)
				slimaudio_debug = true;
			else if (strcmp(optarg, "slimaudio_buffer") == 0)
				slimaudio_buffer_debug = true;
			else if (strcmp(optarg, "slimaudio_buffer_v") == 0)
				slimaudio_buffer_debug_v = true;
			else if (strcmp(optarg, "slimaudio_decoder") == 0)
				slimaudio_decoder_debug = true;
			else if (strcmp(optarg, "slimaudio_decoder_v") == 0)
				slimaudio_decoder_debug_v = true;
			else if (strcmp(optarg, "slimaudio_http") == 0)
				slimaudio_http_debug = true;
			else if (strcmp(optarg, "slimaudio_http_v") == 0)
				slimaudio_http_debug_v = true;
			else if (strcmp(optarg, "slimaudio_output") == 0)
				slimaudio_output_debug = true;
			else
				fprintf(stderr, "%s: Unknown debug option %s\n", argv[0], optarg);
#else
				fprintf(stderr, "%s: Recompile with -DSLIMPROTO_DEBUG to enable debugging.\n", argv[0]);
#endif
			break;
		case 'h':
			print_help();
			exit(0);	
		case 'k':
			keepalive_interval = strtoul(optarg, NULL, 0);
			break;
		case 'm':
			if (parse_macaddress(macaddress, optarg) != 0) {
				fprintf(stderr, "%s: Cannot parse mac address %s\n", argv[0], optarg);
				exit(-1);	
			}
			break;
		case 'O':
			player_type = 3;
			break;
		case 'o':
			output_device_id = strtoul(optarg, NULL, 0);
			break;
		case 'p':
			output_predelay = strtoul(optarg, NULL, 0);
			break;
		case 'R':
			retry_connection = true;
			break;
		case 'r':
			retry_connection = true;
			retry_interval = strtoul(optarg, NULL, 0);
			if ( retry_interval < 1 )
			{
				fprintf (stderr, "Retry option requires value in seconds.\n");
				exit(-1);
			}
			break;
#ifdef INTERACTIVE
		case 'c':
		        free(lircrc);
			lircrc = optarg;
			break;
		case 'D':
			using_curses = 1;
			break;
		case 'i':
			using_lirc = true;
			break;
		case 'l':
			use_lcdd_menu = true;
			break;
		case 'w':
			linelen = strtoul(optarg, NULL, 0);
			break;
#endif
		case 'L':
			listdevs = true;
			break;
		case 'V':
			print_version();
			exit(0);
			break;
		case 'v':
		 	if (strcmp(optarg, "on") == 0) {
				volume_control = VOLUME_DRIVER;
			}
			else if (strcmp(optarg, "sw") == 0 ) {
				volume_control = VOLUME_SOFTWARE;
			}
			else if (strcmp(optarg, "off") == 0 ) {
				volume_control = VOLUME_NONE;
			}
#if 0			
		case '?':
			fprintf( stderr, "Unknown option: %s.\n", argv[optind] );
			exit(-1);
			break;
#endif
		}
	}

	char *slimserver_address = "127.0.0.1";
	if (optind < argc)
		slimserver_address = argv[optind];
	
	if (retry_connection) {
		printf( "Setting retry interval to %d seconds.\n", retry_interval );
	}

	signal(SIGTERM, &exit_handler);
	install_restart_handler();
#ifdef INTERACTIVE
	install_toggle_handler();  //SIGUSR2 to toggle IR/LCD on and off
#endif
	if (slimproto_init(&slimproto) < 0) {
		fprintf(stderr, "Failed to initialize slimproto\n");
		exit(-1);	
	}

	if (slimaudio_init(&slimaudio, &slimproto) < 0) {
		fprintf(stderr, "Failed to initialize slimaudio\n");
		exit(-1);
	}

	if (listdevs) {
	   listAudioDevices(&slimaudio, output_device_id);
	   exit(1);
	}

	slimproto_add_connect_callback(&slimproto, connect_callback, 
					macaddress);
#ifdef INTERACTIVE
	// Process VFD (display) commands
 	slimproto_add_command_callback(&slimproto, "vfdc", vfd_callback, 
					macaddress);
#endif

	if (output_device_id >= 0) {
		slimaudio_set_output_device(&slimaudio, output_device_id);
	}

	slimaudio_set_volume_control(&slimaudio, volume_control);
	slimaudio_set_output_predelay(&slimaudio, output_predelay,
				      output_predelay_amplitude);

	if (keepalive_interval >= 0) {
		slimaudio_set_keepalive_interval(&slimaudio, keepalive_interval);
	}

#ifdef INTERACTIVE
	init_lcd();
#endif

	if (slimaudio_open(&slimaudio) < 0) {
		fprintf(stderr, "Failed to open slimaudio\n");
#ifdef INTERACTIVE
		close (lcd_fd);
#endif
		exit(-1);
	}

#ifdef INTERACTIVE
        init_lirc();

	setlocale(LC_ALL, "");
	initcurses();
#endif

	// When retry_connection is true, retry connecting to the SqueezeCenter 
	// until we succeed, unless the signal handler tells us to give up.
	do {
		while (slimproto_connect(
			&slimproto, slimserver_address, 3483) < 0) {
			if (!retry_connection || signal_exit_flag) {
				if (signal_exit_flag) {
					// No message when the exit is triggered
					// by the user.
#ifdef INTERACTIVE
					exitcurses();
					close_lirc();
					close_lcd();
#endif
					exit(0);
				}
#ifdef INTERACTIVE
				exitcurses();
				close_lirc();
				close_lcd();
#endif
				fprintf(stderr, 
					"Connection to SqueezeCenter %s failed.\n",
					slimserver_address);
				exit(-1);
			}
#ifdef INTERACTIVE
			exitcurses();
#endif
			fprintf(stderr,"Retry in %d seconds.\n", retry_interval);
			Pa_Sleep(1000 * retry_interval);
#ifdef INTERACTIVE
	   	        initcurses();
#endif
		}
                signal_restart_flag = false;
                while (!signal_exit_flag && !signal_restart_flag) {
#ifdef INTERACTIVE
                   if (using_curses == 1 || use_lcdd_menu) {
                      FD_ZERO(&read_fds);
                      FD_ZERO(&write_fds);
		      if (using_curses == 1)
     	                 FD_SET(0, &read_fds); /* watch stdin */
   	              if (use_lcdd_menu) {
		         FD_SET(lcd_fd, &read_fds); 
		         maxfd = lcd_fd;
		      }
   	              if (using_lirc) {
		         FD_SET(lirc_fd, &read_fds); 
                         if (lirc_fd > maxfd) 
		            maxfd = lirc_fd;
                      }
                      if(select(maxfd + 1, &read_fds, NULL, NULL, NULL) == -1) {
    	                 if (errno != EINTR) {
		           fprintf(stderr,"Select Error\n");
   	                   abort();
	                 } 
                      }
		      if (FD_ISSET(0, &read_fds)) {
                         while ((key = getch()) != ERR) {
                            ir = getircode(key);
	                    if (ir == 0x01) {
  		               signal_exit_flag = 1;
                            }else{
			       if (ir != 0) slimproto_ir(&slimproto, 1, 1, ir);
			    }
		         }
		      } 
		      if (using_lirc && FD_ISSET(lirc_fd, &read_fds)) {
                         while((key = read_lirc()) != 0 ) { 
                            ir = getircode(key);
	                    if (ir == 0x01) { 
  		               signal_exit_flag = 1;
                            } else {
			       if (ir != 0) slimproto_ir(&slimproto, 1, 1, ir);
			    }
		         }
		      } 
		      if (use_lcdd_menu && FD_ISSET(lcd_fd, &read_fds)) {
                         while(read_lcd()); 
		      }
		   } else {
                      wait_for_restart_signal();
		   }
#else
                   wait_for_restart_signal();
#endif
		}
#ifdef INTERACTIVE
                FD_ZERO(&read_fds);
                FD_ZERO(&write_fds);
#endif
		if (signal_restart_flag) { 
#ifdef INTERACTIVE
			exitcurses();
#endif
			fprintf(stderr,"Retry in %d seconds.\n", retry_interval);
			Pa_Sleep(1000 * retry_interval);
#ifdef INTERACTIVE
	   	        initcurses();
#endif
		}
        } while (signal_restart_flag && !signal_exit_flag);

#ifdef INTERACTIVE
	close_lirc();
#endif
	slimaudio_close(&slimaudio);
	slimproto_close(&slimproto);
#ifdef INTERACTIVE
        exitcurses();
        close_lcd();
#endif
	slimaudio_destroy(&slimaudio);
	slimproto_destroy(&slimproto);
	return 0;
} 

static void print_version() {
	fprintf(stdout, "squeezeslave %s\n", version);
}

static void print_help() {
	print_version();      
	fprintf(stdout,
"squeezeslave [options] [<squeezecenter address>]\n"
"The SqueezeCenter address defaults to 127.0.0.1.\n"
"Options:\n"
"-h, --help:                 Prints this message.\n"
"-a,                         Sets the amplitude of a high-frequency tone\n"
"--predelay_amplitude <val>: produced during the predelay (see --predelay).\n"
"                            The frequency is set at the source's sampling\n"
"                            rate/2 and the amplitude is in absolute value.\n"
"                            For 16-bit sources, the max is 32767, but values\n"
"                            below 10 are likely to work.  The goal is to\n"
"                            produce an inaudible signal that will cause DACs\n"
"                            to wake-up and lock before actual samples are\n"
"                            played out.  If the DAC locks using only silence,\n"
"                            do not use this option (it will default to 0).\n"
"-k, --keepalive <sec>:      Controls how frequently squeezeslave sends a\n"
"                            alive signal to SqueezeCenter.  6.5.x servers\n"
"                            need this to avoid dropping the player's\n"
"                            connection.  By default, the implementation\n"
"                            chooses the right value: 10s for a >=6.5.x server\n"
"                            and 0s for a <6.5.xserver, which means no\n"
"                            keepalive.\n"
#ifdef INTERACTIVE
"-l, --lcd                   Enable LCDd (lcdproc) text display.\n"
"                            Requires LCDd running on local host.\n"
"-i, --lirc                  Enable lirc remote control support.\n"
"                            Requires lirc running on local host.\n"
"-c, --lircrc <filename>:    Location of lirc client configuration file.\n"
"                            Default: ~/.lircrc\n"
"-D, --display               Enable slimp3 style text display and\n"
"                            keyboard input.\n"
"                            Keys: 0-9:             0-9\n"
"                                  Insert           Add\n"
"                                  Cursor Keys      Arrows\n"
"                                  >,<              Fwd,Rew\n"
"                                  Home             Home\n"
"                                  End              Now Playing\n"
"                                  Space or P       Pause\n"
"                                  Enter            Play\n"
"                                  R                Repeat\n" 
"                                  S                Shuffle\n" 
"                                  ?                Search\n"
"                                  B                Browse\n"
"                                  F                Favourites\n"
"                                  %%                Size\n"
"                                  Z                Sleep\n"
"                                  +,-              Vol up,down\n"
"-w. --width <chars>         Set the display width to <chars> characters\n"
"                            If using LCDd, width is detected.\n"
#endif
"-L, --list                  List available audio devices and exit.\n"
"-m, --mac <mac_address>:    Sets the mac address for this instance.\n"
"                            Use the colon-separated notation.\n"
"                            The default is 00:00:00:00:00:01.\n"
"                            SqueezeCenter uses this value to distinguish\n"
"                            multiple instances, allowing per-player settings.\n"
"-O, --oldplayer:            Uses an old player-type-id mostly compatible with\n"
"                            pre-7.0 SqueezeCenter\n"
"-o, --output <device_id>:   Sets the output device id.\n"
"                            The default id is 0.\n"
"                            The output device ids can be listed with -L.\n"
"-p, --predelay <msec>:      Sets a delay before any playback is started.  This\n"
"                            is useful if the DAC used for output is slow to\n"
"                            wake-up/lock, causing the first few samples to be\n"
"                            dropped.\n"
"--retry                     Causes the program to retry connecting to\n"
"                            SqueezeCenter until it succeeds or is stopped using\n"
"                            SIGTERM or keyboard entry (see -s/--signal).\n"
"                            If the connection to SqueezeCenter is lost, the\n"
"                            program will poll it until it restarts.  --retry\n"
"                            enables retry with a %d second delay between\n"
"                            attempts.\n"
"-r <sec>                    For a different retry interval use -r and the\n"
"                            desired interval in seconds. (ie. -r10)\n"
"                            A value is required for this option.\n"
"-V, --version:              Prints the squeezeslave version.\n"
"-v, --volume <on|sw|off>:   Enables/disables volume changes done by\n"
"                            SqueezeCenter during its operation, such as when\n"
"                            changing the volume through the web interface or\n"
"                            when applying replay gain.  Defaults to sw.\n"
"                                    on:  volume changes performed on device.\n"
"                                    sw:  volume changes performed in software.\n"
"                                    off: volume changes ignored.\n"
"-d, --debug <trace_name>:   Turns on debug tracing for the specified level.\n"
"                            The option can be used multiple times to enable\n"
"                            multiple levels.\n"
"                            Available levels:\n"
#ifndef SLIMPROTO_DEBUG
"                            (disabled, recompile with \"-DSLIMPROTO_DEBUG\")\n"
#endif
"                                    slimproto\n"
"                                    slimaudio\n"
"                                    slimaudio_buffer\n"
"                                    slimaudio_buffer_v\n"
"                                    slimaudio_decoder\n"
"                                    slimaudio_decoder_v\n"
"                                    slimaudio_http\n"
"                                    slimaudio_http_v\n"
"                                    slimaudio_output\n",
RETRY_DEFAULT);
}

// Handles a signal coming from outside this process and that is meant to 
// terminate the program cleanly.
static void exit_handler(int signal_number) {
	signal_exit_flag = true;
}

// Handles a signal coming from inside this process and that causes a restart
// of the SqueezeCenter connection.
static void restart_handler(int signal_number) {
        if (retry_connection) {
		signal_restart_flag = true;
	} else {
 		signal_exit_flag = true;
	}
}


#ifdef INTERACTIVE
// Send line to lcdd
void set_lcd_line(int lineid, char *text, int len) {
   int total = 27 + len;
   char cmd[total];
   
   if (lineid == 1)
      memcpy(cmd,"widget_set main one 1 1 {",25);
   else
      memcpy(cmd,"widget_set main two 1 2 {",25);
   memcpy (cmd+25,text,len);
   memcpy(cmd+total-2,"}\n",2);
   send_lcd(cmd,total);
}

// Change special LCD chars to something more printable on screen
unsigned char printable(unsigned char c) {
   switch (c) {
      case 11:  //block
         return '#';
	 break;;
      case 16:  //righarrow
         return '>';
	 break;;
      case 22:  //circle
         return '@';
	 break;;
      case 145: //note
         return 182;
	 break;;
      case 152: //bell
         return 229;
	 break;;
      default:
         return c;
      }
}

// Replace unprintable symbols in line
void makeprintable(unsigned char * line) {
    int n;

    for (n=0;n<linelen;n++) 
       line[n]=printable(line[n]);
}

// Show the display
void show_display_buffer(char *ddram) {
    char line1[linelen+1];
    char *line2;

    memset(line1, 0, linelen+1);
    strncpy(line1, ddram, linelen);   
    line2 = &(ddram[linelen]);
    line2[linelen] = '\0';

    if (use_lcdd_menu) {
      set_lcd_line(1,line1,linelen);
      set_lcd_line(2,line2,linelen);
    }

    if (using_curses) {
      // Convert special LCD chars
      makeprintable((unsigned char *)line1);
      makeprintable((unsigned char *)line2);
      mvwaddnstr(slimwin, 1, 1, line1, linelen);
      mvwaddnstr(slimwin, 2, 1, line2, linelen);
      wrefresh(slimwin);
    }
}

// Check if char is printable, or a valid symbol
bool charisok(unsigned char c) {

   switch (c) {
      case 11:  //block
      case 16:  //righarrow
      case 22:  //circle
      case 145: //note
      case 152: //bell
         return true;
	 break;;
      default:
         return isprint(c);
   }
}

// Process display data
void receive_display_data( unsigned short *data, int bytes_read) {
    unsigned short *display_data;
    char ddram[linelen * 2];
    int n;
    int addr = 0; /* counter */

    if (bytes_read % 2) bytes_read--; /* even number of bytes */
    display_data = &(data[6]); /* display data starts at byte 12 */

    memset(ddram, ' ', linelen * 2);
    for (n=0; n<(bytes_read/2); n++) {
        unsigned short d; /* data element */
        unsigned char t, c;

        d = ntohs(display_data[n]);
        t = (d & 0x00ff00) >> 8; /* type of display data */
        c = (d & 0x0000ff); /* character/command */
        switch (t) {
            case 0x03: /* character */
                if (!charisok(c)) c = ' ';
                if (addr <= linelen * 2) {
                    ddram[addr++] = c;
		}
                break;
            case 0x02: /* command */
                switch (c) {
                    case 0x06: /* display clear */
                        memset(ddram, ' ', linelen * 2);
			break;
                    case 0x02: /* cursor home */
                        addr = 0;
                        break;
                    case 0xc0: /* cursor home2 */
                        addr = linelen;
                        break;
                }
    	}
    }
    show_display_buffer(ddram);
}

// Called by the library when a vfd command is received.
static int vfd_callback(slimproto_t *p, const unsigned char * buf, int len , void *user_data) {
        unsigned short disp[len+1];
	
	if (p->state == PROTO_CONNECTED) {
           memcpy(disp, buf, len);
	   receive_display_data (disp, len);
        }
	return 0;
}
#endif


// Called by the library when the connection is either established or broken.
static int connect_callback(slimproto_t *p, bool isConnected, void *user_data) {
#ifdef INTERACTIVE
	unsigned char msg[SLIMPROTO_MSG_SIZE];
#endif

	if (isConnected) {
		if (slimproto_helo(p, player_type, 0, (char*) user_data, 0, 0) < 0) {
			fprintf(stderr, "Could not send helo to SqueezeCenter\n");
		        send_restart_signal();
		}
#ifdef INTERACTIVE
		if (!using_curses)
			fprintf(stderr, "Connected.\n");
                memset(&msg, 0, SLIMPROTO_MSG_SIZE);
                packA4(msg, 0, "SETD");
                packN4(msg, 4, 2);
                packC(msg, 8, 0xfe);
                packC(msg, 9, linelen);
               	slimproto_send(p, msg);
#endif
	}
	else {
		// Send the restart signal, which calls restart_handler to tell
		// the main thread to go back waiting for SqueezeCenter to be
		// available.
		if (!signal_exit_flag)
		    send_restart_signal();
	}
	
	return 0;
}

static int parse_macaddress(char *macaddress, const char *str) {
	char *ptr;
	int i;
	
	for (i=0; i<6; i++) {		
		macaddress[i] = (char) strtol (str, &ptr, 16);
		if (i == 5 && *ptr == '\0')
			return 0;
		if (*ptr != ':')
			return -1;
			
		str = ptr+1;
	}	
	return -1;
}
