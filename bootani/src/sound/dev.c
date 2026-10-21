/*************************************************************************
*                                                                        *
*   This file contains the tables and definitions for                 *
*   the synthesis specific DEVice                                     *
*      The device tables are in devtab.c                                  *
**************************************************************************
*
*   The following variables and tables must be defined:
*
*   max_tracks:     1 byte, max # of synthesis channels
*   dev_init:       function called to initialize device
*   do_watchdog     function called to  kick watchdog timer
*   dtimer_int      function called to  disable timer interrupt
*   etimer_int      function called to  enable timer interrupt
*   send_dev_function:
*       function called to send a byte of data to the synthesis
*       device (sound call callable)
*       The data is in the global, "a_value" and the address is
*       in the global, "b_value."
**************************************************************************/

/*
 *  Portable port: the DirectSound calls below go to the software voice
 *  emulation in dsound_soft.c (IDirectSoundBuffer_* are macros for dsoft_*).
 *  Kernel-only pieces (XRAM pokes, pragmas, xtl.h) were removed.
 */

#define _base_

#include <math.h>
#include "dsound_soft.h"
#include "sos.h"
#include "protos.h"
#include "bootsnd.h"

extern  struct DSPpatch const *Patches[];

#include "dsptables.h"

#define MAX_BUFFERS 16


LPDIRECTSOUND8          m_pDSound;                          // DirectSound object
LPDIRECTSOUNDBUFFER8    m_pDSBuffer[MAX_BUFFERS];           // DirectSoundBuffer

#define MIN(a,b) (((signed) a) < ((signed) b) ? (a) : (b))

/*
 *  track_status has the necessary items to restore the state of
 *  a track after a track of a higher level on the same channel
 *  ends
 */

#define MAX_TRACKS  16
#define MAX_PROCESSES       30
#define MAX_LEVELS      2

uchar       channel_level[MAX_TRACKS];/* current level for each chan*/
struct track_info track_status[(MAX_LEVELS) * MAX_TRACKS];
struct process queue_list[MAX_PROCESSES]; /* pre-allocated process packets */
extern      const struct sound    * _base_ current_call;      /* pointer to current sound call */
extern  uchar   sound_call_table;


const   unsigned short  max_processes = MAX_PROCESSES;

extern          uchar       a_value, b_value;
unsigned int    dsp_address;
unsigned int    dsp_data;   
extern  ushort      value_16_bit;
extern  uchar   current_channel;
extern  uchar       master_music_volume;    /* main attenuation for music   */
extern  uchar       master_effect_volume;   /* main attenuation for f/x */
extern  uchar       music_atten;            /* music attenuation */
extern  uchar   init_call;
extern  uchar   current_level;      /* global level of current process */
extern  struct  track_info  * _base_ ti;    /* track info pointer */
extern  uchar   pan_table[];                    /* panning table (8-bit) */
void    put_dsp(void);
const   unsigned char   max_tracks = MAX_TRACKS;

int volume_dsp(uchar, uchar, ushort, uchar);
int silence_dsp(uchar);
int note_on_dsp(void);
int slur_dsp(void);
int note_off_dsp(void);
int patch_dsp(unsigned short);
int pan_dsp(uchar , signed char, ushort);
int vp_filter(void);
static int user_var_evf_dsp(void);



sos_fn const filter_functions[] = {
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter,
    vp_filter   
};


sos_silence_fn const silence_functions[] = {
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    silence_dsp,
    
};

/**************************************************************************
*                                                                        *
*   The note_on functions turn a note on.  They assume that           *
*   the global, "a_value" has the current channel # and that          *
*   the global "value_16_bit" has the 16 bit pitch (iiiiiiii.ffffffff *
*      and "b_value" has the integer part of the pitch                    *
*************************************************************************/
sos_fn const note_on_functions[] = {
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
    note_on_dsp,
};



sos_fn const slur_functions[] = {
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
    slur_dsp,
};


/*************************************************************************
*                                                                        *
*   The note_off functions turn a note off.  They assume that         *
*   the global, "a_value" has the current channel.                    *
*************************************************************************/
sos_fn const note_off_functions[] = {
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
    note_off_dsp,
};
sos_patch_fn const patch_functions[] = {
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,
    patch_dsp,

};


sos_volume_fn const volume_functions[] = {
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,
    volume_dsp,

};
sos_pan_fn const pan_functions[] = {
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,
    pan_dsp,

};


