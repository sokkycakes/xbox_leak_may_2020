/* Guide module: music. The Guide's mini media player (hud_hud.xzp#MiniMediaPlayer.xur in
   scnMusic) and the music picker (minimediaplayer_media.xzp: 579 sources -> 570 library ->
   572 albums/artists/playlists/genres -> 576 songs -> BeginPlayback).
   Playback is local: winmm MCI type mpegvideo (DirectShow; .mp3/.wma/.wav on stock Windows,
   other formats when a DirectShow filter exists; undecodable files are skipped).
   Library: the Windows Music folder (CSIDL_MYMUSIC) and a "music" folder beside the exe,
   scanned once in a worker thread, sorted by path. Tags come from the shell property store,
   with an ID3v2 reader as fallback, and the console's "Unknown ..." strings for empty ones.
   Saved Playlists are the .m3u/.m3u8 files found in those folders.
   Strings are the console's (minimediaplayer Strings.xus, hud Strings.xus #28). */
#include <mmsystem.h>
#include <shlobj.h>
#include <propsys.h>
#define MUSIC_MAX 4096
#define MUSIC_QUEUE_LIMIT 100     /* "Song Limit": only the first 100 songs are played */
#define MUSIC_LISTS 256

typedef struct {WCHAR *path,*title,*artist,*album,*genre;int track;} MusicSong;
static MusicSong music_songs[MUSIC_MAX];
static int music_song_count;
typedef struct {WCHAR *name;int first,count;} MusicPlaylist;
static MusicPlaylist music_playlists[MUSIC_LISTS];
static int music_playlist_count,music_playlist_items[MUSIC_MAX],music_playlist_item_count;
static WCHAR *music_scan_paths[MUSIC_MAX],*music_scan_lists[MUSIC_LISTS];
static int music_scan_count,music_scan_list_count;
static volatile LONG music_scan_state;   /* 0 not started, 1 scanning, 2 done */

/* playback queue (song indices), play order (positions into the queue), current position */
static int music_queue[MUSIC_MAX],music_queue_count,music_order[MUSIC_MAX],music_pos=-1;
static BOOL music_open_ok,music_paused,music_shuffle;
static int music_volume=100;      /* slider units 0..music_volume_max; MCI = 0..1000 */
static int music_volume_max=100;
static DWORD music_last_poll,music_ui_scene;
static BOOL music_volume_mode,music_setting_slider,music_loaded_settings,music_guide_was_open;

/* picker state */
enum {MP_NONE,MP_SOURCES,MP_LIBRARY,MP_CONTAINERS,MP_SONGS,MP_BEGIN};
enum {MC_ALBUMS,MC_ARTISTS,MC_PLAYLISTS,MC_GENRES};
static int music_cat[6]={-2,-2,-2,-2,-2,-2};
static int music_cont_kind;                       /* kind shown by 572 */
static WCHAR *music_cont_names[MUSIC_MAX];        /* 572 rows */
static int music_cont_count;
static int music_list[MUSIC_MAX],music_list_count;/* 576 rows (song indices) */
static WCHAR music_list_header[256];
static int music_request_ids[MUSIC_MAX],music_request_count,music_request_start;
static int music_begin_wait=-1;                   /* frames left on BeginPlayback, -1 = none */
static BOOL music_begin_pending,music_y_pending,music_pages_pending;
static int music_press_kind,music_press_sel;
static WCHAR music_count_text[64];

static MCIERROR music_mci(const WCHAR *cmd,WCHAR *out,UINT cch) {
    WCHAR scratch[64];MCIERROR e=mciSendStringW(cmd,out?out:scratch,out?cch:64,NULL);
    if(e) {char line[600];wsprintfA(line,"MCI %lu: %ls",e,cmd);log_text(line);}
    return e;
}
static WCHAR *music_strdup(const WCHAR *s,int n) {
    if(n<0) n=lstrlenW(s);
    WCHAR *d=HeapAlloc(GetProcessHeap(),0,(n+1)*sizeof(WCHAR));
    if(d) {memcpy(d,s,n*sizeof(WCHAR));d[n]=0;}
    return d;
}

