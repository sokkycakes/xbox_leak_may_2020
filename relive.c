/* ReLive content for the Live blade.
 *
 * ReLive (community Xbox Live "time capsule") serves its dashboard content from a fixed
 * host over plain HTTP with no console authentication (reversed from ReLive.xex, 2026-11-10):
 *   GET /path HTTP/1.0 / Host: <host> / User-Agent: ReLive
 * The content endpoints used here are unauthenticated:
 *   /blades/titles.txt        pipe-delimited featured-title list for the Blades dash
 *   /blades/botd/             index of Banner-of-the-Day packages (numbered .xzp)
 *   /blades/botd/<n>.xzp      one banner (a XUIZ package carrying banner.png or banner.jpg)
 * The authenticated services (sign-in, marketplace, presence) need the console CPU key and
 * keyvault, which this PC dashboard does not have; they are not touched here.
 *
 * The real Blades "Billboard of the Day" is the 420x358 game billboard (e.g. the Fable or
 * Star Wars Force Unleashed key art). ReLive also serves thin ad strips (345x94, 420x72,
 * 731x90) and a 345x240 placeholder tile in default.xzp; those belong to other dashboard
 * slots/themes, not the Live blade, so they are rejected here by size. We enumerate the
 * numbered packages, rotate through the 420x358 Blades billboards by day (so the billboard
 * changes daily), take the one for today, and show it on the Live blade.
 *
 * The fetch runs on a worker thread at start-up. A banner.jpg is transcoded to PNG (the
 * engine's image loader reads PNG, not JPEG from a package); a banner.png is used as-is.
 * Either way it is written into a small XUIZ package beside the dashboard resources, so the
 * Live blade can show it with an XuiImage. If the server is unreachable, or no Blades-shaped
 * banner is found, the blade keeps its authored default billboard.
 */
#include <objbase.h>
#include <wininet.h>

#define RELIVE_HOST      "198.251.80.201"
#define RELIVE_UA        L"ReLive"
#define RELIVE_FEAT_MAX  32
#define RELIVE_BOTD_MAX  48
#define RELIVE_PKG       L"relive.xzp"
#define RELIVE_IMG       L"relivebotd.png"

typedef struct {WCHAR name[96],publisher[64],category[32],desc[220];} ReliveFeat;
static ReliveFeat relive_feat[RELIVE_FEAT_MAX];
static volatile LONG relive_feat_n;
static volatile LONG relive_have_banner;      /* relive.xzp written and loadable */
static volatile LONG relive_done;             /* worker finished (success or not) */
static WCHAR relive_uri[2300];                /* file://<base>relive.xzp#relivebotd.png */
static WCHAR relive_res_dir[2048];            /* <base>/x360/blades-converted/ (native path) */

/* ---- HTTP GET into a heap buffer (caller frees) ---- */
static BYTE *relive_get(const char *path,DWORD *out_len) {
    *out_len=0;
    HINTERNET inet=InternetOpenW(RELIVE_UA,INTERNET_OPEN_TYPE_DIRECT,0,0,0);
    if(!inet) return 0;
    char url[256];wsprintfA(url,"http://%s%s",RELIVE_HOST,path);
    HINTERNET h=InternetOpenUrlA(inet,url,0,0,
        INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_NO_UI|INTERNET_FLAG_PRAGMA_NOCACHE,0);
    if(!h) {InternetCloseHandle(inet);return 0;}
    DWORD code=0,sz=sizeof code,idx=0;
    HttpQueryInfoA(h,HTTP_QUERY_STATUS_CODE|HTTP_QUERY_FLAG_NUMBER,&code,&sz,&idx);
    if(code && code!=200) {InternetCloseHandle(h);InternetCloseHandle(inet);return 0;}
    DWORD cap=0x10000,len=0;BYTE *buf=(BYTE *)HeapAlloc(GetProcessHeap(),0,cap);
    if(buf) for(;;) {
        if(len+0x4000>cap) {cap*=2;BYTE *nb=(BYTE *)HeapReAlloc(GetProcessHeap(),0,buf,cap);if(!nb){HeapFree(GetProcessHeap(),0,buf);buf=0;break;}buf=nb;}
        DWORD got=0;if(!InternetReadFile(h,buf+len,0x4000,&got)||!got) break;
        len+=got;
    }
    InternetCloseHandle(h);InternetCloseHandle(inet);
    if(buf) *out_len=len;
    return buf;
}