/*
 *  The original tables had a single entry (pan_dsp, called without
 *  arguments) but are indexed by channel.  They are not used by the boot
 *  score; give every channel a well-defined entry.
 */
#define UVEF16 user_var_evf_dsp,user_var_evf_dsp,user_var_evf_dsp,user_var_evf_dsp
sos_fn const user_1_var_evf_functions[] = {
    UVEF16, UVEF16, UVEF16, UVEF16
};
sos_fn const user_2_var_evf_functions[] = {
    UVEF16, UVEF16, UVEF16, UVEF16
};

static int user_var_evf_dsp(void)
{
    return pan_dsp(current_channel, ti->pan, ti->patch);
}

//
// write data from global var dsp_data to x memory space "dsp_address"
//

void put_dsp(void)
{

//  LPVOID pvXramBuffer;
//  pvXramBuffer = (LPVOID) (dsp_address);

//    memcpy(pvXramBuffer,pvData,dwDataSize);

//  *(PDWORD) (GPXMEM + dsp_address) = dsp_data;
    
}


/*
 *  WriteDSPDatablock / ReadDSPDatablock / ReadDSPProgblock poked the MCPX
 *  GP DSP memory directly (GPXMEM/GPPMEM) and were unused; removed.
 */

// return dsp data
int get_dsp(unsigned int addr)
{
    (void)addr;
//  return( (*PDWORD) (GPXMEM + dsp_address) );
    return(1);
}

int silence_dsp(unsigned char chan)
{
    (void)chan;
    return(1);
}   


int note_on_dsp(void)
{
    LONG    dwFreq;
    const struct DSPpatch *addr;

#if DBG

//  dwFreq = pitch_table_dsp[value_16_bit>>8];
//  swprintf( StringBuffer, L"Current Pitch: 0x%x", dwFreq);

#endif

    dwFreq = (LONG) ( ((value_16_bit >> 8) - 60) * (4096/12) );
    dwFreq += (LONG) ((((DWORD) value_16_bit & 0xff) * 341) / 255);
//  IDirectSoundBuffer_StopEx(m_pDSBuffer[current_channel], 0, DSBSTOPEX_ENVELOPE);
    IDirectSoundBuffer_SetPitch(m_pDSBuffer[current_channel], dwFreq);

    addr = Patches[ti->patch];
    if (addr->LoopEnable) 
        IDirectSoundBuffer_Play(m_pDSBuffer[current_channel], 0,0,DSBPLAY_LOOPING);
    else
        IDirectSoundBuffer_Play(m_pDSBuffer[current_channel], 0,0,0);

    return(1);

}
int slur_dsp(void)
{
    LONG    dwFreq;

    dwFreq = (LONG) ( ((value_16_bit >> 8) - 60) * (4096/12) );
    dwFreq += (LONG) ((((DWORD) value_16_bit & 0xff) * 341) / 255);

#if DBG
//  swprintf( StringBuffer, L"Current Pitch Slur: 0x%x", dwFreq);
#endif

    IDirectSoundBuffer_SetPitch(m_pDSBuffer[current_channel], dwFreq);

    return(1);
}

int note_off_dsp(void)
{
    IDirectSoundBuffer_StopEx(m_pDSBuffer[current_channel], 0, DSBSTOPEX_ENVELOPE);
    return(1);

}

int vp_filter(void)
{
    DSFILTERDESC    fdesc;


    fdesc.dwMode = DSFILTER_MODE_DLS2;
    fdesc.dwQCoefficient = 0;
    fdesc.adwCoefficients[0] = ti->filtercutoff + 32768;
    fdesc.adwCoefficients[1] = ti->filterres;
    fdesc.adwCoefficients[2] = ti->filtercutoff + 32768;
    fdesc.adwCoefficients[3] = ti->filterres;
    IDirectSoundBuffer_SetFilter(m_pDSBuffer[current_channel], &fdesc);
    return(1);
}



int patch_dsp(unsigned short pat)
{
    const struct DSPpatch *addr;

    addr = Patches[pat];

    IDirectSoundBuffer_SetEG(m_pDSBuffer[current_channel], addr->lpAmpEnvelope);
    IDirectSoundBuffer_SetEG(m_pDSBuffer[current_channel], addr->lpMultiEnvelope);
    IDirectSoundBuffer_SetBufferData(m_pDSBuffer[current_channel], addr->Start, addr->Length );
    IDirectSoundBuffer_SetLoopRegion( m_pDSBuffer[current_channel],0, addr->Length );
    IDirectSoundBuffer_SetCurrentPosition(m_pDSBuffer[current_channel], 0 );
    
    return(1);
}



