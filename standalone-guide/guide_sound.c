#include <math.h>
/* UI sounds: the engine's own XuiSound elements, played through winmm.
 *
 * How the console's sounds fire in this engine (xlive 1.2.241.0, verified by disassembly):
 *   - Scenes and skins author XuiSoundXAudio elements whose File property is keyed in timelines
 *     (skin visuals: Focus/Press frames -> btn_Focus.xma/btn_Select.xma/btn_Back.xma...; dashmain:
 *     1To2.. -> dash_BladeSwitch_N.xma, 1Open.. -> dash_2ndLevelOpen.xma; hudbkgnd: ClosedToHalf ->
 *     HUD_open.xma, HalfToClosed -> HUD_close.xma; hud scr_Notification TransTo -> NotifyPopup.xma).
 *   - The File setter (0x7e36e3) plays when the file changes to a non-empty name: XuiSound arms a
 *     0 ms timer (0x7d1946), and the timer (0x7d1c89) sends XM_SOUND_PLAY 0x7f4 to the element.
 *     0x7f5 = stop, 0x7f6 = "finished?" poll (data |= 1 when done; used for Loop).
 *   - The XuiSoundXAudio class (0x7e3ea1) plays through an IDirectSound given to 0x7e40f0; the host
 *     registers it without one, and the console's files are XMA1, which no PC API decodes.
 * So the host answers 0x7f4/0x7f5/0x7f6 here, in the global message filter, with the same files
 * converted offline to PCM (build.py: assets/sounds/<name>.wav from the .xzp .xma entries).
 * Volume is the element's Volume in dB (default -12), as the console's XuiSoundXAudio uses it:
 * amplitude 10^(Volume/20), Volume clamped to -96..+6 (xam.xex 0x8192d200).
 *
 * Output: waveOut 48 kHz stereo 16-bit with a small software mixer on its own thread. Without an
 * audio device the mixer still runs on a timer (sounds start/finish, nothing is heard).
 * Probe aid: XUI_SOUND_DUMP=<file> also writes the mixed output as raw s16le 48 kHz stereo.
 */
#define SOUND_RATE 48000
#define SOUND_BLOCK 960           /* 20 ms */
#define SOUND_BUFFERS 4
#define SOUND_VOICES 16
#define SOUND_CLIPS 32
typedef struct {WCHAR name[64];short *pcm;DWORD frames;int channels;BOOL missing;} SoundClip;
typedef struct {DWORD element;SoundClip *clip;DWORD pos;int gain;BOOL loop,active;} SoundVoice;  /* gain: 1/4096 */
static SoundClip sound_clips[SOUND_CLIPS];
static SoundVoice sound_voices[SOUND_VOICES];
static CRITICAL_SECTION sound_lock;
static HWAVEOUT sound_out;
static HANDLE sound_event,sound_thread,sound_dump=INVALID_HANDLE_VALUE;
static BOOL sound_ready,sound_device;
static volatile LONG sound_quit;   /* sound_shutdown: the mixer thread leaves its loop */
static volatile LONG sound_mixed;   /* blocks mixed (diagnostics) */
static WAVEHDR sound_hdr[SOUND_BUFFERS];
static short sound_buf[SOUND_BUFFERS][SOUND_BLOCK*2];