/* ---- XUIZ (.xzp) reader: find a named entry. Big-endian 360 package format. ---- */
static DWORD relive_be32(const BYTE *p){return (DWORD)p[0]<<24|(DWORD)p[1]<<16|(DWORD)p[2]<<8|p[3];}
static const BYTE *relive_xzp_find(const BYTE *d,DWORD n,const char *want,DWORD *out_sz) {
    if(n<22||memcmp(d,"XUIZ",4)) return 0;
    DWORD hsize=relive_be32(d+16);WORD count=(WORD)(d[20]<<8|d[21]);
    DWORD base=22+hsize,p=22;
    for(WORD i=0;i<count;i++) {
        if(p+9>n) break;
        DWORD size=relive_be32(d+p),off=relive_be32(d+p+4);BYTE nl=d[p+8];p+=9;
        char name[128];int k=0;for(int j=0;j<nl&&k<127;j++){WCHAR c=(WCHAR)(d[p+j*2]<<8|d[p+j*2+1]);name[k++]=(char)(c&0x7f);}name[k]=0;p+=nl*2;
        if(!lstrcmpiA(name,want)) {if(base+off+size>n) return 0;*out_sz=size;return d+base+off;}
    }
    return 0;
}

/* ---- image dimensions straight from the encoded bytes (to pick the Blades-shaped banner) ---- */
static int relive_png_dim(const BYTE *p,DWORD n,int *w,int *h) {
    if(n<24 || memcmp(p,"\x89PNG\r\n\x1a\n",8)) return 0;
    *w=(int)relive_be32(p+16);*h=(int)relive_be32(p+20);return 1;
}
static int relive_jpg_dim(const BYTE *j,DWORD n,int *w,int *h) {
    DWORD i=2;
    while(i+9<n) {
        if(j[i]!=0xFF) {i++;continue;}
        BYTE m=j[i+1];
        if(m>=0xC0 && m<=0xCF && m!=0xC4 && m!=0xC8 && m!=0xCC) {
            *h=(j[i+5]<<8)|j[i+6];*w=(j[i+7]<<8)|j[i+8];return 1;
        }
        if(m==0xD8 || m==0xD9 || (m>=0xD0 && m<=0xD7)) {i+=2;continue;}
        i+=2+((DWORD)(j[i+2]<<8)|j[i+3]);
    }
    return 0;
}
static int relive_write_file(const WCHAR *path,const BYTE *b,DWORD n) {
    HANDLE w=CreateFileW(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    if(w==INVALID_HANDLE_VALUE) return 0;
    DWORD got=0;int ok=WriteFile(w,b,n,&got,0)&&got==n;CloseHandle(w);return ok;
}

/* ---- GDI+ flat API (gdiplus.dll): transcode JPEG bytes to a PNG file ---- */
typedef struct {UINT32 d1;UINT16 d2,d3;BYTE d4[8];} RGUID;
static int relive_jpg_to_png(const BYTE *jpg,DWORD jlen,const WCHAR *png_path) {
    HMODULE g=LoadLibraryW(L"gdiplus.dll");if(!g) return 0;
    struct {UINT32 ver;void *cb;int sup,noc;} si={1,0,0,0};
    ULONG_PTR tok=0;
    int (WINAPI *Startup)(ULONG_PTR*,void*,void*)=(void*)GetProcAddress(g,"GdiplusStartup");
    void (WINAPI *Shutdown)(ULONG_PTR)=(void*)GetProcAddress(g,"GdiplusShutdown");
    int (WINAPI *LoadStream)(IStream*,void**)=(void*)GetProcAddress(g,"GdipLoadImageFromStream");
    int (WINAPI *SaveFile)(void*,const WCHAR*,const RGUID*,const void*)=(void*)GetProcAddress(g,"GdipSaveImageToFile");
    int (WINAPI *Dispose)(void*)=(void*)GetProcAddress(g,"GdipDisposeImage");
    if(!Startup||!Shutdown||!LoadStream||!SaveFile||!Dispose){FreeLibrary(g);return 0;}
    if(Startup(&tok,&si,0)){FreeLibrary(g);return 0;}
    int ok=0;
    HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,jlen);
    if(hg){void *m=GlobalLock(hg);memcpy(m,jpg,jlen);GlobalUnlock(hg);
        IStream *st=0;
        if(CreateStreamOnHGlobal(hg,TRUE,&st)==S_OK&&st) {
            void *img=0;
            if(LoadStream(st,&img)==0&&img) {
                /* PNG encoder CLSID {557CF406-1A04-11D3-9A73-0000F81EF32E} */
                RGUID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
                ok=(SaveFile(img,png_path,&png,0)==0);
                Dispose(img);
            }
            st->lpVtbl->Release(st);
        }
    }
    Shutdown(tok);FreeLibrary(g);
    return ok;
}