/* ---------- library scan (worker thread; touches no XUI and writes no log) ---------- */
static BOOL music_ext(const WCHAR *name,BOOL playlist) {
    const WCHAR *dot=wcsrchr(name,L'.');if(!dot) return FALSE;
    static const WCHAR *audio[]={L".mp3",L".wma",L".wav",L".m4a",L".aac",L".flac"};
    if(playlist) return !lstrcmpiW(dot,L".m3u") || !lstrcmpiW(dot,L".m3u8");
    for(int i=0;i<6;i++) if(!lstrcmpiW(dot,audio[i])) return TRUE;
    return FALSE;
}
static void music_scan_dir(const WCHAR *dir,int depth) {
    if(depth>8 || music_scan_count>=MUSIC_MAX) return;
    WCHAR pattern[MAX_PATH];if(lstrlenW(dir)+3>=MAX_PATH) return;
    wsprintfW(pattern,L"%s\\*",dir);
    WIN32_FIND_DATAW fd;HANDLE h=FindFirstFileW(pattern,&fd);if(h==INVALID_HANDLE_VALUE) return;
    do {
        if(fd.cFileName[0]==L'.') continue;
        WCHAR path[MAX_PATH];if(lstrlenW(dir)+lstrlenW(fd.cFileName)+2>=MAX_PATH) continue;
        wsprintfW(path,L"%s\\%s",dir,fd.cFileName);
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
            if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) music_scan_dir(path,depth+1);
        } else if(music_ext(fd.cFileName,FALSE) && music_scan_count<MUSIC_MAX) {
            WCHAR *p=music_strdup(path,-1);if(p) music_scan_paths[music_scan_count++]=p;
        } else if(music_ext(fd.cFileName,TRUE) && music_scan_list_count<MUSIC_LISTS) {
            WCHAR *p=music_strdup(path,-1);if(p) music_scan_lists[music_scan_list_count++]=p;
        }
    } while(FindNextFileW(h,&fd) && music_scan_count<MUSIC_MAX);
    FindClose(h);
}
/* Shell sort (no CRT qsort in this host). */
static void music_sort_ptr(WCHAR **a,int n) {
    for(int gap=n/2;gap>0;gap/=2) for(int i=gap;i<n;i++) {
        WCHAR *t=a[i];int j=i;
        for(;j>=gap && lstrcmpiW(a[j-gap],t)>0;j-=gap) a[j]=a[j-gap];
        a[j]=t;
    }
}
static void music_sort_int(int *a,int n,int (*cmp)(int,int)) {
    for(int gap=n/2;gap>0;gap/=2) for(int i=gap;i<n;i++) {
        int t=a[i],j=i;
        for(;j>=gap && cmp(a[j-gap],t)>0;j-=gap) a[j]=a[j-gap];
        a[j]=t;
    }
}
/* ID3v2.2/2.3/2.4 text frames (fallback when the property store has no music tags). */
static WCHAR *music_id3_text(const BYTE *p,int n) {
    if(n<1) return 0;
    int enc=p[0];p++;n--;
    WCHAR buf[512];int len=0;
    if(enc==1 || enc==2) {
        BOOL be=enc==2;
        if(n>=2 && p[0]==0xFF && p[1]==0xFE) {be=FALSE;p+=2;n-=2;}
        else if(n>=2 && p[0]==0xFE && p[1]==0xFF) {be=TRUE;p+=2;n-=2;}
        for(int i=0;i+1<n && len<511;i+=2) {WCHAR c=be?(WCHAR)(p[i]<<8|p[i+1]):(WCHAR)(p[i]|p[i+1]<<8);if(!c) break;buf[len++]=c;}
    } else {
        int m=0;while(m<n && p[m]) m++;
        if(m>511) m=511;
        if(enc==3) len=MultiByteToWideChar(CP_UTF8,0,(const char *)p,m,buf,511);
        else for(int i=0;i<m;i++) buf[len++]=p[i];   /* ISO-8859-1 */
        if(len<0) len=0;
    }
    while(len && (buf[len-1]==L' ' || !buf[len-1])) len--;
    return len?music_strdup(buf,len):0;
}
static void music_id3(MusicSong *s) {
    HANDLE f=CreateFileW(s->path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);
    if(f==INVALID_HANDLE_VALUE) return;
    BYTE h[10];DWORD got=0;
    if(!ReadFile(f,h,10,&got,0) || got!=10 || h[0]!='I' || h[1]!='D' || h[2]!='3' || h[3]<2 || h[3]>4) {CloseHandle(f);return;}
    DWORD size=(h[6]&0x7f)<<21|(h[7]&0x7f)<<14|(h[8]&0x7f)<<7|(h[9]&0x7f);
    if(size>1<<20) size=1<<20;
    BYTE *b=HeapAlloc(GetProcessHeap(),0,size+1);
    if(!b) {CloseHandle(f);return;}
    if(!ReadFile(f,b,size,&got,0)) got=0;
    CloseHandle(f);
    int v=h[3],at=0,n=(int)got;
    if(v>=3 && h[5]&0x40 && n>=4)   /* extended header */
        at=v==4?(b[0]&0x7f)<<21|(b[1]&0x7f)<<14|(b[2]&0x7f)<<7|(b[3]&0x7f):(b[0]<<24|b[1]<<16|b[2]<<8|b[3])+4;
    int idn=v==2?3:4,hn=v==2?6:10;
    while(at>=0 && at+hn<=n && b[at]) {
        int fs;
        if(v==2) fs=b[at+3]<<16|b[at+4]<<8|b[at+5];
        else if(v==4) fs=(b[at+4]&0x7f)<<21|(b[at+5]&0x7f)<<14|(b[at+6]&0x7f)<<7|(b[at+7]&0x7f);
        else fs=b[at+4]<<24|b[at+5]<<16|b[at+6]<<8|b[at+7];
        if(fs<=0 || at+hn+fs>n) break;
        char id[5]={0};memcpy(id,b+at,idn);
        const BYTE *p=b+at+hn;
        WCHAR **slot=0;BOOL track=FALSE;
        if(!lstrcmpA(id,"TIT2") || !lstrcmpA(id,"TT2")) slot=&s->title;
        else if(!lstrcmpA(id,"TPE1") || !lstrcmpA(id,"TP1")) slot=&s->artist;
        else if(!lstrcmpA(id,"TALB") || !lstrcmpA(id,"TAL")) slot=&s->album;
        else if(!lstrcmpA(id,"TCON") || !lstrcmpA(id,"TCO")) slot=&s->genre;
        else if(!lstrcmpA(id,"TRCK") || !lstrcmpA(id,"TRK")) track=TRUE;
        if(slot && !*slot) *slot=music_id3_text(p,fs);
        if(track && !s->track) {
            WCHAR *t=music_id3_text(p,fs);
            if(t) {int k=0;for(WCHAR *c=t;*c>=L'0' && *c<=L'9';c++) k=k*10+(*c-L'0');s->track=k;HeapFree(GetProcessHeap(),0,t);}
        }
        at+=hn+fs;
    }
    HeapFree(GetProcessHeap(),0,b);
}
/* Shell property store (Windows: mp3/wma/m4a/flac... handlers). Keys from propkey.h. */
static const GUID music_fmtid_summary={0xF29F85E0,0x4FF9,0x1068,{0xAB,0x91,0x08,0x00,0x2B,0x27,0xB3,0xD9}};
static const GUID music_fmtid_music={0x56A3372E,0xCE9C,0x11D2,{0x9F,0x0E,0x00,0x60,0x97,0xC6,0x86,0xF6}};
static const GUID music_iid_store={0x886D8EEB,0x8CF2,0x4446,{0x8D,0x02,0xBD,0xBA,0x1D,0xBD,0xCF,0x99}};
static WCHAR *music_prop_text(IPropertyStore *store,const GUID *fmt,DWORD pid,int *number) {
    PROPERTYKEY key={*fmt,pid};PROPVARIANT pv;PropVariantInit(&pv);
    WCHAR *r=0;
    if(SUCCEEDED(store->lpVtbl->GetValue(store,&key,&pv))) {
        const WCHAR *s=0;
        if(pv.vt==VT_LPWSTR) s=pv.pwszVal;
        else if(pv.vt==VT_BSTR) s=pv.bstrVal;
        else if(pv.vt==(VT_VECTOR|VT_LPWSTR) && pv.calpwstr.cElems) s=pv.calpwstr.pElems[0];
        else if(number && (pv.vt==VT_UI4 || pv.vt==VT_I4 || pv.vt==VT_UINT)) *number=(int)pv.ulVal;
        if(s && *s) r=music_strdup(s,-1);
        PropVariantClear(&pv);
    }
    return r;
}
static void music_tags(MusicSong *s) {
    typedef HRESULT (WINAPI *GetStore)(PCWSTR,void *,int,REFIID,void **);
    static GetStore get;static BOOL looked;
    if(!looked) {looked=TRUE;HMODULE shell=GetModuleHandleW(L"shell32.dll");if(shell) get=(GetStore)(void *)GetProcAddress(shell,"SHGetPropertyStoreFromParsingName");}
    IPropertyStore *store=0;
    if(get && SUCCEEDED(get(s->path,0,0,&music_iid_store,(void **)&store)) && store) {
        s->title=music_prop_text(store,&music_fmtid_summary,2,0);
        s->artist=music_prop_text(store,&music_fmtid_music,2,0);
        if(!s->artist) s->artist=music_prop_text(store,&music_fmtid_music,13,0);   /* album artist */
        s->album=music_prop_text(store,&music_fmtid_music,4,0);
        s->genre=music_prop_text(store,&music_fmtid_music,11,0);
        music_prop_text(store,&music_fmtid_music,7,&s->track);
        store->lpVtbl->Release(store);
    }
    if(!s->title || !s->artist || !s->album || !s->genre || !s->track) music_id3(s);
    if(!s->title) {   /* file name without extension */
        const WCHAR *name=wcsrchr(s->path,L'\\');name=name?name+1:s->path;
        const WCHAR *dot=wcsrchr(name,L'.');
        s->title=music_strdup(name,dot?(int)(dot-name):-1);
    }
    /* minimediaplayer Strings.xus */
    if(!s->artist) s->artist=L"Unknown Artist";
    if(!s->album) s->album=L"Unknown Album";
    if(!s->genre) s->genre=L"Unknown Genre";
}
static int music_find_path(const WCHAR *path) {
    int lo=0,hi=music_song_count-1;
    while(lo<=hi) {
        int mid=(lo+hi)/2,c=lstrcmpiW(music_songs[mid].path,path);
        if(!c) return mid;
        if(c<0) lo=mid+1;else hi=mid-1;
    }
    return -1;
}
static void music_read_playlist(const WCHAR *file) {
    if(music_playlist_count>=MUSIC_LISTS) return;
    HANDLE f=CreateFileW(file,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);if(f==INVALID_HANDLE_VALUE) return;
    DWORD size=GetFileSize(f,0),got=0;if(size>1<<20) size=1<<20;
    char *b=HeapAlloc(GetProcessHeap(),0,size+1);
    if(!b) {CloseHandle(f);return;}
    if(!ReadFile(f,b,size,&got,0)) got=0;
    CloseHandle(f);b[got]=0;
    const WCHAR *dot=wcsrchr(file,L'.');
    UINT cp=dot && !lstrcmpiW(dot,L".m3u8")?CP_UTF8:CP_ACP;
    char *at=b;if(got>=3 && (BYTE)at[0]==0xEF && (BYTE)at[1]==0xBB && (BYTE)at[2]==0xBF) {at+=3;cp=CP_UTF8;}
    WCHAR dir[MAX_PATH];lstrcpynW(dir,file,MAX_PATH);WCHAR *slash=wcsrchr(dir,L'\\');if(slash) *slash=0;
    MusicPlaylist *pl=&music_playlists[music_playlist_count];
    pl->first=music_playlist_item_count;pl->count=0;
    while(*at) {
        char *end=at;while(*end && *end!='\n' && *end!='\r') end++;
        char saved=*end;*end=0;
        if(*at && *at!='#') {
            WCHAR line[MAX_PATH],path[MAX_PATH];
            int n=MultiByteToWideChar(cp,0,at,-1,line,MAX_PATH);
            if(n>0) {
                for(WCHAR *c=line;*c;c++) if(*c==L'/') *c=L'\\';
                if(line[0]==L'\\' || (line[0] && line[1]==L':')) lstrcpynW(path,line,MAX_PATH);
                else if(lstrlenW(dir)+lstrlenW(line)+2<MAX_PATH) wsprintfW(path,L"%s\\%s",dir,line);
                else path[0]=0;
                WCHAR full[MAX_PATH];
                if(path[0] && GetFullPathNameW(path,MAX_PATH,full,0)) {
                    int song=music_find_path(full);
                    if(song>=0 && music_playlist_item_count<MUSIC_MAX) {music_playlist_items[music_playlist_item_count++]=song;pl->count++;}
                }
            }
        }
        *end=saved;at=*end?end+1:end;
    }
    HeapFree(GetProcessHeap(),0,b);
    const WCHAR *name=wcsrchr(file,L'\\');name=name?name+1:file;
    pl->name=music_strdup(name,dot && dot>name?(int)(dot-name):-1);
    if(pl->name) music_playlist_count++;
}
static DWORD WINAPI music_scan_thread(void *unused) {
    HRESULT com=CoInitializeEx(0,COINIT_MULTITHREADED);
    WCHAR dir[MAX_PATH];
    if(SUCCEEDED(SHGetFolderPathW(NULL,CSIDL_MYMUSIC,NULL,SHGFP_TYPE_CURRENT,dir))) music_scan_dir(dir,0);
    GetModuleFileNameW(0,dir,MAX_PATH);WCHAR *slash=wcsrchr(dir,L'\\');
    if(slash) {lstrcpyW(slash+1,L"music");music_scan_dir(dir,0);}
    music_sort_ptr(music_scan_paths,music_scan_count);
    int n=0;
    for(int i=0;i<music_scan_count;i++) {
        if(n && !lstrcmpiW(music_songs[n-1].path,music_scan_paths[i])) continue;   /* Music folder == exe\music */
        music_songs[n].path=music_scan_paths[i];music_tags(&music_songs[n]);n++;
    }
    music_song_count=n;
    music_sort_ptr(music_scan_lists,music_scan_list_count);
    for(int i=0;i<music_scan_list_count;i++) music_read_playlist(music_scan_lists[i]);
    if(SUCCEEDED(com)) CoUninitialize();
    InterlockedExchange(&music_scan_state,2);
    return 0;
}
static BOOL music_ready(void) {return music_scan_state==2;}

