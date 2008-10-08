/*
 * PortMixer
 * Mac OS X / CoreAudio implementation
 *
 * Copyright (c) 2002
 *
 * Written by Dominic Mazzoni
 *
 * PortMixer is intended to work side-by-side with PortAudio,
 * the Portable Real-Time Audio Library by Ross Bencina and
 * Phil Burk.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <math.h>
#include <DSound.h>

#include "portaudio.h"
#include "pa_host.h"
#include "portmixer.h"
#include "dsound_wrapper.h"

typedef enum PaDeviceMode
{
    PA_MODE_OUTPUT_ONLY,
    PA_MODE_INPUT_ONLY,
    PA_MODE_IO_ONE_DEVICE,
    PA_MODE_IO_TWO_DEVICES
} PaDeviceMode;


/**************************************************************/
/* Define structure to contain all DirectSound and Windows specific data. */
typedef struct PaHostSoundControl
{
    DSoundWrapper    pahsc_DSoundWrapper;
    MMRESULT         pahsc_TimerID;
    BOOL             pahsc_IfInsideCallback;  /* Test for reentrancy. */
    short           *pahsc_NativeBuffer;
    unsigned int     pahsc_BytesPerBuffer;    /* native buffer size in bytes */
    double           pahsc_ValidFramesWritten;
    int              pahsc_FramesPerDSBuffer;
    /* For measuring CPU utilization. */
    LARGE_INTEGER    pahsc_EntryCount;
    double           pahsc_InverseTicksPerUserBuffer;
}
PaHostSoundControl;


typedef struct PxInfo
{
   LPDIRECTSOUNDCAPTUREBUFFER	input;
   LPDIRECTSOUNDBUFFER	output;
} PxInfo;

int Px_GetNumMixers( void *pa_stream )
{
   return 1;
}

const char *Px_GetMixerName( void *pa_stream, int index )
{
   return "DirectDound";
}

PxMixer *Px_OpenMixer( void *pa_stream, int index )
{
   PxInfo                      *info;
   internalPortAudioStream     *past;
   PaHostSoundControl          *dsInfo;
   
   info = (PxInfo *)malloc(sizeof(PxInfo));   
   past = (internalPortAudioStream *) pa_stream;
   dsInfo = (PaHostSoundControl *) past->past_DeviceData;

   info->input = dsInfo->pahsc_DSoundWrapper.dsw_InputBuffer;
   info->output = dsInfo->pahsc_DSoundWrapper.dsw_OutputBuffer;

   return (PxMixer *)info;
}

/*
 Px_CloseMixer() closes a mixer opened using Px_OpenMixer and frees any
 memory associated with it. 
*/

void Px_CloseMixer(PxMixer *mixer)
{
   PxInfo *info = (PxInfo *)mixer;

   free(info);
}

/*
 Master (output) volume
*/

PxVolume Px_GetMasterVolume( PxMixer *mixer )
{
   return 0.0;
}

void Px_SetMasterVolume( PxMixer *mixer, PxVolume volume )
{
}

/*
 PCM output volume
*/
int Px_SupportsPCMOutputVolume( PxMixer* mixer ) 
{
	return 1 ;
}

PxVolume Px_GetPCMOutputVolume( PxMixer *mixer )
{
   long db;
   PxInfo *info = (PxInfo *)mixer;

   IDirectSoundBuffer8_GetVolume(info->output, &db);

   return pow(10.0, (db / 100.0 / 20.0));
}

void Px_SetPCMOutputVolume( PxMixer *mixer, PxVolume volume )
{
   PxInfo *info = (PxInfo *)mixer;

   /* convert volume (range 0.0 - 1.0) to hundredths of a dB */
   long dB = ((log(volume) / log(10.0) * 20.0)) * 100;

   IDirectSoundBuffer8_SetVolume(info->output, dB);
}

/*
 All output volumes
*/

int Px_GetNumOutputVolumes( PxMixer *mixer )
{
   return 1;
}

const char *Px_GetOutputVolumeName( PxMixer *mixer, int i )
{
   if (i == 0)
      return "PCM";
   else
      return "";
}

PxVolume Px_GetOutputVolume( PxMixer *mixer, int i )
{
   return Px_GetPCMOutputVolume(mixer);
}

void Px_SetOutputVolume( PxMixer *mixer, int i, PxVolume volume )
{
   Px_SetPCMOutputVolume(mixer, volume);
}

/*
 Input sources
*/

int Px_GetNumInputSources( PxMixer *mixer )
{
   return 1 ;
}

const char *Px_GetInputSourceName( PxMixer *mixer, int i)
{
   return "Default Input Source" ;
}

int Px_GetCurrentInputSource( PxMixer *mixer )
{
   return -1; /* none */
}

void Px_SetCurrentInputSource( PxMixer *mixer, int i )
{
}

/*
 Input volume
*/

PxVolume Px_GetInputVolume( PxMixer *mixer )
{
   return 0.0;
}

void Px_SetInputVolume( PxMixer *mixer, PxVolume volume )
{
}

/*
  Balance
*/

int Px_SupportsOutputBalance( PxMixer *mixer )
{
   return -1;
}

PxBalance Px_GetOutputBalance( PxMixer *mixer )
{
   return 0.0;
}

void Px_SetOutputBalance( PxMixer *mixer, PxBalance balance )
{
}

/*
  Playthrough
*/

int Px_SupportsPlaythrough( PxMixer *mixer )
{
   return -1;
}

PxVolume Px_GetPlaythrough( PxMixer *mixer )
{
   return 0.0;
}

void Px_SetPlaythrough( PxMixer *mixer, PxVolume volume )
{
}

