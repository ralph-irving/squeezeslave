/** @file pa_lsf_lessblocking.c
    @ingroup test_src
    @brief Read WAV file and playback on one channel if      stereo file. Implemented using the blocking API      (Pa_ReadStream(), Pa_WriteStream() ), but still      allowing you to stop the stream if using threads.
    @author Sal Aguinaga http://serpentinista.blogspot.com
    @author Phil Burk  http://www.softsynth.com
    @author Ross Bencina rossb@audiomulch.com
*/
#include
#include
#include "portaudio.h"
#include
/* #define SAMPLE_RATE  (17932) // Test failure to open with this value. */
#define SAMPLE_RATE  (44100)
#define FRAMES_PER_BUFFER (1024)
#define LATENCY_MSEC (2000) /* new */
#define NUM_SECONDS     (5)
#define NUM_CHANNELS    (2)
/* #define DITHER_FLAG     (paDitherOff)  */
#define DITHER_FLAG     (0) /**/
/* Select sample format. */
#if 1
#define PA_SAMPLE_TYPE  paFloat32
typedef float SAMPLE;
#define SAMPLE_SILENCE  (0.0f)
#define PRINTF_S_FORMAT "%.8f"
#elif 1
#define PA_SAMPLE_TYPE  paInt16
typedef short SAMPLE;
#define SAMPLE_SILENCE  (0)
#define PRINTF_S_FORMAT "%d"
#elif 0
#define PA_SAMPLE_TYPE  paInt8
typedef char SAMPLE;
#define SAMPLE_SILENCE  (0)
#define PRINTF_S_FORMAT "%d"
#else
#define PA_SAMPLE_TYPE  paUInt8
typedef unsigned char SAMPLE;
#define SAMPLE_SILENCE  (128)
#define PRINTF_S_FORMAT "%d"
#endif
/*******************************************************************/
//typedef struct  {
// float outputBuffer;
// int left_phase;
// int right_phase;
//} paTestData
/*******************************************************************/
int main(int argc, char *argv[]);
/*******************************************************************/
int main(int argc, char *argv[])
{
printf("libsnd portaudio playback\n"); fflush(stdout);
if (argc != 2) {
fprintf(stderr, "Expecting wav file as argument\n");
return 1;
}
    PaStreamParameters /*inputParameters,*/ outputParameters;
    PaStream *stream;
    PaError err;
    SAMPLE *recordedSamples;
    int i;
    int totalFrames;
    int numSamples;
    int numBytes;
    SAMPLE max, average, val;
float *buffer;
    
    // Open sound file
SF_INFO sndInfo;
SNDFILE *sndFile = sf_open(argv[1], SFM_READ, &sndInfo);
if (sndFile == NULL) {
fprintf(stderr, "Error reading source file '%s': %s\n", argv[1], sf_strerror(sndFile));
return 1;
}
    
// Check format - 16bit PCM
if (sndInfo.format != (SF_FORMAT_WAV | SF_FORMAT_PCM_16)) {
fprintf(stderr, "Input should be 16bit Wav\n");
sf_close(sndFile);
return 1;
}
// Check channels 
if (sndInfo.channels > 2) {
fprintf(stderr, "Wrong number of channels\n");
sf_close(sndFile);
return 1;
}
if (sndInfo.channels == 1) {
// Allocate memory
buffer = malloc(sndInfo.frames * sizeof(float));
//float *buffer = calloc(sndInfo.frames * sizeof(float));
if (buffer == NULL) {
fprintf(stderr, "Could not allocate memory for file\n");
sf_close(sndFile);
return 1;
}
// Load data
long numFrames = sf_readf_float(sndFile, buffer, sndInfo.frames);
// Check correct number of samples loaded
if (numFrames != sndInfo.frames) {
fprintf(stderr, "Did not read enough frames for source\n");
sf_close(sndFile);
free(buffer);
return 1;
}
// Output Info
printf(" Read %ld frames from %s,\n "
  "Channel(s):%d,\n "
  "Sample rate: %d,\n "
  "Length: %fs\n",
  numFrames, argv[1], sndInfo.channels, sndInfo.samplerate, (float)numFrames/sndInfo.samplerate);
// Setup for Output
totalFrames = (int)numFrames; /* */
numSamples = totalFrames * sndInfo.channels;
numBytes = numSamples * sizeof(SAMPLE);
recordedSamples = (SAMPLE *) malloc( numBytes );
if( recordedSamples == NULL )
{
printf("Could not allocate record array.\n");
exit(1);
}
for( i=0; i
sf_close(sndFile);
free(buffer);
} else { // 2 channels
    buffer = malloc(sndInfo.frames * 2 * sizeof(float));  // * 1 to put it on one channel.
if (buffer == NULL) {
            fprintf(stderr, "Could not allocate memory for file\n");
            sf_close(sndFile);
            return 1;
}
long numFrames = sf_readf_float(sndFile, buffer, sndInfo.frames);
if (numFrames != sndInfo.frames) {
            fprintf(stderr, "Did not read enough frames for source\n");
            sf_close(sndFile);
            free(buffer);
            return 1;
}
printf(" Read %ld frames from %s,\n "
  "Channel(s):%d,\n "
  "Sample rate: %d,\n "
  "Length: %fs\n",
  numFrames, argv[1], sndInfo.channels, sndInfo.samplerate, (float)numFrames/sndInfo.samplerate);
// Setup for Output
totalFrames = (int)numFrames; /* */
numSamples = totalFrames * sndInfo.channels;
// numSamples = totalFrames / 2; // changed it to 1 channel 
/*************/
// double duration = (double)numFrames/sndInfo.samplerate;
// double *singleChanOutput = (double *) malloc (numFrames * sizeof(doube));
// if (singleChanOutput == NULL) {
// fprintf(stderr, "Could not allocate buffer for output\n");
// }
/*************/
// numBytes = numSamples * sizeof(SAMPLE);
recordedSamples = (SAMPLE *) malloc( numSamples * sizeof(SAMPLE));
if( recordedSamples == NULL )
{
printf("Could not allocate record array.\n");
exit(1);
}
printf("numFrames = %d, numSamples =%d\n", (int)numFrames, numSamples);
for( i=0; i2; i++ ) {
recordedSamples[i*2] = buffer[i*2];
recordedSamples[i*2+1] = 0;
}
        sf_close(sndFile);
        free(buffer);
printf("totalFrames = %d\n"
  "numSamples = %d\n"
  ,totalFrames, numSamples);
}
    // totalFrames = NUM_SECONDS * SAMPLE_RATE; /* Record for a few seconds. */
//  numSamples = totalFrames * NUM_CHANNELS;
    err = Pa_Initialize();
    if( err != paNoError ) goto error;
    /* Measure maximum peak amplitude. */
    max = 0;
    average = 0;
    for( i=0; i
    {
        val = recordedSamples[i];
        if( val < 0 ) val = -val; /* ABS */
        if( val > max )
        {
            max = val;
        }
        average += val;
    }
    average = average / numSamples;
    printf("Sample max amplitude = "PRINTF_S_FORMAT"\n", max );
    printf("Sample average = "PRINTF_S_FORMAT"\n", average );
/*  Was as below. Better choose at compile time because this
    keeps generating compiler-warnings:
    if( PA_SAMPLE_TYPE == paFloat32 )
    {
        printf("sample max amplitude = %f\n", max );
        printf("sample average = %f\n", average );
    }
    else
    {
        printf("sample max amplitude = %d\n", max );
        printf("sample average = %d\n", average );
    }
*/
/*####################################################################
 * Playback recorded data
 ********************************************************************/
    outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
    outputParameters.channelCount = sndInfo.channels;
    outputParameters.sampleFormat =  PA_SAMPLE_TYPE;
    outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
outputParameters.hostApiSpecificStreamInfo = NULL;
    printf("Begin playback.\n"); fflush(stdout);
    err = Pa_OpenStream(
              &stream,
              NULL, /* no input */
              &outputParameters,
              sndInfo.samplerate,
              FRAMES_PER_BUFFER,
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              NULL, /* no callback, use blocking API */
              NULL ); /* no callback, so no callback userData */
    if( err != paNoError ) goto error;
    if( stream )
    {
        err = Pa_StartStream( stream );
        if( err != paNoError ) goto error;
        printf("Waiting for playback to finish.\n"); fflush(stdout);
for (i=0; i
err = Pa_WriteStream( stream, &recordedSamples[i*sndInfo.channels], min(FRAMES_PER_BUFFER,totalFrames-i) );
if( err != paNoError ) {
goto error;
break;
}
}
        err = Pa_CloseStream( stream );
        if( err != paNoError ) goto error;
        printf("Done.\n"); fflush(stdout);
    }
    free( recordedSamples );
    Pa_Terminate();
    return 0;
error:
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return -1;
}