static void sound_mix(short *out) {
    static int acc[SOUND_BLOCK*2];
    memset(acc,0,sizeof acc);
    EnterCriticalSection(&sound_lock);
    for(int v=0;v<SOUND_VOICES;v++) {
        SoundVoice *s=&sound_voices[v];
        if(!s->active) continue;
        const short *p=s->clip->pcm;int ch=s->clip->channels;
        for(int i=0;i<SOUND_BLOCK;i++) {
            if(s->pos>=s->clip->frames) {if(s->loop && s->clip->frames) s->pos=0;else {s->active=FALSE;break;}}
            int l=p[s->pos*ch],r=p[s->pos*ch+ch-1];s->pos++;
            acc[i*2]+=l*s->gain>>12;acc[i*2+1]+=r*s->gain>>12;
        }
    }
    LeaveCriticalSection(&sound_lock);
    for(int i=0;i<SOUND_BLOCK*2;i++) out[i]=(short)(acc[i]>32767?32767:acc[i]<-32768?-32768:acc[i]);
    if(sound_dump!=INVALID_HANDLE_VALUE) {DWORD w;WriteFile(sound_dump,out,SOUND_BLOCK*4,&w,0);}
    InterlockedIncrement(&sound_mixed);
}
static DWORD WINAPI sound_main(void *unused) {
    if(sound_device) {
        while(!sound_quit) {
            WaitForSingleObject(sound_event,100);
            if(sound_quit) break;
            for(int b=0;b<SOUND_BUFFERS;b++) if(sound_hdr[b].dwFlags&WHDR_DONE) {
                sound_mix(sound_buf[b]);sound_hdr[b].dwFlags&=~WHDR_DONE;
                if(waveOutWrite(sound_out,&sound_hdr[b],sizeof(WAVEHDR))!=MMSYSERR_NOERROR) sound_hdr[b].dwFlags|=WHDR_DONE;
            }
        }
        return 0;
    }
    /* No device: keep time so voices start and finish as they would. */
    DWORD start=GetTickCount();ULONGLONG done=0;
    while(!sound_quit) {
        Sleep(10);
        ULONGLONG due=(ULONGLONG)(GetTickCount()-start)*SOUND_RATE/1000/SOUND_BLOCK;
        while(done<due) {short block[SOUND_BLOCK*2];sound_mix(block);done++;}
    }
    return 0;
}
static void sound_init(void) {
    if(sound_ready || sound_quit) return;
    InitializeCriticalSection(&sound_lock);
    char dump[MAX_PATH];
    if(GetEnvironmentVariableA("XUI_SOUND_DUMP",dump,sizeof dump)) sound_dump=CreateFileA(dump,GENERIC_WRITE,FILE_SHARE_READ,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    WAVEFORMATEX f={WAVE_FORMAT_PCM,2,SOUND_RATE,SOUND_RATE*4,4,16,0};
    sound_event=CreateEventW(0,FALSE,FALSE,0);
    MMRESULT r=waveOutGetNumDevs()?waveOutOpen(&sound_out,WAVE_MAPPER,&f,(DWORD_PTR)sound_event,0,CALLBACK_EVENT):MMSYSERR_NODRIVER;
    sound_device=r==MMSYSERR_NOERROR;
    log_value(sound_device?"Sound output (waveOut 48 kHz stereo)":"Sound output: no audio device (sounds are timed, not heard)",r);
    if(sound_device) for(int b=0;b<SOUND_BUFFERS;b++) {
        sound_hdr[b].lpData=(char *)sound_buf[b];sound_hdr[b].dwBufferLength=sizeof sound_buf[b];
        if(waveOutPrepareHeader(sound_out,&sound_hdr[b],sizeof(WAVEHDR))==MMSYSERR_NOERROR) sound_hdr[b].dwFlags|=WHDR_DONE;
    }
    sound_ready=TRUE;
    sound_thread=CreateThread(0,0,sound_main,0,0,0);
    if(sound_thread) SetThreadPriority(sound_thread,THREAD_PRIORITY_ABOVE_NORMAL);
    if(sound_device) SetEvent(sound_event);
}

/* Program exit (ui.c, before the engine is released): stop the mixer thread, then the device, so no
   thread mixes or writes buffers while the process tears down. */
static void sound_shutdown(void) {
    if(!sound_ready) return;
    InterlockedExchange(&sound_quit,1);
    if(sound_event) SetEvent(sound_event);
    if(sound_thread) {
        if(WaitForSingleObject(sound_thread,2000)!=WAIT_OBJECT_0) log_text("Sound thread did not stop");
        CloseHandle(sound_thread);sound_thread=0;
    }
    if(sound_device) {
        waveOutReset(sound_out);
        for(int b=0;b<SOUND_BUFFERS;b++) waveOutUnprepareHeader(sound_out,&sound_hdr[b],sizeof(WAVEHDR));
        waveOutClose(sound_out);sound_device=FALSE;
    }
    if(sound_dump!=INVALID_HANDLE_VALUE) {CloseHandle(sound_dump);sound_dump=INVALID_HANDLE_VALUE;}
    sound_ready=FALSE;
    log_text("Sound output closed");
}

/* assets\sounds\<name>.wav: PCM 16-bit, 48 kHz, mono or stereo (build.py writes them). */
static SoundClip *sound_clip(const WCHAR *file) {
    WCHAR name[64];
    const WCHAR *base=file;
    for(const WCHAR *p=file;*p;p++) if(*p==L'#' || *p==L'/' || *p==L'\\') base=p+1;
    lstrcpynW(name,base,48);
    WCHAR *dot=wcsrchr(name,L'.');if(dot) *dot=0;
    SoundClip *free_slot=0;
    for(int i=0;i<SOUND_CLIPS;i++) {
        if(!sound_clips[i].name[0]) {if(!free_slot) free_slot=&sound_clips[i];continue;}
        if(!lstrcmpiW(sound_clips[i].name,name)) return sound_clips[i].missing?0:&sound_clips[i];
    }
    if(!free_slot) return 0;
    SoundClip *c=free_slot;lstrcpyW(c->name,name);c->missing=TRUE;
    WCHAR path[MAX_PATH];DWORD n=GetModuleFileNameW(0,path,MAX_PATH);
    WCHAR *slash=n?wcsrchr(path,L'\\'):0;
    if(!slash || (slash-path)+lstrlenW(name)+24>=MAX_PATH) return 0;
    wsprintfW(slash+1,L"assets\\sounds\\%s.wav",name);
    HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);
    DWORD size=h==INVALID_HANDLE_VALUE?0:GetFileSize(h,0),got=0;
    BYTE *data=size>44 && size<16*1024*1024?HeapAlloc(GetProcessHeap(),0,size):0;
    if(data && (!ReadFile(h,data,size,&got,0) || got!=size)) {HeapFree(GetProcessHeap(),0,data);data=0;}
    if(h!=INVALID_HANDLE_VALUE) CloseHandle(h);
    if(!data) {log_text("Sound file missing");log_wide(path);return 0;}
    int channels=0,bits=0;DWORD rate=0;
    if(!memcmp(data,"RIFF",4) && !memcmp(data+8,"WAVE",4)) for(DWORD at=12;at+8<=size;) {
        DWORD len=*(DWORD *)(data+at+4);
        if(len>size-at-8) break;
        if(!memcmp(data+at,"fmt ",4) && len>=16) {
            if(*(WORD *)(data+at+8)==WAVE_FORMAT_PCM) {channels=*(WORD *)(data+at+10);rate=*(DWORD *)(data+at+12);bits=*(WORD *)(data+at+22);}
        } else if(!memcmp(data+at,"data",4) && channels>=1 && channels<=2 && bits==16 && rate==SOUND_RATE) {
            c->pcm=(short *)(data+at+8);c->channels=channels;c->frames=len/(2*channels);c->missing=FALSE;
            return c;
        }
        at+=8+len+(len&1);
    }
    log_text("Sound file is not 16-bit 48 kHz PCM");log_wide(path);
    HeapFree(GetProcessHeap(),0,data);
    return 0;
}