int pan_dsp(uchar chan, signed char pan, ushort patch)
{
    uchar   mod;
    unsigned int    vol_mul;
    unsigned int    tmp;
    signed char     tmp_pan;

    vol_mul = 0x7fff;
    if (current_level >= 1) {           
/*      mod = master_effect_volume;*/
//      vol_mul = volume_table_dsp[master_music_volume];
    }
    else {
/*      mod = 0;*/
//      tmp = volume_table_dsp[music_atten];
//      vol_mul = volume_table_dsp[master_music_volume];

//      vol_mul = (long)((long)tmp * (long)vol_mul) >>15;
    
    }

    mod = 0;
    (void)chan; (void)pan; (void)patch; (void)mod;

    tmp_pan = ti->pan >> 3;

    tmp_pan += 16;                      /* set range 0 - 31 */
    tmp = 1;    
//  tmp = volume_table_dsp[MIN(127,ti->volume + mod)];
    tmp = (long)((long)tmp * (long)vol_mul) >>15;

/*  tmp >>= 7;*/
    dsp_data = (long) ((long)tmp * (long)pan_table[(int)tmp_pan]) >> 7;
/*  dsp_data = (char)tmp * pan_table[(int)tmp_pan];*/
//  a_value = VOLUME_L_ADDRESS(chan);
    put_dsp();

    dsp_data = (long) ((long)tmp * (long)pan_table[31 - (int)tmp_pan]) >> 7;
/*  dsp_data = (char)tmp * pan_table[31 - (int)tmp_pan];*/
//  a_value = VOLUME_R_ADDRESS(chan);
    put_dsp();

    return(1);

}


// initialize stuff for the dsp.
// write sine wave into high x memory.
// also create 16 dsound buffers that we'll use
// for our sounds.


/* the MS CRT LCG, renamed so it does not replace the host's libc rand() */
static int32_t holdrand = 1L;

static void sos_srand(unsigned int seed)
{
    holdrand = (int32_t)seed;
}

static int sos_rand(void)
{
    holdrand = (int32_t)((uint32_t)holdrand * 214013u + 2531011u);
    return((holdrand >> 16) & 0x7fff);
}

