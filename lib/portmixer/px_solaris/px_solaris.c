/*
 * PortMixer
 * Unix Solaris Implementation
 *
 * Copyright (c) 2004 Sun Microsystems
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: Redistribution of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * Redistribution in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * Neither the name of Sun Microsystems, Inc. or the names of
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission. This software
 * is provided "AS IS," without a warranty of any kind.  ALL EXPRESS OR
 * IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES, INCLUDING ANY
 * IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
 * OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED. SUN MIDROSYSTEMS, INC.
 * ("SUN") AND ITS LICENSORS SHALL NOT BE LIABLE FOR ANY DAMAGES SUFFERED
 * BY LICENSEE AS A RESULT OF USING, MODIFYING OR DISTRIBUTING THIS
 * SOFTWARE OR ITS DERIVATIVES. IN NO EVENT WILL SUN OR ITS LICENSORS BE
 * LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA, OR FOR DIRECT, INDIRECT,
 * SPECIAL, CONSEQUENTIAL, INCIDENTAL OR PUNITIVE DAMAGES, HOWEVER CAUSED
 * AND REGARDLESS OF THE THEORY OF LIABILITY, ARISING OUT OF THE USE OF
 * OR INABILITY TO USE THIS SOFTWARE, EVEN IF SUN HAS BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES. You acknowledge that this software is not
 * designed, licensed or intended for use in the design, construction,
 * operation or maintenance of any nuclear facility.
 *
 * Written by Stephen Uhler
 */

#include <sys/audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include "portaudio.h"
#include "portmixer.h"

/* Only support the primary audio device for now */

#define AUDIO_CTL "/dev/audioctl"

#define BETWEEN(a, x, b)	((x)<(a) ? (a) : ((x)>(b) ? (b) : (x)))

typedef struct PxInfo {
   int fd;		/* file descriptor */
   PxVolume master;	/* master volume scaler, starts at 1.0 [not used yet] */
} PxInfo;

/*
 Px_GetNumMixers returns the number of mixers which could be
 used with the given PortAudio device.  On most systems, there
 will be only one mixer for each device; however there may be
 multiple mixers for each device, or possibly multiple mixers
 which are independent of any particular PortAudio device.
*/

int Px_GetNumMixers( void *pa_stream ) {
   return 1;
}

const char *Px_GetMixerName( void *pa_stream, int i ) {
   return "default";
}

/*
 Px_OpenMixer() returns a mixer which will work with the given PortAudio
 audio device.  Pass 0 as the index for the first (default) mixer.
*/

PxMixer *Px_OpenMixer( void *pa_stream, int i ) {
   struct audio_device device;
   PxInfo *info;
   int fd;

   if (i != 0) {
      return NULL;
   }

   /* AUDIODEV is defined for sunrays */

   if (getenv("AUDIODEV") != NULL) {
       char buff[255];	/* audio control device */
       snprintf(buff, sizeof(buff), "%s%s", getenv("AUDIODEV"), "ctl");
       fd = open(buff, O_RDWR);
   } else {
       fd = open(AUDIO_CTL, O_RDWR);
   }

   if (fd < 0) {
       perror("audio device won't open");
       return NULL;
   }

   /* make sure we're an audio devoce */

   if (ioctl(fd, AUDIO_GETDEV, &device) < 0) {
       perror("Bogus audio device");
   }
   fprintf(stderr, "Mixer using audio control device: %s\n", device.name);

   info = (PxInfo *)malloc(sizeof(PxInfo));
   info->fd = fd;
   info->master = 1.0;
   return (PxMixer *)info;
}

/*
 Px_CloseMixer() closes a mixer opened using Px_OpenMixer and frees any
 memory associated with it. 
*/

void Px_CloseMixer(PxMixer *mixer) {
   PxInfo *info = (PxInfo *)mixer;
   close(info->fd);
   free(info);
}

/*
 Master (output) volume
*/

PxVolume Px_GetMasterVolume( PxMixer *mixer ) {
   PxInfo *info = (PxInfo *)mixer;
   return info->master;
}
void Px_SetMasterVolume( PxMixer *mixer, PxVolume volume ) {
   PxInfo *info = (PxInfo *)mixer;
   info->master = BETWEEN(0.0, volume, 1.0);
}

/*
 Main output volume
*/

PxVolume Px_GetPCMOutputVolume( PxMixer *mixer ) {
    return (PxVolume) 0;
}
void Px_SetPCMOutputVolume( PxMixer *mixer, PxVolume volume ) {
    return;
}
int Px_SupportsPCMOutputVolume( PxMixer* mixer )  {
    return 0;
}