/* ---------- playback ---------- */
static int music_current(void) {return music_pos>=0 && music_pos<music_queue_count?music_queue[music_order[music_pos]]:-1;}
/* Play order: in order, or shuffled with the song at queue index keep first. */
static void music_reorder(int keep) {
    for(int i=0;i<music_queue_count;i++) music_order[i]=i;
    if(music_shuffle) for(int i=music_queue_count-1;i>0;i--) {
        int j=(int)((GetTickCount()*2654435761u+(unsigned)i*40503u)%(unsigned)(i+1));
        int t=music_order[i];music_order[i]=music_order[j];music_order[j]=t;
    }
    if(keep>=0) for(int i=0;i<music_queue_count;i++) if(music_order[i]==keep) {
        if(music_shuffle) {int t=music_order[0];music_order[0]=music_order[i];music_order[i]=t;music_pos=0;}
        else music_pos=i;
        break;
    }
}
static void music_close(void) {if(music_open_ok) music_mci(L"close blades_song",0,0);music_open_ok=FALSE;}
static void music_image(const WCHAR *id,const WCHAR *png) {
    static WCHAR path[2][4200];int k=!lstrcmpW(id,L"btnPlay")?0:1;
    DWORD e=guide_find(blades_music_scene,id);if(!e) return;
    guide_uri(path[k],L"hud_hud.xzp",png);xui_string(e,L"ImagePath",path[k]);
}
static void music_ui(void) {
    if(!blades_music_scene) return;
    int song=music_current();
    dash_set_text(blades_music_scene,L"btnPlaylist",song>=0?music_songs[song].title:L"Select Music");   /* hud Strings.xus 28 */
    music_image(L"btnPlay",music_open_ok && !music_paused?L"mediaPause.png":L"mediaPlay.png");
    music_image(L"btnPlaybackMode",music_shuffle?L"mediaShufflePlayback.png":L"mediaInOrderPlayback.png");
}
static int music_mci_volume(void) {return music_volume_max>0?music_volume*1000/music_volume_max:1000;}
static BOOL music_start(int pos) {
    for(;pos>=0 && pos<music_queue_count;pos++) {   /* past the end of the queue: stop */
        music_pos=pos;music_close();
        const MusicSong *s=&music_songs[music_current()];
        WCHAR cmd[MAX_PATH+64];wsprintfW(cmd,L"open \"%s\" type mpegvideo alias blades_song",s->path);
        if(music_mci(cmd,0,0)) continue;           /* undecodable file: skip it */
        music_open_ok=TRUE;
        music_mci(L"set blades_song time format milliseconds",0,0);
        wsprintfW(cmd,L"setaudio blades_song volume to %d",music_mci_volume());music_mci(cmd,0,0);
        if(music_mci(L"play blades_song",0,0)) {music_close();continue;}
        music_paused=FALSE;log_text("Music playing");log_wide(s->title);music_ui();return TRUE;
    }
    /* Nothing (more) to play: the queue stays, stopped at its start. */
    music_close();music_paused=FALSE;
    music_pos=music_queue_count?0:-1;
    log_text("Music stopped");music_ui();return FALSE;
}
static void music_set_queue(const int *ids,int n,int start) {
    if(n>MUSIC_QUEUE_LIMIT) n=MUSIC_QUEUE_LIMIT;
    if(start>=n || start<0) start=0;
    for(int i=0;i<n;i++) music_queue[i]=ids[i];
    music_queue_count=n;music_pos=0;
    music_reorder(start);
}
static void music_toggle(void) {
    if(!music_open_ok) {music_start(music_pos<0?0:music_pos);return;}
    music_mci(music_paused?L"resume blades_song":L"pause blades_song",0,0);
    music_paused=!music_paused;music_ui();
}
static void music_step(int delta) {
    if(!music_queue_count) return;
    if(delta<0 && music_open_ok) {   /* restart the song unless near its start */
        WCHAR v[32]=L"0";music_mci(L"status blades_song position",v,32);
        int ms=0;for(const WCHAR *c=v;*c>=L'0' && *c<=L'9';c++) ms=ms*10+(*c-L'0');
        if(ms>3000) {music_mci(L"play blades_song from 0",0,0);music_paused=FALSE;music_ui();return;}
    }
    int pos=music_pos+delta;
    if(pos<0) pos=music_queue_count-1;
    if(pos>=music_queue_count) pos=0;
    music_start(pos);
}
static void music_apply_volume(void) {
    if(!music_open_ok) return;
    WCHAR cmd[64];wsprintfW(cmd,L"setaudio blades_song volume to %d",music_mci_volume());music_mci(cmd,0,0);
}

