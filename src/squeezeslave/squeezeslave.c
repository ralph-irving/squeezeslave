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
 *   Modified 18 January 2009 by Ivor Bosloper
 *      - Added support to startup as a daemon (--daemonize)
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

#include "squeezeslave.h"

#define INET_FQDNSTRLEN	(256)

// Retry support
bool retry_connection = false;
bool output_change = false;

#ifdef PORTAUDIO_DEV
// User suggested latency
bool modify_latency = false;
unsigned int user_latency = 0L;
#endif

static volatile bool signal_exit_flag = false;
static volatile bool signal_restart_flag = false;
const char* version = "1.2L";
const int revision = 333;
static int port = SLIMPROTOCOL_PORT;
static int firmware = FIRMWARE_VERSION;
static int player_type = PLAYER_TYPE;

#ifdef EMPEG
extern volatile struct empeg_state_t empeg_state;
#endif

#ifdef SLIMPROTO_DEBUG
FILE *debuglog = NULL;
bool debug_logfile = false;
#endif

#ifdef PADEV_WASAPI
bool wasapi_exclusive = true;
#endif

#ifdef RENICE
#ifdef EMPEG /* Always enabled for empeg */
bool renice = true;
#else
bool renice = false;
#endif /* EMPEG */
#endif /* RENICE */

#ifdef INTERACTIVE
struct lirc_config *lircconfig;
int linelen = 40;
char *lircrc;
int lirc_fd = 0;
bool using_lirc = false;
int using_curses = 0;
bool use_lcdd_menu = false;
bool lcdd_compat = false;
int lcd_fd = 0;
struct sockaddr_in *lcd_addr;
#endif

// There is not enough support in Windows+mingw to use signals in the
// implementation of the 'restart' feature.  So we support two implementations:
// One that is based on signals and is more responsive when a restart is 
// needed (USE_SIGNALS_FOR_RESTART defined).
// One that polls the restart flag once in a while: less responsive but
// portable (USE_SIGNALS_FOR_RESTART not defined).

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

#ifdef INTERACTIVE

// Used to toggle IR/LCD support on and off
static void install_toggle_handler() {
#ifndef __WIN32__
	signal(SIGUSR2, &toggle_handler);
#endif
}
#endif /* INTERACTIVE */

// Handles a signal coming from outside this process and that is meant to 
// terminate the program cleanly.
void exit_handler(int signal_number) {
	signal_exit_flag = true;
}

// Handles a signal coming from inside this process and that causes a restart
// of the Squeezebox Server connection.
void restart_handler(int signal_number) {
        if (retry_connection) {
		signal_restart_flag = true;
	} else {
 		signal_exit_flag = true;
	}
}

// Called by the library when the connection is either established or broken.
int connect_callback(slimproto_t *p, bool isConnected, void *user_data) {
#ifdef INTERACTIVE
	unsigned char msg[SLIMPROTO_MSG_SIZE];
#endif

	if (isConnected) {
		if (slimproto_helo(p, player_type, firmware, (char*) user_data, 0, 0) < 0) {
			fprintf(stderr, "Could not send helo to Squeezebox Server.\n");
		        send_restart_signal();
		}
#ifdef INTERACTIVE
	if ( using_curses || using_lirc || use_lcdd_menu )
	{
                memset(&msg, 0, SLIMPROTO_MSG_SIZE);
                packA4(msg, 0, "SETD");
                packN4(msg, 4, 2);
                packC(msg, 8, 0xfe);
                packC(msg, 9, linelen);
                slimproto_send(p, msg);
	}
#endif
	}
	else {
		// Send the restart signal, which calls restart_handler to tell
		// the main thread to go back waiting for Squeezebox Server to be
		// available.
		if (!signal_exit_flag)
		    send_restart_signal();
	}
	
	return 0;
}