/*
 All output volumes
*/

int Px_GetNumOutputVolumes( PxMixer *mixer ) {
    return 1;
}
const char *Px_GetOutputVolumeName( PxMixer *mixer, int i ) {
    return "all";
}
PxVolume Px_GetOutputVolume( PxMixer *mixer, int i ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   PxVolume result = (PxVolume) 0.0;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
       return result;
   }
   result =  (PxVolume) ((float)audio.play.gain / AUDIO_MAX_GAIN);
   return result;
}

void Px_SetOutputVolume( PxMixer *mixer, int i, PxVolume volume ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
   }
   audio.play.gain =  AUDIO_MAX_GAIN * BETWEEN(0.0, (float)volume, 1.0);
   if (ioctl(info->fd,AUDIO_SETINFO,&audio) < 0)  {
       perror("Bad audio");
   }
}

/*
 Input source - support only "mic" and "line" for now.
*/

int Px_GetNumInputSources( PxMixer *mixer ) {
   return 2;
}

const char *Px_GetInputSourceName( PxMixer *mixer, int i) {
   switch (i) {
       case 0: return "microphone";
       case 1: return "line";
   }
   return "unsupported";
}

int Px_GetCurrentInputSource( PxMixer *mixer ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   int port;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
       return 0;
   }
   port = audio.record.port;
   if (port == AUDIO_MICROPHONE) {
      return 0;
   } else if (port == AUDIO_LINE_IN) {
      return 1;
   } else {
      return -1;
   }
}

void Px_SetCurrentInputSource( PxMixer *mixer, int i ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   int port;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
       return;
   }
   switch (i) {
       case 0:	audio.record.port = AUDIO_MICROPHONE;
       case 1:	audio.record.port = AUDIO_LINE_IN;
   }
   if (ioctl(info->fd,AUDIO_SETINFO,&audio) < 0)  {
       perror("Bad audio");
   }
}

/*
 Input volume
*/

PxVolume Px_GetInputVolume( PxMixer *mixer ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   PxVolume result = (PxVolume) 0.0;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
       return result;
   }
   result =  (PxVolume) ((float)audio.record.gain / AUDIO_MAX_GAIN);
   return result;
}

void Px_SetInputVolume( PxMixer *mixer, PxVolume volume ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
   }
   audio.record.gain =  AUDIO_MAX_GAIN * BETWEEN(0.0, (float)volume, 1.0);
   if (ioctl(info->fd,AUDIO_SETINFO,&audio) < 0)  {
       perror("Bad audio");
   }
}

/*
  Balance
*/

int Px_SupportsOutputBalance( PxMixer *mixer ) {
    return 1;
}
PxBalance Px_GetOutputBalance( PxMixer *mixer ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   PxBalance result = (PxBalance) 0.0;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
       return result;
   }
   result =  (PxBalance) ((float)(audio.play.balance-32) / 32.0);
   return result;
}

void Px_SetOutputBalance( PxMixer *mixer, PxBalance balance ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   PxBalance result = (PxBalance) 0.0;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
       return;
   }
   audio.play.balance = ((float)balance * 32.0) + 32;
   if (ioctl(info->fd,AUDIO_SETINFO,&audio) < 0)  {
       perror("Bad audio");
   }
}

/*
  Playthrough
*/

int Px_SupportsPlaythrough( PxMixer *mixer ) {
   return 1;
}
PxVolume Px_GetPlaythrough( PxMixer *mixer ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   PxVolume result = (PxVolume) 0.0;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
       return result;
   }
   result =  (PxBalance) ((float)audio.monitor_gain /  AUDIO_MAX_GAIN);
   return result;
}
void Px_SetPlaythrough( PxMixer *mixer, PxVolume volume ) {
   struct audio_info audio;
   PxInfo *info = (PxInfo *)mixer;
   if (ioctl(info->fd,AUDIO_GETINFO,&audio) < 0)  {
       perror("Bad audio");
   }
   audio.monitor_gain =  AUDIO_MAX_GAIN * BETWEEN(0.0, (float)volume, 1.0);
   if (ioctl(info->fd,AUDIO_SETINFO,&audio) < 0)  {
       perror("Bad audio");
   }
}


/*
  unimplemented stubs
*/

int Px_SetMicrophoneBoost( PxMixer* mixer, int enable )
{
	return 1 ;
}

int Px_GetMicrophoneBoost( PxMixer* mixer )
{
	return -1 ;
}

int Px_SetCurrentInputSourceByName( PxMixer* mixer, const char* line_name )
{
	return 1 ;
}