/* ---------- mini player ---------- */
static void music_volume_ui(BOOL on) {
    if(!blades_music_scene) return;
    music_volume_mode=on;
    guide_show(blades_music_scene,L"sliderVolume",on);guide_show(blades_music_scene,L"imgVolume",on);
    guide_show(blades_music_scene,L"sceneControlsContainer",!on);
    DWORD slider=guide_find(blades_music_scene,L"sliderVolume");
    if(slider && on) {
        music_setting_slider=TRUE;
        music_volume_max=xui_get_int(slider,L"RangeMax",100);if(music_volume_max<=0) music_volume_max=100;
        if(music_volume>music_volume_max) music_volume=music_volume_max;
        xui_int(slider,L"Value",music_volume);
        music_setting_slider=FALSE;
    }
    DWORD focus=guide_find(blades_music_scene,on?L"sliderVolume":L"btnVolume");if(focus) xui_focus(focus);
    log_value("Music volume mode",on);
}

/* ---------- picker pages ---------- */
static int music_catalog(int kind) {
    static const WCHAR *files[6]={0,L"579_SelectMediaSource.xur",L"570_MusicLibrary.xur",L"572_SelectMediaContainer.xur",L"576_SelectSong.xur",L"BeginPlayback.xur"};
    if(kind<=0 || kind>5) return -1;
    if(music_cat[kind]==-2) music_cat[kind]=blades_catalog(L"minimediaplayer_media.xzp",files[kind]);
    return music_cat[kind];
}
static int music_kind(int catalog) {
    for(int k=1;k<6;k++) if(catalog>=0 && catalog==music_catalog(k)) return k;
    return MP_NONE;
}
static int music_top_kind(void) {
    if(!blades_guide_open) return MP_NONE;
    BladesPage *page=blades_page();
    return page?music_kind(page->catalog):MP_NONE;
}
/* Songs order inside a container. */
static int music_by_track(int a,int b) {
    const MusicSong *x=&music_songs[a],*y=&music_songs[b];
    if(x->track!=y->track) return x->track<y->track?-1:1;
    int c=lstrcmpiW(x->title,y->title);return c?c:a-b;
}
static int music_by_album(int a,int b) {int c=lstrcmpiW(music_songs[a].album,music_songs[b].album);return c?c:music_by_track(a,b);}
static int music_by_artist(int a,int b) {int c=lstrcmpiW(music_songs[a].artist,music_songs[b].artist);return c?c:music_by_album(a,b);}
static int music_by_title(int a,int b) {int c=lstrcmpiW(music_songs[a].title,music_songs[b].title);return c?c:music_by_artist(a,b);}
static const WCHAR *music_field(int song,int kind) {
    const MusicSong *s=&music_songs[song];
    return kind==MC_ALBUMS?s->album:kind==MC_ARTISTS?s->artist:s->genre;
}
static void music_build_containers(int kind) {
    music_cont_kind=kind;music_cont_count=0;
    if(kind==MC_PLAYLISTS) {
        for(int i=0;i<music_playlist_count;i++) music_cont_names[music_cont_count++]=music_playlists[i].name;
        music_sort_ptr(music_cont_names,music_cont_count);
        return;
    }
    static WCHAR *all[MUSIC_MAX];
    for(int i=0;i<music_song_count;i++) all[i]=(WCHAR *)music_field(i,kind);
    music_sort_ptr(all,music_song_count);
    for(int i=0;i<music_song_count;i++)
        if(!music_cont_count || lstrcmpiW(music_cont_names[music_cont_count-1],all[i])) music_cont_names[music_cont_count++]=all[i];
}
/* Songs of container i of the current kind, into out[]. */
static int music_container_songs(int i,int *out) {
    int n=0;
    if(i<0 || i>=music_cont_count) return 0;
    if(music_cont_kind==MC_PLAYLISTS) {
        for(int p=0;p<music_playlist_count;p++) if(music_playlists[p].name==music_cont_names[i]) {
            for(int k=0;k<music_playlists[p].count;k++) out[n++]=music_playlist_items[music_playlists[p].first+k];
            break;
        }
        return n;
    }
    for(int s=0;s<music_song_count;s++) if(!lstrcmpiW(music_field(s,music_cont_kind),music_cont_names[i])) out[n++]=s;
    music_sort_int(out,n,music_cont_kind==MC_ALBUMS?music_by_track:music_cont_kind==MC_ARTISTS?music_by_album:music_by_artist);
    return n;
}
static int music_list_count_fn(void *ctx) {return ctx?music_cont_count:music_list_count;}
static const WCHAR *music_list_text_fn(void *ctx,int i) {
    if(ctx) return i>=0 && i<music_cont_count?music_cont_names[i]:0;
    return i>=0 && i<music_list_count?music_songs[music_list[i]].title:0;
}
static int music_source_count(void *ctx) {return music_ready() && music_song_count?1:0;}
static const WCHAR *music_source_text(void *ctx,int i) {return i==0?L"Hard Drive":0;}   /* Strings.xus "Hard Drive" */
static const WCHAR *music_source_image(void *ctx,int i) {
    static WCHAR uri[4200];
    if(i) return 0;
    if(!uri[0]) guide_uri(uri,L"minimediaplayer_media.xzp",L"ico_32x_HD_black.png");
    return uri;
}
/* XuiCommonList keeps its own items (proc 0x7fb619): 0x7dd insert {index,count}, 0x7de delete
   {index,count}, 0x7e0 set text {index,0,1,text}, 0x7e2 set image {index,0,1,0,path}. Its 0x7dd is
   not the XuiList refresh, so guide_list_bind (refresh with no data) must not be used on it. */