/* ---- write a one-entry XUIZ package (same big-endian format the dash loads) ---- */
static int relive_write_pkg(const WCHAR *png_path) {
    HANDLE f=CreateFileW(png_path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);
    if(f==INVALID_HANDLE_VALUE) return 0;
    DWORD psz=GetFileSize(f,0);BYTE *png=(BYTE*)HeapAlloc(GetProcessHeap(),0,psz);DWORD rd=0;
    if(!png||!ReadFile(f,png,psz,&rd,0)||rd!=psz){CloseHandle(f);if(png)HeapFree(GetProcessHeap(),0,png);return 0;}
    CloseHandle(f);
    const char *nm="relivebotd.png";int nl=(int)lstrlenA(nm);
    DWORD dirsz=9+nl*2;
    DWORD total=22+dirsz+psz;
    BYTE *pkg=(BYTE*)HeapAlloc(GetProcessHeap(),0,total);
    if(!pkg){HeapFree(GetProcessHeap(),0,png);return 0;}
    int o=0;
    memcpy(pkg,"XUIZ",4);o=4;
    #define PB32(v) do{pkg[o++]=(BYTE)((v)>>24);pkg[o++]=(BYTE)((v)>>16);pkg[o++]=(BYTE)((v)>>8);pkg[o++]=(BYTE)(v);}while(0)
    PB32(1);PB32(total);PB32(0);PB32(dirsz);
    pkg[o++]=0;pkg[o++]=1;              /* count u16 = 1 */
    PB32(psz);PB32(0);                  /* entry size, offset */
    pkg[o++]=(BYTE)nl;
    for(int j=0;j<nl;j++){pkg[o++]=0;pkg[o++]=(BYTE)nm[j];}   /* UTF-16BE name */
    memcpy(pkg+o,png,psz);o+=psz;
    #undef PB32
    WCHAR out[2300];wsprintfW(out,L"%s%s",relive_res_dir,RELIVE_PKG);
    HANDLE w=CreateFileW(out,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
    int ok=0;
    if(w!=INVALID_HANDLE_VALUE){DWORD n;ok=WriteFile(w,pkg,total,&n,0)&&n==total;CloseHandle(w);}
    HeapFree(GetProcessHeap(),0,png);HeapFree(GetProcessHeap(),0,pkg);
    return ok;
}

/* ---- parse /blades/titles.txt (TitleID|MediaID|slot|Name|img|publisher|category|desc) ---- */
static void relive_parse_titles(const BYTE *d,DWORD n) {
    LONG count=0;const BYTE *p=d,*end=d+n;
    while(p<end && count<RELIVE_FEAT_MAX) {
        const BYTE *nl=p;while(nl<end&&*nl!='\n'&&*nl!='\r')nl++;
        char line[1024];int ll=(int)(nl-p);if(ll>1023)ll=1023;memcpy(line,p,ll);line[ll]=0;
        /* split on '|' */
        char *f[8]={0};int nf=0;char *s=line;
        for(char *c=line;nf<8;c++){if(*c=='|'||*c==0){char term=*c;*c=0;f[nf++]=s;s=c+1;if(term==0)break;}}
        if(nf>=8 && f[3] && f[3][0]) {
            ReliveFeat *e=&relive_feat[count];memset(e,0,sizeof *e);
            MultiByteToWideChar(CP_UTF8,0,f[3],-1,e->name,96);
            MultiByteToWideChar(CP_UTF8,0,f[5]?f[5]:"",-1,e->publisher,64);
            MultiByteToWideChar(CP_UTF8,0,f[6]?f[6]:"",-1,e->category,32);
            MultiByteToWideChar(CP_UTF8,0,f[7]?f[7]:"",-1,e->desc,220);
            count++;
        }
        p=nl;while(p<end&&(*p=='\n'||*p=='\r'))p++;
    }
    relive_feat_n=count;
}

/* ---- enumerate the numbered banner packages from the /blades/botd/ autoindex ---- */
static int relive_botd_ids[RELIVE_BOTD_MAX];
static int relive_botd_n;
static void relive_list_botd(void) {
    relive_botd_n=0;
    DWORD n=0;BYTE *idx=relive_get("/blades/botd/",&n);
    if(!idx) return;
    for(DWORD i=0;i+6<n && relive_botd_n<RELIVE_BOTD_MAX;i++) {
        if(idx[i]=='h' && !memcmp(idx+i,"href=\"",6)) {
            DWORD j=i+6;int v=0,any=0;
            while(j<n && idx[j]>='0' && idx[j]<='9') {v=v*10+(idx[j]-'0');j++;any=1;}
            if(any && j+4<=n && !memcmp(idx+j,".xzp",4)) {
                int dup=0;for(int k=0;k<relive_botd_n;k++) if(relive_botd_ids[k]==v){dup=1;break;}
                if(!dup) relive_botd_ids[relive_botd_n++]=v;
            }
        }
    }
    HeapFree(GetProcessHeap(),0,idx);
    for(int a=0;a<relive_botd_n;a++) for(int b=a+1;b<relive_botd_n;b++)
        if(relive_botd_ids[b]<relive_botd_ids[a]){int t=relive_botd_ids[a];relive_botd_ids[a]=relive_botd_ids[b];relive_botd_ids[b]=t;}
}

/* Fetch one banner; if it's a 420x358 Blades billboard, package it and return 1. */
static int relive_try_banner(int id) {
    char path[64];wsprintfA(path,"/blades/botd/%d.xzp",id);
    DWORD blen=0;BYTE *botd=relive_get(path,&blen);
    if(!botd) return 0;
    int ok=0,w=0,h=0;
    DWORD isz=0,jsz=0;
    const BYTE *png=relive_xzp_find(botd,blen,"banner.png",&isz);
    const BYTE *jpg=0;
    int have_dim=0;
    if(png && isz) have_dim=relive_png_dim(png,isz,&w,&h);
    else {jpg=relive_xzp_find(botd,blen,"banner.jpg",&jsz);if(jpg&&jsz)have_dim=relive_jpg_dim(jpg,jsz,&w,&h);}
    /* Live-blade billboard is the 420x358 game billboard; skip thin ad strips and the
       345x240 default placeholder tile. */
    if(have_dim && w==420 && h>=300) {
        WCHAR tmp[2300];wsprintfW(tmp,L"%srelivebotd_tmp.png",relive_res_dir);
        int built = png ? relive_write_file(tmp,png,isz) : relive_jpg_to_png(jpg,jsz,tmp);
        if(built && relive_write_pkg(tmp)) {
            wsprintfW(relive_uri,L"file://%s%s#%s",relive_res_dir,RELIVE_PKG,RELIVE_IMG);
            for(WCHAR *c=relive_uri;*c;c++) if(*c==L'\\') *c=L'/';
            InterlockedExchange(&relive_have_banner,1);
            ok=1;
        }
        DeleteFileW(tmp);
    }
    HeapFree(GetProcessHeap(),0,botd);
    return ok;
}

static DWORD WINAPI relive_worker(void *unused) {
    (void)unused;
    DWORD len=0;
    BYTE *titles=relive_get("/blades/titles.txt",&len);
    if(titles){relive_parse_titles(titles,len);HeapFree(GetProcessHeap(),0,titles);}

    relive_list_botd();
    if(relive_botd_n) {
        /* Billboard of the Day: rotate through the Blades billboards by date. */
        SYSTEMTIME st;GetLocalTime(&st);
        int day=st.wYear*372+(st.wMonth-1)*31+(st.wDay-1);
        int start=((day%relive_botd_n)+relive_botd_n)%relive_botd_n;
        for(int k=0;k<relive_botd_n;k++)
            if(relive_try_banner(relive_botd_ids[(start+k)%relive_botd_n])) break;
    }
    InterlockedExchange(&relive_done,1);
    return 0;
}

/* Public surface used by the Live blade. */
static void relive_init(void) {
    GetModuleFileNameW(0,relive_res_dir,2048);
    WCHAR *s=wcsrchr(relive_res_dir,L'\\');if(s)*s=0;s=wcsrchr(relive_res_dir,L'\\');if(s)*s=0;
    lstrcatW(relive_res_dir,L"\\x360\\blades-converted\\");
    HANDLE t=CreateThread(0,0,relive_worker,0,0,0);if(t)CloseHandle(t);
}
static BOOL relive_banner_ready(void) {return relive_have_banner!=0;}
static const WCHAR *relive_banner_uri(void) {return relive_have_banner?relive_uri:0;}
static int relive_feat_count(void) {return (int)relive_feat_n;}
static const ReliveFeat *relive_feat_at(int i) {return (i>=0&&i<(int)relive_feat_n)?&relive_feat[i]:0;}
