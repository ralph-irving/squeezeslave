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

#include "squeezeslave.h"

extern char* version;

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

int parse_macaddress(char *macaddress, const char *str) {
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

void print_version(void) {
	fprintf(stdout, "squeezeslave %s\n", version);
}

void print_help(void) {
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
"                            and 0s for a <6.5.x server, which means no\n"
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
"-w, --width <chars>         Set the display width to <chars> characters\n"
"                            If using LCDd, width is detected.\n"
#endif
#ifdef DAEMONIZE
"-M, --daemonize <logfile>   Run squeezeslave as a daemon.\n"
"                            Messages written to specified file.\n"
#endif
"-L, --list                  List available audio devices and exit.\n"
"-m, --mac <mac_address>:    Sets the mac address for this instance.\n"
"                            Use the colon-separated notation.\n"
"                            The default is 00:00:00:00:00:01.\n"
"                            SqueezeCenter uses this value to distinguish\n"
"                            multiple instances, allowing per-player settings.\n"
"-e, --playerid:             Pretend to be the player-type-id specified.  Some\n"
"                            types may require -m00:04:20:00:00:01 as well.\n"
"                                softsqueeze\n"
"                                squeezebox2\n"
"                                transporter\n"
"                                softsqueeze3\n"
"                                receiver\n"
"                                controller\n"
"                                boom\n"
"                                softboom\n"
"                                squeezeplay\n"
"-O, --oldplayer:            Emulate the player-type-id softsqueeze for use with\n"
"                            SqueezeCenter versions older than 7.0.\n"
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
"-s, --signal:               Ignored. Always uses SIGTERM to exit.\n"
"-V, --version:              Prints the squeezeslave version.\n"
"-v, --volume <on|sw|off>:   Enables/disables volume changes done by\n"
"                            SqueezeCenter during its operation, such as when\n"
"                            changing the volume through the web interface or\n"
"                            when applying replay gain.  Defaults to sw.\n"
"                                    on:  volume changes performed on device.\n"
"                                    sw:  volume changes performed in software.\n"
"                                    off: volume changes ignored.\n"
#ifdef SLIMPROTO_DEBUG
"-Y, --debuglog <logfile>:   Redirect debug output from stderr to <logfile>.\n"
#endif
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