static void music_common_list(DWORD list,int n,const WCHAR *(*text)(void *,int),const WCHAR *(*image)(void *,int)) {
    DWORD count[2]={0,0};xui_send(list,0x7dc,count,sizeof count);
    if((int)count[1]>0) {DWORD del[2]={0,count[1]};xui_send(list,0x7de,del,sizeof del);}
    if(n>0) {DWORD ins[2]={0,(DWORD)n};xui_send(list,0x7dd,ins,sizeof ins);}
    for(int i=0;i<n;i++) {
        DWORD t[10]={(DWORD)i,0,1,(DWORD)text(0,i)};xui_send(list,0x7e0,t,sizeof t);
        const WCHAR *img=image?image(0,i):0;
        if(img) {DWORD m[10]={(DWORD)i,0,1,0,(DWORD)img};xui_send(list,0x7e2,m,sizeof m);}
    }
    xui_bool(list,L"Show",n>0);
}
static void music_item_count(DWORD page,DWORD list,int count) {
    int sel=list?xui_cursel(list):0;if(sel<0) sel=0;
    if(count) wsprintfW(music_count_text,L"%d of %d",sel+1,count);   /* Strings.xus "%d of %d" */
    else music_count_text[0]=0;
    guide_text(page,L"labelItemCount",music_count_text);
}
static void music_fill_page(DWORD page,int kind) {
    if(!page) return;
    if(kind==MP_SOURCES) {
        guide_show(page,L"scnDetectedIPod",FALSE);   /* only while a USB device is scanned */
        DWORD list=guide_find(page,L"listSources");
        BOOL any=music_ready() && music_song_count>0;
        guide_show(page,L"labelNoMediaSourcesText",music_ready() && !any);
        if(list) {
            xui_string(list,L"Visual",L"hud.List_standard");   /* authored; rows = btn_standard */
            music_common_list(list,music_source_count(0),music_source_text,music_source_image);
            if(any) {xui_set_cursel(list,0);xui_focus(list);}
        }
    } else if(kind==MP_CONTAINERS || kind==MP_SONGS) {
        BOOL containers=kind==MP_CONTAINERS;
        static const WCHAR *heads[]={L"Albums",L"Artists",L"Saved Playlists",L"Genres"};
        static const WCHAR *empty[]={L"No albums found.",L"No artists found.",L"No playlists found.",L"No genres found."};
        guide_text(page,L"labelHeader",containers?heads[music_cont_kind&3]:music_list_header);
        guide_show(page,L"labelLoadingText",FALSE);guide_show(page,L"labelLoadingAnimation",FALSE);
        int count=containers?music_cont_count:music_list_count;
        DWORD list=guide_find(page,L"listItems");
        guide_text(page,L"labelNoResultsText",containers?empty[music_cont_kind&3]:L"No songs found.");
        guide_show(page,L"labelNoResultsText",count==0);
        if(list) {
            /* 572: no authored visual: the hud skin's class default XuiList (rows = its XuiButton).
               576: List_prgbr_2string is in no supplied skin, so the class default applies there too. */
            xui_string(list,L"Visual",L"hud.XuiList");
            if(count) {guide_list_bind(list,music_list_count_fn,music_list_text_fn,0,containers?(void *)1:0);xui_set_cursel(list,0);xui_focus(list);}
            else xui_bool(list,L"Show",FALSE);
        }
        music_item_count(page,count?list:0,count);
    }
}
static void music_open(int kind) {
    int catalog=music_catalog(kind);
    if(catalog<0) {log_text("Music picker scene missing");return;}
    blades_navigate(catalog);
}
/* Start playing a request: the 100 song limit notice first, then BeginPlayback. */
static void music_begin(void) {
    music_begin_pending=FALSE;
    if(!blades_guide_open) return;
    music_open(MP_BEGIN);
    music_begin_wait=24;
}
static void music_limit_done(int choice,void *ctx) {music_begin_pending=TRUE;}
static void music_request(const int *ids,int n,int start) {
    if(n<=0) return;
    music_request_count=n;music_request_start=start;
    for(int i=0;i<n;i++) music_request_ids[i]=ids[i];
    if(n>MUSIC_QUEUE_LIMIT) {
        static const WCHAR *ok[]={L"OK"};   /* Strings.xus "Song Limit" / "OK" */
        log_text("Music song limit");
        guide_msgbox(L"Song Limit",L"Can't play all songs. Only the first 100 songs will be played.",ok,1,0,music_limit_done,0);
        return;
    }
    music_begin_pending=TRUE;
}
static void music_play_all(void) {
    static int all[MUSIC_MAX];
    if(!music_ready() || !music_song_count) return;
    for(int i=0;i<music_song_count;i++) all[i]=i;
    music_request(all,music_song_count,0);
}
static int music_selected(int kind) {
    BladesPage *page=blades_page();
    DWORD list=page?guide_find(page->scene,L"listItems"):0;
    int sel=list?xui_cursel(list):-1;
    int count=kind==MP_CONTAINERS?music_cont_count:music_list_count;
    return sel>=0 && sel<count?sel:-1;
}
static void music_open_container(int i) {
    music_list_count=music_container_songs(i,music_list);
    lstrcpynW(music_list_header,music_cont_names[i],256);
    music_open(MP_SONGS);
}
static void music_play_container(int i) {
    static int ids[MUSIC_MAX];int n=music_container_songs(i,ids);
    music_request(ids,n,0);
}
/* Y on a picker page (the base treats Y as Xbox Dashboard in the Guide). */
static void music_y(void) {
    int kind=music_top_kind();
    if(!music_ready()) return;
    if(kind==MP_SOURCES || kind==MP_LIBRARY) {music_play_all();return;}   /* Play All Music */
    if(kind==MP_CONTAINERS) {int i=music_selected(kind);if(i>=0) music_play_container(i);return;}  /* Play Album/Artist/... */
    if(kind==MP_SONGS) {int i=music_selected(kind);if(i>=0) music_request(music_list,music_list_count,i);}  /* Play Song */
}
/* Called by blades_key for Y while the Guide is open; TRUE = a picker page owns Y. */
static BOOL music_key_y(BOOL down) {
    if(!music_top_kind()) return FALSE;
    if(down) music_y_pending=TRUE;
    return TRUE;
}
/* Back to the Guide main menu from the picker. */
static void music_close_picker(void) {
    for(int guard=blades_guide_stack.depth+2;guard>0 && blades_guide_stack.depth && music_top_kind();guard--) blades_back();
}

