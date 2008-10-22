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
#include <string.h>
#include <getopt.h>
#include <signal.h>

#include "slimproto/slimproto.h"
#include "slimaudio/slimaudio.h"

static int connect_callback(slimproto_t *p, bool isConnected, void *user_data);
static int parse_macaddress(char *macaddress, const char *str);
static void print_version();
static void print_help();
static void exit_handler(int signal_number);
static void restart_handler(int signal_number);

static volatile bool signal_exit_flag = false;
static volatile bool signal_restart_flag = false;
static const char* version = "0.8-23425-8";

static int player_type = 8;

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

int main(int argc, char *argv[]) {
	slimproto_t slimproto;
	slimaudio_t slimaudio;
	char macaddress[6] = { 0, 0, 0, 0, 0, 1 };
	int output_device_id = -1;
	bool use_signal_to_exit = false;
	bool retry_connection = false;
	slimaudio_volume_t volume_control = VOLUME_SOFTWARE;
	unsigned int output_predelay = 0;
	unsigned int output_predelay_amplitude = 0;
	unsigned int retry_interval = 5;
	int keepalive_interval = -1;
	
	while (true) {
		static struct option long_options[] = {
			{"predelay_amplitude", required_argument, 0, 'a'},
			{"debug",              required_argument, 0, 'd'},
			{"help",               no_argument,       0, 'h'},
			{"keepalive",          required_argument, 0, 'k'},
			{"mac",	               required_argument, 0, 'm'},
			{"output",             required_argument, 0, 'o'},
			{"oldplayer",          no_argument,       0, 'O'},
			{"predelay",           required_argument, 0, 'p'},
			{"retry",              optional_argument, 0, 'r'},
			{"signal",             no_argument,       0, 's'},
			{"version",            no_argument,       0, 'V'},
			{"volume",             required_argument, 0, 'v'},
			{0, 0, 0, 0}
		};
	
		const char shortopt =
#ifdef GETOPT_SUPPORTS_OPTIONAL
			getopt_long_only(argc, argv, "a:d:hk:m:Oo:p:r::sVv:",
#else
			getopt_long_only(argc, argv, "a:d:hk:m:Oo:p:r:sVv:",
#endif
					 long_options, NULL);
	
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
		case 'r':
			retry_connection = true;
			if (optarg != NULL) {
				fprintf( stderr, "Got retry opt arg %s\n", optarg );
				retry_interval = strtoul(optarg, NULL, 0);
			}
			break;
		case 's':
			use_signal_to_exit = true;
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
		}
	}

	char *slimserver_address = "127.0.0.1";
	if (optind < argc)
		slimserver_address = argv[optind];
	
	if (use_signal_to_exit) {
		signal(SIGTERM, &exit_handler);
	}

	if (retry_connection) {
		install_restart_handler();
	}

	if (slimproto_init(&slimproto) < 0) {
		fprintf(stderr, "Failed to initialize slimproto\n");
		exit(-1);	
	}

	if (slimaudio_init(&slimaudio, &slimproto) < 0) {
		fprintf(stderr, "Failed to initialize slimaudio\n");
		exit(-1);
	}

	slimproto_add_connect_callback(&slimproto, connect_callback, 
					macaddress);

	int num_devices;
	char **devices;
	slimaudio_get_output_devices(&slimaudio, &devices, &num_devices);
	
	printf("Output devices:\n");	
	int i;
	for (i=0; i<num_devices; i++) {
		printf("%i: %s\n", i, devices[i]);
	}

	if (output_device_id >= 0) {
		slimaudio_set_output_device(&slimaudio, output_device_id);
	}

	slimaudio_set_volume_control(&slimaudio, volume_control);
	slimaudio_set_output_predelay(&slimaudio, output_predelay,
				      output_predelay_amplitude);

	if (keepalive_interval >= 0) {
		slimaudio_set_keepalive_interval(&slimaudio, keepalive_interval);
	}

	if (slimaudio_open(&slimaudio) < 0) {
		fprintf(stderr, "Failed to open slimaudio\n");
		exit(-1);
	}

	// When retry_connection is true, retry connecting to the SqueezeCenter 
	// until we succeed, unless the signal handler tells us to give up.
	do {
		while (slimproto_connect(
			&slimproto, slimserver_address, 3483) < 0) {
			if (!retry_connection || signal_exit_flag) {
				if (signal_exit_flag) {
					// No message when the exit is triggered
					// by the user.
					exit(0);
				}
				fprintf(stderr, 
					"Connection to SqueezeCenter %s failed.\n",
					slimserver_address);
				exit(-1);
			}
			fprintf(stderr,"Sleeping for %d s.\n", retry_interval);
			Pa_Sleep(1000 * retry_interval);
		}

		if (use_signal_to_exit) {
			signal_restart_flag = false;
			while (!signal_exit_flag && !signal_restart_flag) {
				wait_for_restart_signal();
			}
		}
	} while (signal_restart_flag && !signal_exit_flag);

	if (!use_signal_to_exit) {
		getc(stdin);
#if 0
	int ch = getc(stdin);
	// This does not work on all platforms and I don't have time to sort it out right
	// now. getc(stdin) starts reading from the slimproto connection after the user 
	// presses return.
	while (ch != 'q') {
		printf("ch %d %d\n", ch, stdin);
		switch (ch) {
		case 'z': // rewind
			slimproto_ir(&slimproto, 1, 1, 0x7689c03f);
			break;

		case 'b': // forward
			slimproto_ir(&slimproto, 1, 1, 0x7689a05f);
			break;

		case 'c': // pause
			slimproto_ir(&slimproto, 1, 1, 0x768920df);
			break;

		case '+': // volume up
			slimproto_ir(&slimproto, 1, 1, 0x7689807f);
			break;

		case '-': // volume down
			slimproto_ir(&slimproto, 1, 1, 0x768900ff);
			break;

		case 'm': // volume mute
			slimproto_ir(&slimproto, 1, 1, 0x0000c038);
			break;

		case 'x': // play
			slimproto_ir(&slimproto, 1, 1, 0x768910ef);
			break;
		}
		
		ch = getc(stdin);
	}
#endif
	}

	slimaudio_close(&slimaudio);
	slimproto_close(&slimproto);
	
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
"-h, --help:                     Prints this message.\n"
"-a, --predelay_amplitude <val>: Sets the amplitude of a high-frequency tone\n"
"                                produced during the predelay (see --predelay).\n"
"                                The frequency is set at the source's sampling\n"
"                                rate/2 and the amplitude is in absolute value.\n"
"                                For 16-bit sources, the max is 32767, but values\n"
"                                below 10 are likely to work.  The goal is to\n"
"                                produce an inaudible signal that will cause DACs\n"
"                                to wake-up and lock before actual samples are\n"
"                                played out.  If the DAC locks using only silence,\n"
"                                do not use this option (it will default to 0).\n"
"-k, --keepalive <sec>:          Controls how frequently squeezeslave sends a\n"
"                                keepalive signal to SqueezeCenter.  6.5.x servers\n"
"                                need this to avoid dropping the player's\n"
"                                connection.  By default, the implementation\n"
"                                chooses the right value: 10s for a >=6.5.x server\n"
"                                and 0s for a <6.5.xserver, which means no\n"
"                                keepalive.\n"
"-m, --mac <mac_address>:        Sets the mac address for this instance.\n"
"                                Use the colon-separated notation.\n"
"                                The default is 00:00:00:00:01.\n"
"                                SqueezeCenter uses this value to distinguish\n"
"                                multiple instances, allowing per-player settings.\n"
"-O, --oldplayer:                Uses an old player-type-id mostly compatible with\n"
"                                pre-7.0 SqueezeCenter\n"
"-o, --output <device_id>:       Sets the output device id.\n"
"                                The default id is 0.\n"
"                                The output device ids are enumerated at startup.\n"
"-p, --predelay <msec>:          Sets a delay before any playback is started.  This\n"
"                                is useful if the DAC used for output is slow to\n"
"                                wake-up/lock, causing the first few samples to be\n"
"                                dropped.\n"
"-r, --retry [sec]:              Causes the program to retry connecting to\n"
"                                SqueezeCenter until it succeeds or is stopped using\n"
"                                SIGTERM or keyboard entry (see -s/--signal).\n"
"                                If the connection to SqueezeCenter is lost, the\n"
"                                program will poll it until it restarts.  The\n"
"                                optional value specifies the interval between\n"
"                                retries and defaults to 5s.\n"
"-s, --signal:                   Causes the program to wait for SIGTERM to exit.\n"
"                                The default is to wait for a keyboard entry, which\n"
"                                prevents the program from running in background.\n"
"-V, --version:                  Prints the squeezeslave version.\n"
"-v, --volume <on|sw|off>:       Enables/disables volume changes done by\n"
"                                SqueezeCenter during its operation, such as when\n"
"                                changing the volume through the web interface or\n"
"                                when applying replay gain.  Defaults to sw.\n"
"                                        on:  volume changes performed on device.\n"
"                                        sw:  volume changes performed in software.\n"
"                                        off: volume changes ignored.\n"
"-d, --debug <trace_name>:       Turns on debug tracing for the specified level.\n"
"                                The option can be used multiple times to enable\n"
"                                multiple levels.\n"
"                                Available levels:\n"
#ifndef SLIMPROTO_DEBUG
"                                (disabled, recompile with \"-DSLIMPROTO_DEBUG\")\n"
#endif
"                                        slimproto\n"
"                                        slimaudio\n"
"                                        slimaudio_buffer\n"
"                                        slimaudio_buffer_v\n"
"                                        slimaudio_decoder\n"
"                                        slimaudio_decoder_v\n"
"                                        slimaudio_http\n"
"                                        slimaudio_http_v\n"
"                                        slimaudio_output\n");
}

// Handles a signal coming from outside this process and that is meant to 
// terminate the program cleanly.
static void exit_handler(int signal_number) {
	signal_exit_flag = true;
}

// Handles a signal coming from inside this process and that causes a restart
// of the SqueezeCenter connection.
static void restart_handler(int signal_number) {
	signal_restart_flag = true;
}

// Called by the library when the connection is either established or broken.
static int connect_callback(slimproto_t *p, bool isConnected, void *user_data) {
	if (isConnected) {
		if (slimproto_helo(p, player_type, 0, (char*) user_data, 1, 0) < 0) {
			fprintf(stderr, "Could not send helo to SqueezeCenter\n");
			exit(-1);	
		}
	}
	else {
		// Send the restart signal, which calls restart_handler to tell
		// the main thread to go back waiting for SqueezeCenter to be
		// available.
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
