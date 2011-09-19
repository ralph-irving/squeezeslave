/*
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 * Linux OSS Implementation by douglas repetto and Phil Burk
 *
 * Copyright (c) 1999-2000 Phil Burk
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

/* Modification history:
   20020621: pa_unix_oss.c split into pa_unix.c, pa_unix.h, pa_unix_oss.c by
       Augustus Saunders. See pa_unix.c for previous history. Pa_FlushStream
       added by Augustus Saunders for Solaris compatibility.
   PLB20021018 - Fill device info table with actual sample rates instead of wished for rates.
               - Allow stream to open if sample rate within 10% of desired rate.
   20030630 - Thomas Richter - eliminated unused variable warnings.
*/

#include "pa_unix.h"

#ifdef __linux__
#include <linux/soundcard.h>
#else
#include <machine/soundcard.h> /* JH20010905 */
#endif


#ifndef AFMT_S16_NE
#define AFMT_S16_NE  Get_AFMT_S16_NE()
/*********************************************************************
 * Some versions of OSS do not define AFMT_S16_NE. So check CPU.
 * PowerPC is Big Endian. X86 is Little Endian.
 */
int Get_AFMT_S16_NE( void )
{
    long testData = 1; 
    char *ptr = (char *) &testData;
    int isLittle = ( *ptr == 1 ); /* Does address point to least significant byte? */
    return isLittle ? AFMT_S16_LE : AFMT_S16_BE;
}
#endif /* AFMT_S16_NE */


/*********************************************************************
 * Try to open the named device.
 * If it opens, try to set various rates and formats and fill in 
 * the device info structure.
 */
PaError Pa_QueryDevice( const char *deviceName, internalPortAudioDevice *pad )
{
    int result = paHostError;
    int tempDevHandle;
    int maxNumChannels;

    /* douglas:
     we have to do this querying in a slightly different order. apparently
     some sound cards will give you different info based on their settings. 
     e.g. a card might give you stereo at 22kHz but only mono at 44kHz.
     the correct order for OSS is: format, channels, sample rate
     
    */
    if ( (tempDevHandle = open(deviceName,O_WRONLY|O_NONBLOCK))  == -1 )
    {
        DBUG(("Pa_QueryDevice: could not open %s\n", deviceName ));
        return paHostError;
    }

    /*  Ask OSS what formats are supported by the hardware. */
    pad->pad_Info.nativeSampleFormats = paInt16;

    /* Negotiate for the maximum number of channels for this device. PLB20010927
     * Consider up to 16 as the upper number of channels.
     * Variable numChannels should contain the actual upper limit after the call.
     * Thanks to John Lazzaro and Heiko Purnhagen for suggestions.
     */
    maxNumChannels = 2;

    pad->pad_Info.maxOutputChannels = maxNumChannels;
    DBUG(("Pa_QueryDevice: maxNumChannels = %d\n", maxNumChannels))

    /* FIXME - for now, assume maxInputChannels = maxOutputChannels.
     *    Eventually do separate queries for O_WRONLY and O_RDONLY
    */
    pad->pad_Info.maxInputChannels = pad->pad_Info.maxOutputChannels;

    DBUG(("Pa_QueryDevice: maxInputChannels = %d\n",
          pad->pad_Info.maxInputChannels))


    /* Determine available sample rates by trying each one and seeing result.
     * OSS often supports funky rates such as 44188 instead of 44100!
     */
    pad->pad_SampleRates[0] = 44100;

    pad->pad_Info.numSampleRates = 1;
    pad->pad_Info.sampleRates = pad->pad_SampleRates; /* use pointer to embedded array */

    pad->pad_Info.name = deviceName;

    result = paNoError;

    /* We MUST close the handle here or we won't be able to reopen it later!!!  */
    close(tempDevHandle);

    return result;
}

/*******************************************************************************************/
PaError Pa_SetupDeviceFormat( int devHandle, int numChannels, int sampleRate )
{
    return paNoError;
}

PaError Pa_SetupOutputDeviceFormat( int devHandle, int numChannels, int sampleRate )
{
  return Pa_SetupDeviceFormat(devHandle, numChannels, sampleRate);
}

PaError Pa_SetupInputDeviceFormat( int devHandle, int numChannels, int sampleRate )
{
  return Pa_SetupDeviceFormat(devHandle, numChannels, sampleRate);
}


/*******************************************************************************************
** Set number of fragments and size of fragments to achieve desired latency.
*/

static int CalcHigherLogTwo( int n )
{
    int log2 = 0;
    while( (1<<log2) < n ) log2++;
    return log2;
}

void Pa_SetLatency( int devHandle, int numBuffers, int framesPerBuffer, int channelsPerFrame  )
{
    int     tmp;
    int     bufferSize, powerOfTwo;

    /* Increase size of buffers and reduce number of buffers to reduce latency inside driver. */
    while( numBuffers > 8 )
    {
        numBuffers = (numBuffers + 1) >> 1;
        framesPerBuffer = framesPerBuffer << 1;
    }

    /* calculate size of buffers in bytes */
    bufferSize = framesPerBuffer * channelsPerFrame * sizeof(short); /* FIXME - other sizes? */

    /* Calculate next largest power of two */
    powerOfTwo = CalcHigherLogTwo( bufferSize );
    DBUG(("Pa_SetLatency: numBuffers = %d, framesPerBuffer = %d, powerOfTwo = %d\n",
          numBuffers, framesPerBuffer, powerOfTwo ));

    /* Encode info into a single int */
    tmp=(numBuffers<<16) + powerOfTwo;

}

/***********************************************************************/
PaTimestamp Pa_StreamTime( PortAudioStream *stream )
{
    internalPortAudioStream *past = (internalPortAudioStream *) stream;
    PaHostSoundControl *pahsc;

    if( past == NULL ) return paBadStreamPtr;
    
    pahsc = (PaHostSoundControl *) past->past_DeviceData;

    if( pahsc->pahsc_NativeOutputBuffer )
    {
       return (pahsc->pahsc_LastStreamBytes) / (past->past_NumOutputChannels * sizeof(short));
    }
    else
    {
       return (pahsc->pahsc_LastStreamBytes) / (past->past_NumInputChannels * sizeof(short));
    }
}

void Pa_UpdateStreamTime(PaHostSoundControl *pahsc)
{

}

PaError Pa_FlushStream(int devHandle)
{
  /* AS: This doesn't do anything under OSS; it was added for Solaris.*/
  devHandle = devHandle; /* unused */
  return paNoError;
}