/* ---------- hooks ---------- */
static BOOL music_dispatch(DWORD button,const WCHAR *id) {
    if(!blades_guide_open) return FALSE;
    if(blades_music_scene && blades_contains(blades_music_scene,button,0)) {
        if(!lstrcmpW(id,L"btnPlay")) {
            if(!music_queue_count) music_play_all();   /* nothing queued: Play All */
            else music_toggle();
            return TRUE;
        }
        if(!lstrcmpW(id,L"btnPrev")) {music_step(-1);return TRUE;}
        if(!lstrcmpW(id,L"btnNext")) {music_step(1);return TRUE;}
        if(!lstrcmpW(id,L"btnPlaybackMode")) {
            int keep=music_pos>=0 && music_pos<music_queue_count?music_order[music_pos]:-1;
            music_shuffle=!music_shuffle;console_set_int(L"MusicShuffle",music_shuffle);
            music_reorder(keep);music_ui();log_value("Music shuffle",music_shuffle);return TRUE;
        }
        if(!lstrcmpW(id,L"btnVolume")) {music_volume_ui(TRUE);return TRUE;}
        if(!lstrcmpW(id,L"btnDismissVolumeSliderA") || !lstrcmpW(id,L"btnDismissVolumeSliderB")) {music_volume_ui(FALSE);return TRUE;}
        if(!lstrcmpW(id,L"btnPlaylist")) {music_open(MP_SOURCES);return TRUE;}
        return FALSE;
    }
    int kind=music_top_kind();
    if(!kind) return FALSE;
    if(!lstrcmpW(id,L"btnY")) {music_y();return TRUE;}
    if(!lstrcmpW(id,L"btnX")) {if(kind!=MP_SOURCES) music_close_picker();return TRUE;}   /* Cancel; 579's X is "Return control to game" */
    if(!music_ready()) return TRUE;
    if(kind==MP_LIBRARY) {
        static const WCHAR *ids[]={L"btnAlbums",L"btnArtists",L"btnPlaylists",L"btnGenres"};
        if(!lstrcmpW(id,L"btnPlayAll")) {music_play_all();return TRUE;}
        for(int i=0;i<4;i++) if(!lstrcmpW(id,ids[i])) {music_build_containers(i);music_open(MP_CONTAINERS);return TRUE;}
        if(!lstrcmpW(id,L"btnAllSongs")) {
            for(int i=0;i<music_song_count;i++) music_list[i]=i;
            music_list_count=music_song_count;music_sort_int(music_list,music_list_count,music_by_title);
            lstrcpyW(music_list_header,L"Songs");music_open(MP_SONGS);return TRUE;
        }
    }
    if(kind==MP_CONTAINERS && !lstrcmpW(id,L"btnPlayAll")) {
        static int ids[MUSIC_MAX],part[MUSIC_MAX];int n=0;
        for(int i=0;i<music_cont_count && n<MUSIC_MAX;i++) {
            int k=music_container_songs(i,part);
            for(int j=0;j<k && n<MUSIC_MAX;j++) ids[n++]=part[j];
        }
        music_request(ids,n,0);return TRUE;
    }
    if(kind==MP_SONGS && !lstrcmpW(id,L"btnPlayAll")) {music_request(music_list,music_list_count,0);return TRUE;}
    return TRUE;   /* nothing else on these pages has an action (e.g. A with an empty source list) */
}
static void music_notify(DWORD src,DWORD code,DWORD *data) {
    if(!blades_guide_open) return;
    DWORD slider=blades_music_scene?guide_find(blades_music_scene,L"sliderVolume"):0;
    if(code==0xB && slider && src==slider) {
        if(music_setting_slider) return;
        music_volume=xui_get_int(src,L"Value",music_volume);
        console_set_int(L"MusicVolume",music_volume);
        music_apply_volume();log_value("Music volume",music_volume);
        return;
    }
    if(code==2 && music_volume_mode && slider && src!=slider) {
        /* focus left the slider (Down to btnPlaylist): leave volume mode, focus stays where it went */
        music_volume_mode=FALSE;
        guide_show(blades_music_scene,L"sliderVolume",FALSE);guide_show(blades_music_scene,L"imgVolume",FALSE);
        guide_show(blades_music_scene,L"sceneControlsContainer",TRUE);
        return;
    }
    int kind=music_top_kind();
    if(!kind) return;
    BladesPage *page=blades_page();
    DWORD list=guide_find(page->scene,kind==MP_SOURCES?L"listSources":L"listItems");
    if(!list || src!=list) return;
    if(code==4 && (kind==MP_CONTAINERS || kind==MP_SONGS)) {
        music_item_count(page->scene,list,kind==MP_CONTAINERS?music_cont_count:music_list_count);return;
    }
    if(code!=1 || !music_ready()) return;
    /* act from music_tick: navigating inside the list's own press handling loses the new page's focus */
    music_press_kind=kind;music_press_sel=xui_cursel(list);
}
static void music_list_press(int kind,int sel) {
    if(kind!=music_top_kind()) return;
    if(kind==MP_SOURCES && sel==0 && music_song_count) music_open(MP_LIBRARY);
    else if(kind==MP_CONTAINERS && sel>=0 && sel<music_cont_count) music_open_container(sel);
    else if(kind==MP_SONGS && sel>=0 && sel<music_list_count) music_request(music_list,music_list_count,sel);
}
static void music_page(DWORD page,int catalog) {
    int kind=music_kind(catalog);
    if(!kind) return;
    log_value("Music picker page",kind);
    music_fill_page(page,kind);
}
static BOOL music_back(void) {
    if(blades_guide_open && music_volume_mode) {music_volume_ui(FALSE);return TRUE;}   /* btnDismissVolumeSliderB */
    return FALSE;
}
static void music_tick(void) {
    if(!music_loaded_settings) {
        music_loaded_settings=TRUE;
        music_volume=console_get_int(L"MusicVolume",100);
        music_shuffle=console_get_int(L"MusicShuffle",0)!=0;
        InterlockedExchange(&music_scan_state,1);
        HANDLE t=CreateThread(0,0,music_scan_thread,0,0,0);
        if(t) CloseHandle(t);else music_scan_thread(0);
    }
    if(music_scan_state==2 && !music_pages_pending) {
        music_pages_pending=TRUE;
        log_value("Music files found",music_song_count);log_value("Music playlists found",music_playlist_count);
        /* the source page may have opened during the scan */
        if(music_top_kind()==MP_SOURCES) {BladesPage *page=blades_page();music_fill_page(page->scene,MP_SOURCES);guide_legend(page->scene);}
    }
    if(blades_music_scene!=music_ui_scene) {   /* the mini player was (re)loaded with the Guide */
        music_ui_scene=blades_music_scene;
        if(blades_music_scene) {music_volume_mode=FALSE;music_ui();}
    }
    if(blades_guide_open!=music_guide_was_open) {
        music_guide_was_open=blades_guide_open;
        /* blades_guide(TRUE) drops the pages left open, so the Guide always reopens at its main menu */
        if(blades_guide_open && music_volume_mode) music_volume_ui(FALSE);
    }
    /* hudbkgnd's HalfToFull timeline keeps Legend_X shown while it runs (~45 frames): the page's
       legend (music_legend) is applied again by live_legend_tick once the frame has settled. */
    if(music_press_kind) {int kind=music_press_kind;music_press_kind=0;music_list_press(kind,music_press_sel);}
    if(music_y_pending) {music_y_pending=FALSE;music_y();}
    if(music_begin_pending) music_begin();
    if(music_begin_wait>=0) {
        if(music_top_kind()!=MP_BEGIN) music_begin_wait=-1;    /* B on BeginPlayback cancels */
        else if(music_begin_wait-- ==0) {
            music_begin_wait=-1;
            music_close();
            music_set_queue(music_request_ids,music_request_count,music_request_start);
            music_start(music_pos);
            music_close_picker();
            DWORD playlist=guide_find(blades_music_scene,L"btnPlaylist");if(playlist) xui_focus(playlist);
        }
    }
    /* end of song: next one (the mini player plays on while the Guide is closed) */
    if(music_open_ok && !music_paused && GetTickCount()-music_last_poll>=250) {
        music_last_poll=GetTickCount();
        WCHAR mode[32]=L"";
        if(!music_mci(L"status blades_song mode",mode,32) && !lstrcmpW(mode,L"stopped")) music_start(music_pos+1);
    }
}
static BOOL music_filter(DWORD element,DWORD *message) {
    return FALSE;
}
static void music_guide_opened(void) {}
static const GuideLegend *music_legend(DWORD scene) {
    static GuideLegend l;
    static const WCHAR *ys[]={L"Play Album",L"Play Artist",L"Play Saved Playlist",L"Play Genre"};
    BOOL any=music_ready() && music_song_count>0;
    l=(GuideLegend){0};
    if(ui_find_deep(scene,L"scnSelectMusicSource",0)) l=(GuideLegend){L"scnSelectMusicSource",any?L"Select":0,L"Back",0,any?L"Play All Music":0};
    else if(ui_find_deep(scene,L"scnMusicLibrary",0)) l=(GuideLegend){L"scnMusicLibrary",L"Select",L"Back",0,L"Play All Music"};
    else if(ui_find_deep(scene,L"scnMediaContainers",0)) l=(GuideLegend){L"scnMediaContainers",music_cont_count?L"Select":0,L"Back",0,music_cont_count?ys[music_cont_kind&3]:0};
    else if(ui_find_deep(scene,L"scnSongs",0)) l=(GuideLegend){L"scnSongs",music_list_count?L"Select":0,L"Back",0,music_list_count?L"Play Song":0};
    else if(ui_find_deep(scene,L"scnBeginPlayback",0)) l=(GuideLegend){L"scnBeginPlayback",0,0,0,0};
    return l.scene?&l:0;
}