static SoundVoice *sound_voice(DWORD element) {
    for(int v=0;v<SOUND_VOICES;v++) if(sound_voices[v].element==element) return &sound_voices[v];
    return 0;
}
static void sound_stop(DWORD element) {
    if(!sound_ready) return;
    EnterCriticalSection(&sound_lock);
    SoundVoice *s=sound_voice(element);if(s) s->active=FALSE;
    LeaveCriticalSection(&sound_lock);
}
static void sound_play(DWORD element) {
    XuiValue v;
    if(FAILED(xui_get(element,L"File",&v))) return;
    WCHAR file[MAX_PATH]=L"";
    if(v.type==5 && v.text) lstrcpynW(file,v.text,MAX_PATH);
    xui_value_free(&v);
    if(!file[0]) return;
    const WCHAR *id=blades_id(element);
    char line[1100];
    /* xlive's own (Games for Windows) resources, e.g. the hidden standalone background's
       panres xam#HUD_open.wav: not console sounds, never played. */
    WCHAR *uri=XUI_FN(0x7d1b69,WCHAR *,DWORD,const WCHAR *)(element,file);
    BOOL gfwl=uri && !wcsncmp(uri,L"panres://",9);
    if(uri) XUI_FN(0x7d17e8,void,void *)(uri);
    if(gfwl) return;
    /* Notification toast (hud scr_Notification's PopUpSound): Personal Settings > Notifications > Play Sound. */
    if(id && !lstrcmpW(id,L"PopUpSound")) {
        if(!setting_get_int(L"NotificationSound",1)) {wsprintfA(line,"Sound muted by NotificationSound: %ls",file);log_text(line);return;}
        /* PopUpSound is the one sound with a static File: when the host rebuilds visuals (graphics
           device reset) the popup's new visual would chime again. Once per popup (guide_toast's serial),
           however long the popup has been up. */
        static int played=-1;
        DWORD visual=0,control=0;
        XUI_FN(0x7d32af,HRESULT,DWORD,DWORD *)(element,&visual);
        if(visual) XUI_FN(0x7d32af,HRESULT,DWORD,DWORD *)(visual,&control);
        if(control && control==live_toast.control) {
            if(played==live_toast.serial) {log_text("Sound repeat skipped (popup visual rebuilt)");return;}
            played=live_toast.serial;
        }
    }
    float volume=-12;
    if(SUCCEEDED(xui_get(element,L"Volume",&v))) {if(v.type==4) volume=v.number;xui_value_free(&v);}
    BOOL loop=FALSE;
    if(SUCCEEDED(xui_get(element,L"Loop",&v))) {if(v.type==1) loop=v.integer!=0;xui_value_free(&v);}
    /* The console's XuiSoundXAudio (xam.xex 0x8192d200) accepts -96..+6 and sets the XAudio voice
       volume to 10^(Volume*0.05): Volume is plain dB. */
    if(volume<-96) volume=-96;
    if(volume>6) volume=6;
    int mb=(int)(volume*100.0f+(volume<0?-0.5f:0.5f));
    if(XUI_GLOBAL(0x8f1228,DWORD)) mb=-10000;   /* the engine's global sound mute */
    double gain=pow(10.0,mb/2000.0);
    /* Log what fired it: the control whose visual owns the sound, or the scene. */
    const WCHAR *owner=L"?";
    DWORD up=element;
    for(int i=0;i<4;i++) {
        DWORD parent=0;
        if(FAILED(XUI_FN(0x7d32af,HRESULT,DWORD,DWORD *)(up,&parent)) || !parent) break;
        up=parent;const WCHAR *n=blades_id(up);
        if(n && n[0]) {owner=n;break;}
    }
    wsprintfA(line,"Sound play %ls (%ls in %ls) volume %d.%02d dB%s",file,id?id:L"?",owner,mb/100,(mb<0?-mb:mb)%100,loop?" loop":"");
    log_text(line);
    sound_init();
    SoundClip *c=sound_clip(file);
    if(!c) return;
    EnterCriticalSection(&sound_lock);
    SoundVoice *s=sound_voice(element);
    for(int i=0;!s && i<SOUND_VOICES;i++) if(!sound_voices[i].active) s=&sound_voices[i];
    if(!s) {s=&sound_voices[0];for(int i=1;i<SOUND_VOICES;i++) if(sound_voices[i].pos>s->pos) s=&sound_voices[i];}
    s->element=element;s->clip=c;s->pos=0;s->gain=(int)(gain*4096+0.5);s->loop=loop;s->active=TRUE;
    LeaveCriticalSection(&sound_lock);
}
/* Called first from ui_message_filter. The first (most derived) class level gets the message;
   marking it handled stops the engine's own XuiSoundXAudio proc, which has no device. */