int main(int argc, char *argv[]) {
	int exit_code = 0;
	slimproto_t slimproto;
	slimaudio_t slimaudio;

	PaDeviceIndex output_device_id = PA_DEFAULT_DEVICE;
	char *output_device_name = NULL;
	char *hostapi_name = NULL;

	unsigned int output_predelay = 0;
	unsigned int output_predelay_amplitude = 0;
#ifdef EMPEG
	bool power_bypass = false, power_last = false;
	bool geteq = false;
	long key, ir;
	slimaudio_volume_t volume_control = VOLUME_DRIVER;
#else
	slimaudio_volume_t volume_control = VOLUME_SOFTWARE;
#endif
	unsigned int retry_interval = RETRY_DEFAULT;

	char macaddress[6] = { 0, 0, 0, 0, 0, 1 };

	int keepalive_interval = -1;

	bool listdevs = false;
	bool listservers = false;
	bool discover_server = false;
	unsigned int json_port;

#ifdef ZONES
	bool default_macaddress = true;
	unsigned int zone = 0;
	unsigned int num_zones = 1;
#endif
#ifdef DAEMONIZE
	bool should_daemonize = false;
	char *logfile = NULL;
#endif
	char slimserver_address[INET_FQDNSTRLEN] = "127.0.0.1";

#ifdef INTERACTIVE
        fd_set read_fds;
        fd_set write_fds;
        int key = 0;
        unsigned long ir = 0;
	int maxfd = 0;
	char *home;
	struct timeval timeout;
	timeout.tv_usec = 0;

#ifdef __WIN32__
	int WSAerrno;
	int ptw32_processInitialize (void);
	ptw32_processInitialize();
#endif
        // default lircrc file ($HOME/.lircrc)
	home = getenv("HOME");
	if (home == NULL) home = "";
	lircrc = (char *)malloc((strlen(home) + strlen("/.lircrc") + 1) * sizeof(char));
	strcpy(lircrc,home);
	strcat(lircrc,"/.lircrc");
#endif

	char getopt_options[OPTLEN] = "a:FId:Y:e:f:hk:Lm:n:o:P:p:Rr:Vv:";
	static struct option long_options[] = {
		{"predelay_amplitude", required_argument, 0, 'a'},
		{"discover",           no_argument,       0, 'F'},
		{"debug",              required_argument, 0, 'd'},
		{"debuglog",           required_argument, 0, 'Y'},
		{"help",               no_argument,       0, 'h'},
		{"keepalive",          required_argument, 0, 'k'},
		{"list",               no_argument,       0, 'L'},
		{"findservers",        no_argument,       0, 'I'},
		{"mac",	               required_argument, 0, 'm'},
		{"name",               required_argument, 0, 'n'},
		{"output",             required_argument, 0, 'o'},
		{"playerid",           required_argument, 0, 'e'},
		{"firmware",           required_argument, 0, 'f'},
		{"port",               required_argument, 0, 'P'},
		{"predelay",           required_argument, 0, 'p'},
#ifdef EMPEG
		{"puteq",              no_argument,       0, 'Q'},
		{"geteq",              no_argument,       0, 'q'},
#endif
		{"retry",              no_argument,       0, 'R'},
		{"intretry",           required_argument, 0, 'r'},
		{"version",            no_argument,       0, 'V'},
		{"volume",             required_argument, 0, 'v'},
		{"zone",               required_argument, 0, 'z'},
#ifdef PORTAUDIO_DEV
		{"latency",            required_argument, 0, 'y'},
		{"audiotype",          required_argument, 0, 't'},
#endif
#ifdef DAEMONIZE
		{"daemonize",          required_argument, 0, 'M'},
#endif
#ifdef __WIN32__
		{"highpriority",       no_argument,       0, 'H'},
#ifdef PADEV_WASAPI
		{"shared",             no_argument,       0, 'S'},
#endif
#endif
#ifdef INTERACTIVE
		{"lircrc",             required_argument, 0, 'c'},
		{"lirc",               no_argument,       0, 'i'},
		{"lcd",                no_argument,       0, 'l'},
		{"lcdc",               no_argument,       0, 'C'},
		{"display",            no_argument,       0, 'D'},
		{"width",              required_argument, 0, 'w'},
#endif
#ifdef RENICE
		{"renice",             no_argument,       0, 'N'},
#endif
#ifdef ZONES
		{"zone",               required_argument, 0, 'z'},
#endif
		{0, 0, 0, 0}
	};
#ifdef EMPEG
	strcat (getopt_options, "Qq");
#endif
#ifdef PORTAUDIO_DEV
	strcat (getopt_options, "y:t:");
#endif	
#ifdef DAEMONIZE	
	strcat (getopt_options, "M:");
#endif
#ifdef INTERACTIVE
	strcat (getopt_options, "c:CDilw:");
#endif
#ifdef __WIN32__
	strcat (getopt_options, "H");
#ifdef PADEV_WASAPI
	strcat (getopt_options, "S");
#endif
#endif
#ifdef RENICE
	strcat (getopt_options, "N");
#endif
#ifdef ZONES
	strcat (getopt_options, "z:");
#endif
#ifdef EMPEG
	empeg_getmac(macaddress);
#endif

	while (true) {
		const char shortopt =
			getopt_long_only(argc, argv, getopt_options, long_options, NULL);

		if (shortopt == (char) -1) {
			break;
		}

		switch (shortopt) {
		case 'a':
			output_predelay_amplitude = strtoul(optarg, NULL, 0);
			break;
		case 'F':
			discover_server = true;
			break;
		case 'd':
#ifdef SLIMPROTO_DEBUG
			if (strcmp(optarg, "all") == 0)
			{
				slimproto_debug = true;
				slimaudio_debug = true;
				slimaudio_buffer_debug = true;
				slimaudio_buffer_debug_v = true;
				slimaudio_decoder_debug = true;
				slimaudio_decoder_debug_r = true;
				slimaudio_decoder_debug_v = true;
				slimaudio_http_debug = true;
				slimaudio_http_debug_v = true;
				slimaudio_output_debug = true;
				slimaudio_output_debug_v = true;
			}
			else if (strcmp(optarg, "slimproto") == 0)
				slimproto_debug = true;
			else if (strcmp(optarg, "slimaudio") == 0)
				slimaudio_debug = true;
			else if (strcmp(optarg, "slimaudio_buffer") == 0)
				slimaudio_buffer_debug = true;
			else if (strcmp(optarg, "slimaudio_buffer_v") == 0)
				slimaudio_buffer_debug_v = true;
			else if (strcmp(optarg, "slimaudio_decoder") == 0)
				slimaudio_decoder_debug = true;
			else if (strcmp(optarg, "slimaudio_decoder_r") == 0)
				slimaudio_decoder_debug_r = true;
			else if (strcmp(optarg, "slimaudio_decoder_v") == 0)
				slimaudio_decoder_debug_v = true;
			else if (strcmp(optarg, "slimaudio_http") == 0)
				slimaudio_http_debug = true;
			else if (strcmp(optarg, "slimaudio_http_v") == 0)
				slimaudio_http_debug_v = true;
			else if (strcmp(optarg, "slimaudio_output") == 0)
				slimaudio_output_debug = true;
			else if (strcmp(optarg, "slimaudio_output_v") == 0)
				slimaudio_output_debug_v = true;
			else
				fprintf(stderr, "%s: Unknown debug option %s\n", argv[0], optarg);
#else
				fprintf(stderr, "%s: Recompile with -DSLIMPROTO_DEBUG to enable debugging.\n", argv[0]);
#endif
			break;
		case 'Y':
#ifdef SLIMPROTO_DEBUG
                        if ( optarg == NULL )
                        {
                                fprintf(stderr, "%s: Cannot parse debug log filename %s\n", argv[0], optarg);
                                exit(-1);
                        } else
                        {
				debuglog = freopen( optarg, "a", stderr);
				if ( debuglog )
					debug_logfile = true;
				else
					fprintf(stderr, "%s: Redirection of stderr to %s failed.\n", argv[0], optarg);
                        }
#endif
			break;

// From server/Slim/Networking/Slimproto.pm from 7.5r28596
// squeezebox(2)
// softsqueeze(3)
// squeezebox2(4)
// transporter(5)
// softsqueeze3(6)
// receiver(7)
// squeezeslave(8)
// controller(9)
// boom(10)
// softboom(11)
// squeezeplay(12)
// radio(13)
// touch(14)

		case 'e':
			player_type = strtoul(optarg, NULL, 0);
			if ( (player_type < 2) || (player_type > 14) )
			{
				player_type = PLAYER_TYPE;
				fprintf(stderr, "%s: Unknown player type, using (%d)\n", argv[0], player_type);
			}
			break;
		case 'f':
			firmware = strtoul(optarg, NULL, 0);
			if ( (firmware < 0) || (firmware > 254) )
			{
				firmware = FIRMWARE_VERSION;
				fprintf(stderr, "%s: Invalid firmware value, using (%d)\n", argv[0], firmware);
			}
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
#ifdef ZONES
			default_macaddress = false;
#endif
			break;
#ifdef DAEMONIZE
		case 'M':
			if ( optarg == NULL )
			{
				fprintf(stderr, "%s: Cannot parse log filename %s\n", argv[0], optarg);
				exit(-1);	
			} else
			{
				logfile = optarg;
			}
			should_daemonize = true;
			break;
#endif

#ifdef __WIN32__
		case 'H':
			/* Change Window process priority class to HIGH */
			if ( !SetPriorityClass ( GetCurrentProcess(), HIGH_PRIORITY_CLASS ) )
			{
				int dwError = GetLastError();
				fprintf(stderr, "%s: Failed to set priority (%d), using default.\n", argv[0],
					dwError);
			} 
			break;
#ifdef PADEV_WASAPI
		case 'S':
			wasapi_exclusive = false;
			break;
#endif
#endif
#ifdef RENICE
		case 'N':
			renice = true;
			break;
#endif
		case 'n':
			output_device_name = optarg;
			output_change = true;
			break;
		case 'o':
			output_device_id = strtoul(optarg, NULL, 0);
			output_change = true;
			break;
		case 'p':
			output_predelay = strtoul(optarg, NULL, 0);
			break;
		case 'P':
			port = strtoul(optarg, NULL, 0);
			if ( (port < 0) || (port > 65535) )
			{
				port = SLIMPROTOCOL_PORT;
				fprintf(stderr, "%s: Invalid port number, using %d.\n", argv[0], port);
			}
			break;
			break;
#ifdef EMPEG
		case 'Q':
			empeg_puteq_tofile();
			exit(0);
			break;
		case 'q':
			geteq = true;
			break;
#endif
		case 'R':
			retry_connection = true;
			break;
		case 'r':
			retry_connection = true;
			retry_interval = strtoul(optarg, NULL, 0);
			if ( ( retry_interval < 1 ) || ( retry_interval > 120 ) )
			{
				retry_interval = RETRY_DEFAULT;
				fprintf (stderr, "Invalid retry interval, using %d seconds.\n", retry_interval );
			}
			break;
#ifdef INTERACTIVE
		case 'c':
		        free(lircrc);
			lircrc = optarg;
			break;
#ifndef __WIN32__
		case 'i':
			using_lirc = true;
			break;
		case 'l':
			use_lcdd_menu = true;
			break;
		case 'C':
			use_lcdd_menu = true;
			lcdd_compat = true;
			break;
#endif
		case 'D':
			using_curses = 1;
			break;
		case 'w':
			linelen = strtoul(optarg, NULL, 0);
			break;
#endif
		case 'L':
			listdevs = true;
			break;
		case 'I':
			listservers = true;
			break;
		case 'V':
			print_version();
			exit(0);
			break;
		case 'v':
		 	if (strcmp(optarg, "sw") == 0) {
				volume_control = VOLUME_SOFTWARE;
			}
#ifndef PORTAUDIO_DEV
			else if (strcmp(optarg, "on") == 0 ) {
				volume_control = VOLUME_DRIVER;
			}
#endif
			else if (strcmp(optarg, "off") == 0 ) {
				volume_control = VOLUME_NONE;
			}
			break;
#ifdef PORTAUDIO_DEV
		case 'y':
			modify_latency = true;
			user_latency = strtoul(optarg, NULL, 0);

			if ( user_latency > 1000 )
			{
				fprintf (stderr, "Suggested latency invalid, using device default.\n");
				modify_latency = false;
			}
			break;
		case 't':
			hostapi_name = optarg;
			break;
#endif
#ifdef ZONES
		case 'z':
			if (sscanf(optarg, "%u/%u", &zone, &num_zones) != 2)
			{
				fprintf (stderr, "Invalid zone specification, using default.\n");
			}
			if (num_zones > MAX_ZONES)
			{
				fprintf(stderr, "Number of zones > %d not supported\n", MAX_ZONES);
				zone=0;
				num_zones=1;
			}
			if (num_zones <= zone)
			{
				fprintf (stderr, "Invalid zone specification, using default.\n");
				zone = 0;
				num_zones = 1;
			}
			break;
#endif
		default:
			break;
		}
	}

	if (listdevs) {
		GetAudioDevices(output_device_id, output_device_name, hostapi_name, output_change, true);
		exit(0);
	}

	if (listservers) {
		slimproto_discover(slimserver_address, sizeof(slimserver_address), port, &json_port, true);
		exit(0);
	}

	if (optind < argc)
		strncpy(slimserver_address, argv[optind], sizeof(slimserver_address));

#ifdef DAEMONIZE
	if ( should_daemonize ) {
#ifdef INTERACTIVE
		if ( using_curses || use_lcdd_menu )
		{
			fprintf(stderr, "Daemonize not supported with display modes.\n");
			exit(-1);
		}
		else
#endif
			init_daemonize();
	}
#endif
	signal(SIGTERM, &exit_handler);
	signal(SIGINT, &exit_handler);
	install_restart_handler();

#ifdef INTERACTIVE
	install_toggle_handler();  //SIGUSR2 to toggle IR/LCD on and off
#endif
	if (slimproto_init(&slimproto) < 0) {
		fprintf(stderr, "Failed to initialize slimproto\n");
		exit(-1);	
	}

#ifdef ZONES
	if (slimaudio_init(&slimaudio, &slimproto, output_device_id, output_device_name,
		hostapi_name, output_change, zone, num_zones) < 0)
#else
	if (slimaudio_init(&slimaudio, &slimproto, output_device_id, output_device_name,
		hostapi_name, output_change) < 0)
#endif
	{
		fprintf(stderr, "Failed to initialize slimaudio\n");
		exit(-1);
	}
#ifdef ZONES
	if (default_macaddress)
		macaddress[5] += zone;
#endif
	slimproto_add_connect_callback(&slimproto, connect_callback, macaddress);

#ifdef INTERACTIVE
	// Process VFD (display) commands
	if ( using_curses || using_lirc || use_lcdd_menu )
		slimproto_add_command_callback(&slimproto, "vfdc", vfd_callback, macaddress);
#endif
#ifdef EMPEG
	slimproto_add_command_callback(&slimproto, "grfe", empeg_vfd_callback, macaddress);
	slimproto_add_command_callback(&slimproto, "grfb", empeg_vfdbrt_callback, macaddress);
	slimproto_add_command_callback(&slimproto, "aude", empeg_aude_callback, macaddress);
#endif

	slimaudio_set_volume_control(&slimaudio, volume_control);
	slimaudio_set_output_predelay(&slimaudio, output_predelay, output_predelay_amplitude);

	if (keepalive_interval >= 0) {
		slimaudio_set_keepalive_interval(&slimaudio, keepalive_interval);
	}

#ifdef INTERACTIVE
	init_lcd();
#endif
#ifdef EMPEG
	empeg_init();
	if (geteq)
	   empeg_geteq_fromfile();
	power_last = empeg_state.power_on;
	empeg_state.power_on = false;
#endif

	if (slimaudio_open(&slimaudio) < 0) {
		fprintf(stderr, "Failed to open slimaudio\n");
		exit_code = -1;
		goto exit;
	}

#ifdef SLIMPROTO_DEBUG
	if (slimaudio_debug)
		fprintf ( stderr, "Using audio device index: %d\n", slimaudio.output_device_id );
#endif

#ifdef INTERACTIVE
        init_lirc();

	setlocale(LC_ALL, "");
	initcurses();
#endif
#ifdef DAEMONIZE
	if ( should_daemonize ) {
		daemonize(logfile);
	}
#endif
	// When retry_connection is true, retry connecting to Squeezebox Server 
	// until we succeed, unless the signal handler tells us to give up.
	do {
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
#ifdef EMPEG
		if (discover_server && empeg_state.last_server[0] != '\0')
		{
			strcpy(slimserver_address, (char *)empeg_state.last_server);
			empeg_state.last_server[0] = '\0';
		}
		else
#endif
		if (discover_server && slimproto_discover(slimserver_address,
			sizeof(slimserver_address), port, &json_port, false) < 0)
		{
			fprintf(stderr,"Discover failed.\n");
			if (!retry_connection) {
				exit_code = -1;
				goto exit;
			}
			signal_restart_flag = true;
			continue;
		}

		if (slimproto_connect(
			&slimproto, slimserver_address, port) < 0) {
			fprintf(stderr, "Connection to Squeezebox Server %s failed.\n", slimserver_address);
			if (!retry_connection) {
				exit_code = -1;
				goto exit;
			}
			signal_restart_flag = true;
			continue;
		}
		signal_restart_flag = false;
		discover_server = false;
#ifdef EMPEG
		strcpy((char *)empeg_state.last_server, slimserver_address);
		if (power_last)
			while (!empeg_state.power_on)
			{
				Pa_Sleep(100);
				slimproto_ir(&slimproto, 1, 1, 0x76898F70);
			}
#endif
                while (!signal_exit_flag && !signal_restart_flag) {
#ifdef EMPEG
		   int rc = empeg_idle();
		   if (power_bypass)
		   {
		      if (rc == 0 || !empeg_state.power_on)
		      {
		         power_last = false;
		         power_bypass = false;
		      }
		   }
		   else if (rc == -1)
		   {
		      fprintf(stderr, "Power loss detected.\n");
		      power_last = empeg_state.power_on;
                      slimproto_ir(&slimproto, 1, 1, 0x76898778);
		      while (empeg_state.power_on)
		         Pa_Sleep(250);
		   }
		   else if (rc == -2 && empeg_state.power_on)
		   {
		      fprintf(stderr, "Manual override, aborting power down.\n");
		      power_bypass = true;
		   }
		   else if (rc == -3)
		   {
		      fprintf(stderr, "Power restored.\n");
		      if (power_last)
                         slimproto_ir(&slimproto, 1, 1, 0x76898F70);
		   }
		   else if (rc == -4)
		   {
		      fprintf(stderr, "Powering down.\n");
		      slimproto_goodbye(&slimproto, 0x00);
		      Pa_Sleep(400);
		      slimproto_close(&slimproto);
		      empeg_state.power_on = power_last;
		      empeg_poweroff();
		      signal_restart_flag = true;
		   }
#endif
#ifdef INTERACTIVE
                   if (using_curses == 1 || use_lcdd_menu || using_lirc) {
                      FD_ZERO(&read_fds);
                      FD_ZERO(&write_fds);
		      if (using_curses == 1)
     	                 FD_SET(0, &read_fds); /* watch stdin */
   	              if  (use_lcdd_menu) {
		         FD_SET(lcd_fd, &read_fds); 
		         maxfd = lcd_fd;
		      }
   	              if (using_lirc) {
		         FD_SET(lirc_fd, &read_fds); 
                         if (lirc_fd > maxfd) 
		            maxfd = lirc_fd;
                      }
		      timeout.tv_sec = 5;
                      if(select(maxfd + 1, &read_fds, NULL, NULL, &timeout) == -1) {
#ifndef __WIN32__
    	                 if (errno != EINTR)
			 {
		           fprintf(stderr,"Select Error:%d\n", errno);
#else
			 WSAerrno = WSAGetLastError();
			 if ( (WSAerrno != WSAEINTR) && (WSAerrno != WSAENOTSOCK) )
			 {	
		           fprintf(stderr,"Select Error:%d\n", WSAerrno);
#endif
   	                   abort();
	                 }
#ifdef __WIN32__
			 else
				 WaitForSingleObject( GetStdHandle(STD_INPUT_HANDLE), 5000 );
#endif
                      }
		      if (FD_ISSET(0, &read_fds)) {
                         while ((key = getch()) != ERR) {
                            ir = getircode(key);
	                    if (ir == (unsigned long) 0x01) {
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
#ifdef EMPEG
                   while ((key = empeg_getkey()) != -1)
                   {
                      ir = empeg_getircode(key);
                      if (ir != 0)
                         slimproto_ir(&slimproto, 1, 1, ir);
                   }
#else
                   wait_for_restart_signal();
#endif
#endif
		}
#ifdef INTERACTIVE
                FD_ZERO(&read_fds);
                FD_ZERO(&write_fds);
#endif
        } while (signal_restart_flag && !signal_exit_flag);

exit:
	slimaudio_close(&slimaudio);

	slimproto_goodbye(&slimproto, 0x00);

	/* Wait 200ms for BYE! message send to complete */
	Pa_Sleep(200);

	slimproto_close(&slimproto);

#ifdef INTERACTIVE
	exitcurses();
	close_lirc();
#endif

#if defined(EMPEG) || defined(INTERACTIVE)
	close_lcd();
#endif

#ifdef SLIMPROTO_DEBUG
	if (debug_logfile)
	{
		fclose (debuglog);
	}
#endif

	slimproto_destroy(&slimproto);
	slimaudio_destroy(&slimaudio);

	return exit_code;
} 