int dev_init(void)
{
    
    DSBUFFERDESC dsbdesc;
    WAVEFORMATEX wfFirst;
    DWORD       dwMixBinMask = DSMIXBIN_FRONT_LEFT | DSMIXBIN_FRONT_RIGHT | DSMIXBIN_FXSEND_0;
    LONG        lVolumes[3];
    int j;
    double  dtmp;
    double  FMc = 4.0;
    double  FMm = 2.0;


    int i;

    sos_srand(1003);
    for (i = 0; i < 8192; i++) {
        Noise8192[i] = (unsigned short) sos_rand();
    }

    /* (short) first: converting a negative double straight to an unsigned
       type is undefined behaviour in portable C */
    for (i = 0; i < 128; i++) {
        Sin128[i] = (unsigned short)(short)(32767*sin(2.0*3.14159*(double)i/128.0));
    }
    j = 0;
    for (i = 0; i < 32768; i++) {
        if (i < 16384)
            j++;
        else
            j--;
        dtmp = (double)j/16384.0  * sin(FMm * 2.0*3.14159*(double)i/128.0);
        FM32768[i] = (unsigned short)(short)(32767*sin(dtmp + FMc * 2.0*3.14159*(double)i/128.0));
    }

    for (i = 0; i < 128; i++) {         // create sawtooth wave
        Saw128[i] = (unsigned short)(short) (65536 * ((float)(i-64) /128.0));
    }

    for (i = 0; i < 0x5540; i++) {      // size of glock sound..make 16-bit
        ThunEl16[i] = (unsigned short) ((ThunEl16Data[i]) << 8);   // signed 8-bit data
    }
    /* the original read ThunEl16[0x5540] (one past the end, i.e. the first,
       still zero, word of ReverseThunEl16) for i == 0 */
    for (i = 0,j=0x5540; i < 0x5540; i++,j--) {     // size of glock sound..make 16-bit
        ReverseThunEl16[i] = (j < 0x5540) ? ThunEl16[j] : 0;
    }

    

    for (i = 0; i < 3768; i++) {        // size of glock sound..make 16-bit
        Glock[i] = (unsigned short) ((GlockData[i]^0x80) << 8);   // unsigned 8-bit data
    }
        
    for (i = 0; i < 6719; i++) {        // size of glock sound..make 16-bit
        Bubble[i] = (unsigned short) ((BubbleData[i]^0x80) << 8); // unsigned 8-bit data
    }


    if( FAILED( DirectSoundCreate( NULL, &m_pDSound, NULL ) ) )
        return (0);


    ZeroMemory( &dsbdesc, sizeof( DSBUFFERDESC ) );
    dsbdesc.dwSize = sizeof( DSBUFFERDESC );

    wfFirst.wFormatTag = WAVE_FORMAT_PCM;
    wfFirst.nChannels = 1;
    wfFirst.nSamplesPerSec = 48000;
    wfFirst.wBitsPerSample = 16;
    wfFirst.nBlockAlign = wfFirst.nChannels * wfFirst.wBitsPerSample/8;
    wfFirst.nAvgBytesPerSec = wfFirst.nSamplesPerSec * wfFirst.nBlockAlign;

    dsbdesc.dwFlags = 0;
    dsbdesc.dwBufferBytes = 0;
    dsbdesc.lpwfxFormat = &wfFirst;
    dsbdesc.dwMixBinMask = dwMixBinMask;


    for (i = 0; i < MAX_BUFFERS; i++) {


        if (i%2) {
        lVolumes[0] = -600;
        lVolumes[1] = 0;
        lVolumes[2] = -2800;
        dwMixBinMask = DSMIXBIN_FRONT_LEFT | DSMIXBIN_FRONT_RIGHT;  
            dsbdesc.dwMixBinMask = dwMixBinMask;        
        }
        else {
        lVolumes[0] = 0;
        lVolumes[1] = -600;
        lVolumes[2] = -2800;
        dwMixBinMask = DSMIXBIN_FRONT_LEFT | DSMIXBIN_FRONT_RIGHT;  
            dsbdesc.dwMixBinMask = dwMixBinMask;    
        }
        if ((i == 3) || (i == 5)) {
        lVolumes[0] = 0;
        lVolumes[1] = -100;
        lVolumes[2] = 00;
        dwMixBinMask = DSMIXBIN_FRONT_LEFT | DSMIXBIN_FRONT_RIGHT | DSMIXBIN_FXSEND_0;
            dsbdesc.dwMixBinMask = dwMixBinMask;
        }

            if( FAILED( DirectSoundCreateBuffer( &dsbdesc, &m_pDSBuffer[i]) ) )
                return 0;

            IDirectSoundBuffer_SetMixBinVolumes(m_pDSBuffer[i], dwMixBinMask, lVolumes);
    }

//  ReadDSPDatablock(0xa00*4, databack, sizeof(databack) );

     put_fifo(0x1);
     return(1);
}
//
// free dsound buffers and dsound object we created
//
int dev_cleanup(void)
{
    int i;

    for (i = 0; i < MAX_BUFFERS; i++) {
        if (m_pDSBuffer[i])
            IDirectSoundBuffer_Release(m_pDSBuffer[i]);
        m_pDSBuffer[i] = NULL;
    }
    if (m_pDSound)
        IDirectSound_Release(m_pDSound);
    m_pDSound = NULL;
    return(1);
}


void do_watchdog(void)
{
}
int dtimer_int(void)
{
    return(0);
}
int etimer_int(void)
{
    return(0);
}
int send_dev_function(void)
{
    return(0);
}
/*************************************************************************
*                                                                        *
*   Adjust the volume of that patch, "patch_addr" on channel,     *
*   "chan" by the amount volume + whatever the global volume is   *
*   for "level"                           *
*                                                                        *
*************************************************************************/

int volume_dsp(
uchar op_level, 
uchar sound_level,
ushort patch_addr,
uchar   chan)
{
    (void)sound_level; (void)patch_addr; (void)chan;

    IDirectSoundBuffer_SetVolume(m_pDSBuffer[current_channel], (-1*op_level*30) + 200);
    return(1);
}

void call_user_function(void)
{

}


int user_silence_function(void)
{
#if DBG
//  swprintf( StringBuffer, L"Current Sound: %S", "SilenceFunction");
#endif
    return(0);
}

/* pan_dsp indexes this with 0..31; the original only had 8 entries (the
   out-of-range reads were harmless because put_dsp() is a no-op). */
uchar pan_table[32] = {
    1,2,3,4,5,6,7,8
};