static BOOL sound_filter(DWORD element,DWORD *message) {
    DWORD id=message[1];
    if(id<0x7f4 || id>0x7f6) return FALSE;
    if(!xui_is(element,L"XuiSoundXAudio")) return FALSE;
    if(id==0x7f4) {
        /* As 0x7e3f26: elements flagged 0x2000 do not play. */
        if(!XUI_FN(0x7d395c,BOOL,DWORD)(element)) sound_play(element);
    } else if(id==0x7f5) sound_stop(element);
    else if(message[4] && message[3]>=4) {
        BOOL playing=FALSE;
        if(sound_ready) {EnterCriticalSection(&sound_lock);SoundVoice *s=sound_voice(element);playing=s && s->active;LeaveCriticalSection(&sound_lock);}
        if(!playing) *(DWORD *)message[4]|=1;
    }
    message[2]=1;
    return TRUE;
}
/* The engine's timer service (XuiTimersRun 0x7e35eb, list 0x8f2bfc) is not run by this host, and
   XuiSound plays from a timer (id 0xaabbccdd, XM_TIMER 0x27). Run just those timers, as
   XuiTimersRun does, before the timelines each frame. Entry: {obj,id,ms,last,flags(1=killed),next}. */
static void sound_timers(void) {
    CRITICAL_SECTION *cs=&XUI_GLOBAL(0x8f2be4,CRITICAL_SECTION);
    DWORD sound_class=XUI_FN(0x7dec34,DWORD,const WCHAR *)(L"XuiSound");
    if(!sound_class) return;
    EnterCriticalSection(cs);
    for(DWORD *t=XUI_GLOBAL(0x8f2bfc,DWORD *),*next;t;t=next) {
        next=(DWORD *)t[5];
        if(t[4]&1 || t[1]!=0xaabbccdd) continue;
        DWORD now=GetTickCount();
        if(now-t[3]<t[2]) continue;
        if(!XUI_FN(0x7d876c,DWORD,DWORD,DWORD)(t[0],sound_class)) continue;
        t[3]=now;
        DWORD timer[1]={t[1]},m[5]={20,0x27,0,sizeof timer,(DWORD)timer};
        XUI_FN(0x7dbd06,HRESULT,DWORD,void *)(t[0],m);
    }
    LeaveCriticalSection(cs);
}
