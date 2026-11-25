//----------------------------------------------------------------------------
// File: devkitdash.cpp
//
// Desc: The Sion's devkit dashboard.  A plain screen in the manner of the
//       XDK's launcher: a background picture, the console's host name and
//       IP address drawn with XFONT, and a list of the titles on the hard
//       disk that A launches with a developer command line (launch data
//       LDT_FROM_DEBUGGER_CMDLINE, as a devkit's debugger launches them).
//
//       Laid out after Xbox Demos (KeybadeBlox's xdemos-decomp): one main
//       loop that reads the pads, redraws what changed and swaps.  The
//       whole screen is composed on the CPU into one 640x480 canvas (the
//       background, the panels and XFONT's text, antialiased into an A8
//       scratch buffer and blended), copied into a linear texture only when
//       something on it changed, and drawn as one quad every frame.
//
//       Files (D: is this XBE's folder, C:\devkit on the Sion):
//         D:\media\background.bmp   the background (24 or 32-bit BMP)
//         D:\media\font.ttf         text; C:\Fonts\tahoma.ttf, then XFONT's
//         D:\media\fontb.ttf        own Arial when missing
//         E:\DEVKIT\hostinfo.txt    name=/ip=/if= lines the host keeps
//                                   current (xbox-hostinfo on the Sion)
//         E:\DEVKIT\devkit.ini      cmdline=-dev, launcher=1
//         E:\DEVKIT\run-xboxdash    written to ask the host's session for
//                                   the stock Xbox Dashboard next
//
//       Titles listed: C:\xboxdash.xbe (the stock dashboard, run through
//       the session as usual), the disc in the drive, E:\Games\*\default.xbe,
//       E:\Apps\*\default.xbe and E:\DEVKIT\*\*.xbe.
//----------------------------------------------------------------------------
#include <xtl.h>
#define XFONT_TRUETYPE
#include <xfont.h>
#include <stdio.h>
#include <string.h>

//----------------------------------------------------------------------------
// Kernel imports the XDK headers keep private
//----------------------------------------------------------------------------
extern "C" {
typedef struct _DK_OBJECT_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR  Buffer;
} DK_OBJECT_STRING;

__declspec(dllimport) LONG __stdcall IoCreateSymbolicLink( DK_OBJECT_STRING* pLink, DK_OBJECT_STRING* pTarget );
__declspec(dllimport) LONG __stdcall IoDeleteSymbolicLink( DK_OBJECT_STRING* pLink );
__declspec(dllimport) VOID __stdcall HalReturnToFirmware( ULONG uRoutine );

DWORD WINAPI XWriteTitleInfoAndRebootA( LPCSTR pszLaunchPath, LPCSTR pszDDrivePath,
                                        DWORD dwLaunchDataType, DWORD dwTitleId,
                                        PLAUNCH_DATA pLaunchData );
}

#define DK_HAL_REBOOT  1      // HalRebootRoutine: no title, back to the dashboard
#define DK_LDT_NONE    0xFFFFFFFF   // LDT_NONE (xlaunch.h)
#define DK_TITLE_ID    0xFFFE0000   // a dashboard's, as imagebld stamps it

//----------------------------------------------------------------------------
// Screen layout (640x480, inside the title-safe area)
//----------------------------------------------------------------------------
#define SCREEN_W        640
#define SCREEN_H        480
#define SAFE_L          48
#define SAFE_T          36
#define SAFE_R          (SCREEN_W - 48)
#define SAFE_B          (SCREEN_H - 36)
#define LIST_X          SAFE_L
#define LIST_Y          182
#define LIST_W          360
#define ROW_H           30
#define LIST_ROWS       6

#define MAX_TITLES      64
#define MAX_IFACES      4
#define TEXT_MAX        96

#define COL_TEXT        0xFFFFFFFF
#define COL_DIM         0xFFB8B4D4
#define COL_ACCENT      0xFFE8366F   // crimson pink
#define COL_SHADOW      0xFF15131F
#define COL_PANEL       0xB01A1828
#define COL_HILITE      0xFFE8366F
#define COL_HILITE_TXT  0xFFFFFFFF

//----------------------------------------------------------------------------
// State
//----------------------------------------------------------------------------
enum TitleKind { KIND_XBOXDASH, KIND_DISC, KIND_TITLE };

struct TITLE
{
    TitleKind kind;
    WCHAR     szName[TEXT_MAX];     // the certificate's title name
    WCHAR     szWhere[TEXT_MAX];    // where it lives, for the second line
    CHAR      szDir[MAX_PATH];      // D: for the launch (a \Device path)
    CHAR      szXbe[64];            // the XBE, relative to szDir
    DWORD     dwTitleId;
};

static LPDIRECT3DDEVICE8  g_pDevice;
static LPDIRECT3DTEXTURE8 g_pCanvasTex;
static DWORD*             g_pCanvas;        // 640x480 X8R8G8B8, composed here
static DWORD*             g_pBackground;    // 640x480, the scaled picture
static BYTE*              g_pScratch;       // 640x480 A8, XFONT draws here
static XFONT*             g_pFont;
static XFONT*             g_pFontBold;

static TITLE              g_aTitles[MAX_TITLES];
static UINT               g_uTitles;
static UINT               g_uSel;
static UINT               g_uTop;
static BOOL               g_bShowList = TRUE;
static CHAR               g_szCmdLine[256] = "-dev";
static CHAR               g_szLast[MAX_PATH + 64];  // the title last launched: "<dir>|<xbe>"

static CHAR               g_szHostName[64];
static CHAR               g_szHostIp[64];
static CHAR               g_aszIfaces[MAX_IFACES][64];
static UINT               g_uIfaces;

static BOOL               g_bDirty = TRUE;
static WCHAR              g_szStatus[TEXT_MAX];
static DWORD              g_dwStatusUntil;
static BOOL               g_bLaunching;

//----------------------------------------------------------------------------
// Small helpers
//----------------------------------------------------------------------------
static VOID MakeLink( const CHAR* szLink, const CHAR* szTarget )
{
    DK_OBJECT_STRING link, target;
    link.Buffer = (PCHAR)szLink;
    link.Length = (USHORT)strlen( szLink );
    link.MaximumLength = link.Length + 1;
    target.Buffer = (PCHAR)szTarget;
    target.Length = (USHORT)strlen( szTarget );
    target.MaximumLength = target.Length + 1;
    IoDeleteSymbolicLink( &link );
    IoCreateSymbolicLink( &link, &target );
}

static VOID AtoW( WCHAR* dst, const CHAR* src, UINT cch )
{
    UINT i;
    for( i = 0; i + 1 < cch && src[i]; i++ )
        dst[i] = (WCHAR)(BYTE)src[i];
    dst[i] = 0;
}

static VOID SetStatus( const WCHAR* sz )
{
    wcsncpy( g_szStatus, sz, TEXT_MAX - 1 );
    g_szStatus[TEXT_MAX - 1] = 0;
    g_dwStatusUntil = GetTickCount() + 4000;
    g_bDirty = TRUE;
}

static BOOL FileExists( const CHAR* sz )
{
    DWORD a = GetFileAttributesA( sz );
    return a != 0xFFFFFFFF && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Reads a whole (small) file into buf, zero terminated.  Returns its length or -1.
static INT ReadSmallFile( const CHAR* sz, CHAR* buf, UINT cb )
{
    HANDLE h = CreateFileA( sz, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if( h == INVALID_HANDLE_VALUE )
        return -1;
    DWORD got = 0;
    BOOL ok = ReadFile( h, buf, cb - 1, &got, NULL );
    CloseHandle( h );
    if( !ok )
        return -1;
    buf[got] = 0;
    return (INT)got;
}

static BOOL WriteSmallFile( const CHAR* sz, const CHAR* data, UINT cb )
{
    HANDLE h = CreateFileA( sz, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if( h == INVALID_HANDLE_VALUE )
        return FALSE;
    DWORD put = 0;
    BOOL ok = WriteFile( h, data, cb, &put, NULL ) && put == cb;
    CloseHandle( h );
    return ok;
}

// Splits "key=value" lines; calls fn for each.
typedef VOID (*KEYFN)( const CHAR* key, const CHAR* value );
static VOID ParseLines( CHAR* buf, KEYFN fn )
{
    CHAR* line = buf;
    while( line && *line )
    {
        CHAR* next = strchr( line, '\n' );
        if( next )
            *next++ = 0;
        UINT n = strlen( line );
        while( n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t') )
            line[--n] = 0;
        while( *line == ' ' || *line == '\t' )
            line++;
        CHAR* eq = strchr( line, '=' );
        if( eq && *line != '#' && *line != ';' )
        {
            *eq = 0;
            fn( line, eq + 1 );
        }
        line = next;
    }
}

//----------------------------------------------------------------------------
// Settings: E:\DEVKIT\devkit.ini
//----------------------------------------------------------------------------
static VOID SettingKey( const CHAR* key, const CHAR* value )
{
    if( !_stricmp( key, "cmdline" ) )
    {
        strncpy( g_szCmdLine, value, sizeof(g_szCmdLine) - 1 );
        g_szCmdLine[sizeof(g_szCmdLine) - 1] = 0;
    }
    else if( !_stricmp( key, "launcher" ) )
        g_bShowList = atoi( value ) != 0;
    else if( !_stricmp( key, "last" ) )
    {
        strncpy( g_szLast, value, sizeof(g_szLast) - 1 );
        g_szLast[sizeof(g_szLast) - 1] = 0;
    }
}

static VOID LoadSettings()
{
    CHAR buf[1024];
    if( ReadSmallFile( "E:\\DEVKIT\\devkit.ini", buf, sizeof(buf) ) >= 0 )
        ParseLines( buf, SettingKey );
}

static VOID SaveSettings()
{
    CHAR buf[1024];
    INT n = _snprintf( buf, sizeof(buf),
                       "# Devkit dashboard settings\r\n"
                       "# cmdline: what A passes to a title (LDT_FROM_DEBUGGER_CMDLINE)\r\n"
                       "# launcher: 1 shows the application list, 0 hides it (Y)\r\n"
                       "cmdline=%s\r\nlauncher=%d\r\nlast=%s\r\n", g_szCmdLine, g_bShowList ? 1 : 0, g_szLast );
    CreateDirectoryA( "E:\\DEVKIT", NULL );
    WriteSmallFile( "E:\\DEVKIT\\devkit.ini", buf, n > 0 ? n : 0 );
}

//----------------------------------------------------------------------------
// Host name and addresses: E:\DEVKIT\hostinfo.txt, kept current by the host
//----------------------------------------------------------------------------
static CHAR  s_szNewName[64], s_szNewIp[64], s_aszNewIf[MAX_IFACES][64];
static UINT  s_uNewIf;

static VOID HostKey( const CHAR* key, const CHAR* value )
{
    if( !_stricmp( key, "name" ) )
        strncpy( s_szNewName, value, sizeof(s_szNewName) - 1 );
    else if( !_stricmp( key, "ip" ) )
        strncpy( s_szNewIp, value, sizeof(s_szNewIp) - 1 );
    else if( !_stricmp( key, "if" ) && s_uNewIf < MAX_IFACES )
        strncpy( s_aszNewIf[s_uNewIf++], value, 63 );
}

static VOID LoadHostInfo()
{
    CHAR buf[1024];
    memset( s_szNewName, 0, sizeof(s_szNewName) );
    memset( s_szNewIp, 0, sizeof(s_szNewIp) );
    memset( s_aszNewIf, 0, sizeof(s_aszNewIf) );
    s_uNewIf = 0;
    if( ReadSmallFile( "E:\\DEVKIT\\hostinfo.txt", buf, sizeof(buf) ) >= 0 )
        ParseLines( buf, HostKey );
    if( !s_szNewName[0] )
        strcpy( s_szNewName, "xbox" );
    if( !s_szNewIp[0] )
        strcpy( s_szNewIp, "not connected" );

    if( strcmp( s_szNewName, g_szHostName ) || strcmp( s_szNewIp, g_szHostIp ) ||
        s_uNewIf != g_uIfaces || memcmp( s_aszNewIf, g_aszIfaces, sizeof(g_aszIfaces) ) )
    {
        strcpy( g_szHostName, s_szNewName );
        strcpy( g_szHostIp, s_szNewIp );
        memcpy( g_aszIfaces, s_aszNewIf, sizeof(g_aszIfaces) );
        g_uIfaces = s_uNewIf;
        g_bDirty = TRUE;
    }
}

//----------------------------------------------------------------------------
// Titles
//----------------------------------------------------------------------------

// The title name and ID from an XBE's certificate.
static BOOL ReadXbeInfo( const CHAR* szPath, WCHAR* szName, UINT cchName, DWORD* pdwTitleId )
{
    HANDLE h = CreateFileA( szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL );
    if( h == INVALID_HANDLE_VALUE )
        return FALSE;
    BYTE hdr[0x180];
    DWORD got = 0;
    BOOL ok = ReadFile( h, hdr, sizeof(hdr), &got, NULL ) && got == sizeof(hdr) &&
              *(DWORD*)hdr == 0x48454258;   // "XBEH"
    if( ok )
    {
        DWORD dwBase = *(DWORD*)(hdr + 0x104);
        DWORD dwCert = *(DWORD*)(hdr + 0x118);
        BYTE cert[0xC + 80];
        ok = dwCert > dwBase &&
             SetFilePointer( h, dwCert - dwBase, NULL, FILE_BEGIN ) != 0xFFFFFFFF &&
             ReadFile( h, cert, sizeof(cert), &got, NULL ) && got == sizeof(cert);
        if( ok )
        {
            *pdwTitleId = *(DWORD*)(cert + 8);
            const WCHAR* w = (const WCHAR*)(cert + 0xC);
            UINT i;
            for( i = 0; i < 40 && i + 1 < cchName && w[i]; i++ )
                szName[i] = w[i];
            szName[i] = 0;
        }
    }
    CloseHandle( h );
    return ok;
}

// Adds a title; szDos is its path through our drive letters, szDir/szXbe
// what the launch is given.
static VOID AddTitle( TitleKind kind, const CHAR* szDos, const CHAR* szDir, const CHAR* szXbe,
                      const CHAR* szWhere, const CHAR* szFallbackName )
{
    if( g_uTitles >= MAX_TITLES )
        return;
    TITLE* t = &g_aTitles[g_uTitles];
    memset( t, 0, sizeof(*t) );
    if( !ReadXbeInfo( szDos, t->szName, TEXT_MAX, &t->dwTitleId ) )
        return;
    if( !t->szName[0] )
        AtoW( t->szName, szFallbackName, TEXT_MAX );
    t->kind = kind;
    strncpy( t->szDir, szDir, sizeof(t->szDir) - 1 );
    strncpy( t->szXbe, szXbe, sizeof(t->szXbe) - 1 );
    AtoW( t->szWhere, szWhere, TEXT_MAX );
    g_uTitles++;
}

// Orders g_aTitles[uFirst..] by name, then by where they are.
static VOID SortTitles( UINT uFirst )
{
    for( UINT i = uFirst + 1; i < g_uTitles; i++ )
    {
        TITLE t = g_aTitles[i];
        UINT j = i;
        while( j > uFirst )
        {
            TITLE* p = &g_aTitles[j - 1];
            INT c = _wcsicmp( p->szName, t.szName );
            if( c < 0 || (c == 0 && _wcsicmp( p->szWhere, t.szWhere ) <= 0) )
                break;
            g_aTitles[j] = *p;
            j--;
        }
        g_aTitles[j] = t;
    }
}

// Each <root>\<folder>\default.xbe (or every .xbe when bAnyXbe).
static VOID ScanFolders( const CHAR* szDosRoot, const CHAR* szDevRoot, const CHAR* szShown, BOOL bAnyXbe )
{
    CHAR szFind[MAX_PATH];
    WIN32_FIND_DATAA fd;
    _snprintf( szFind, sizeof(szFind), "%s\\*", szDosRoot );
    HANDLE h = FindFirstFileA( szFind, &fd );
    if( h == INVALID_HANDLE_VALUE )
        return;
    do
    {
        if( !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' )
            continue;
        CHAR szDir[MAX_PATH], szDos[MAX_PATH], szWhere[MAX_PATH];
        _snprintf( szDir, sizeof(szDir), "%s\\%s", szDevRoot, fd.cFileName );
        _snprintf( szWhere, sizeof(szWhere), "%s\\%s", szShown, fd.cFileName );
        if( !bAnyXbe )
        {
            _snprintf( szDos, sizeof(szDos), "%s\\%s\\default.xbe", szDosRoot, fd.cFileName );
            if( FileExists( szDos ) )
                AddTitle( KIND_TITLE, szDos, szDir, "default.xbe", szWhere, fd.cFileName );
            continue;
        }
        CHAR szFind2[MAX_PATH];
        WIN32_FIND_DATAA fd2;
        _snprintf( szFind2, sizeof(szFind2), "%s\\%s\\*.xbe", szDosRoot, fd.cFileName );
        HANDLE h2 = FindFirstFileA( szFind2, &fd2 );
        if( h2 == INVALID_HANDLE_VALUE )
            continue;
        do
        {
            if( fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
                continue;
            _snprintf( szDos, sizeof(szDos), "%s\\%s\\%s", szDosRoot, fd.cFileName, fd2.cFileName );
            AddTitle( KIND_TITLE, szDos, szDir, fd2.cFileName, szWhere, fd.cFileName );
        } while( FindNextFileA( h2, &fd2 ) );
        FindClose( h2 );
    } while( FindNextFileA( h, &fd ) );
    FindClose( h );
}

static VOID ScanTitles()
{
    TITLE aOld[MAX_TITLES];
    UINT uOld = g_uTitles;
    CHAR szSelDir[MAX_PATH], szSelXbe[64];
    memcpy( aOld, g_aTitles, sizeof(TITLE) * uOld );
    szSelDir[0] = szSelXbe[0] = 0;
    if( g_uSel < uOld )
    {
        strcpy( szSelDir, g_aTitles[g_uSel].szDir );
        strcpy( szSelXbe, g_aTitles[g_uSel].szXbe );
    }

    g_uTitles = 0;
    AddTitle( KIND_XBOXDASH, "C:\\xboxdash.xbe", "\\Device\\Harddisk0\\Partition2", "xboxdash.xbe",
              "C:\\xboxdash.xbe", "Xbox Dashboard" );
    if( g_uTitles )
        wcscpy( g_aTitles[0].szName, L"Xbox Dashboard" );
    AddTitle( KIND_DISC, "V:\\default.xbe", "\\Device\\CdRom0", "default.xbe", "Disc in the drive", "Disc" );
    UINT uFirst = g_uTitles;   // the stock dashboard and the disc stay on top
    ScanFolders( "E:\\Games", "\\Device\\Harddisk0\\Partition1\\Games", "E:\\Games", FALSE );
    ScanFolders( "E:\\Apps", "\\Device\\Harddisk0\\Partition1\\Apps", "E:\\Apps", FALSE );
    ScanFolders( "E:\\DEVKIT", "\\Device\\Harddisk0\\Partition1\\DEVKIT", "E:\\DEVKIT", TRUE );
    SortTitles( uFirst );

    if( g_uTitles != uOld || memcmp( aOld, g_aTitles, sizeof(TITLE) * uOld ) )
    {
        // Keep the same title selected when the list changes under it.
        UINT i;
        g_uSel = g_uSel < g_uTitles ? g_uSel : (g_uTitles ? g_uTitles - 1 : 0);
        for( i = 0; i < g_uTitles; i++ )
            if( !strcmp( g_aTitles[i].szDir, szSelDir ) && !strcmp( g_aTitles[i].szXbe, szSelXbe ) )
                g_uSel = i;
        g_bDirty = TRUE;
    }
}

static VOID Launch( BOOL bDev )
{
    if( g_uSel >= g_uTitles || g_bLaunching )
        return;
    TITLE* t = &g_aTitles[g_uSel];

    // Come back to this title when the dashboard starts again.
    _snprintf( g_szLast, sizeof(g_szLast), "%s|%s", t->szDir, t->szXbe );
    g_szLast[sizeof(g_szLast) - 1] = 0;
    SaveSettings();

    if( t->kind == KIND_XBOXDASH )
    {
        // The stock dashboard runs the way the session always runs it: ask
        // for it and leave.
        CreateDirectoryA( "E:\\DEVKIT", NULL );
        if( !WriteSmallFile( "E:\\DEVKIT\\run-xboxdash", "1\r\n", 3 ) )
        {
            SetStatus( L"Could not write E:\\DEVKIT\\run-xboxdash" );
            return;
        }
        g_bLaunching = TRUE;
        HalReturnToFirmware( DK_HAL_REBOOT );
        return;
    }

    // Re-check: the disc may have come out, the folder may be gone.
    CHAR szCheck[MAX_PATH];
    if( t->kind == KIND_DISC )
        strcpy( szCheck, "V:\\default.xbe" );
    else
    {
        // \Device\Harddisk0\Partition1\X -> E:\X
        _snprintf( szCheck, sizeof(szCheck), "E:%s\\%s", t->szDir + 28, t->szXbe );
    }
    if( !FileExists( szCheck ) )
    {
        SetStatus( L"That title is no longer there" );
        g_uTitles = 0;
        ScanTitles();
        return;
    }

    static LAUNCH_DATA ld;
    memset( &ld, 0, sizeof(ld) );
    DWORD dwType = DK_LDT_NONE;
    if( bDev )
    {
        // A devkit's debugger hands a title its command line this way;
        // the title reads it back with XGetLaunchInfo.
        strncpy( ((LD_FROM_DEBUGGER_CMDLINE*)&ld)->szCmdLine, g_szCmdLine, MAX_LAUNCH_DATA_SIZE - 1 );
        dwType = LDT_FROM_DEBUGGER_CMDLINE;
    }
    g_bLaunching = TRUE;
    DWORD dwErr = XWriteTitleInfoAndRebootA( t->szXbe, t->szDir, dwType, DK_TITLE_ID, &ld );
    // Only back here when the launch could not be written.
    g_bLaunching = FALSE;
    WCHAR sz[TEXT_MAX];
    swprintf( sz, L"Launch failed (error %u)", dwErr );
    SetStatus( sz );
}

//----------------------------------------------------------------------------
// Pictures and fonts
//----------------------------------------------------------------------------

// A 24 or 32-bit uncompressed BMP, scaled (nearest) to 640x480.
static BOOL LoadBackground( const CHAR* sz )
{
    HANDLE h = CreateFileA( sz, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL );
    if( h == INVALID_HANDLE_VALUE )
        return FALSE;
    DWORD cb = GetFileSize( h, NULL ), got = 0;
    BYTE* p = (cb > 54 && cb < 32 * 1024 * 1024) ? (BYTE*)LocalAlloc( LMEM_FIXED, cb ) : NULL;
    BOOL ok = p && ReadFile( h, p, cb, &got, NULL ) && got == cb;
    CloseHandle( h );
    if( ok )
    {
        DWORD off = *(DWORD*)(p + 10);
        LONG  w = *(LONG*)(p + 18), hgt = *(LONG*)(p + 22);
        WORD  bpp = *(WORD*)(p + 28);
        DWORD comp = *(DWORD*)(p + 30);
        BOOL  bTopDown = hgt < 0;
        if( bTopDown )
            hgt = -hgt;
        DWORD stride = ((w * (bpp / 8)) + 3) & ~3;
        ok = p[0] == 'B' && p[1] == 'M' && (bpp == 24 || bpp == 32) && (comp == 0 || comp == 3) &&
             w > 0 && hgt > 0 && off + stride * hgt <= cb;
        if( ok )
        {
            for( INT y = 0; y < SCREEN_H; y++ )
            {
                LONG sy = (LONG)y * hgt / SCREEN_H;
                const BYTE* row = p + off + stride * (bTopDown ? sy : hgt - 1 - sy);
                for( INT x = 0; x < SCREEN_W; x++ )
                {
                    const BYTE* px = row + ((LONG)x * w / SCREEN_W) * (bpp / 8);
                    g_pBackground[y * SCREEN_W + x] = 0xFF000000 | (px[2] << 16) | (px[1] << 8) | px[0];
                }
            }
        }
    }
    if( p )
        LocalFree( p );
    return ok;
}

static VOID DefaultBackground()
{
    for( INT y = 0; y < SCREEN_H; y++ )
    {
        DWORD c = (y & 2) ? 0xFF3C3A52 : 0xFF46445E;
        for( INT x = 0; x < SCREEN_W; x++ )
            g_pBackground[y * SCREEN_W + x] = c;
    }
}

static XFONT* OpenFont( const WCHAR* szPreferred, const WCHAR* szFallback )
{
    XFONT* f = NULL;
    if( FAILED( XFONT_OpenTrueTypeFont( szPreferred, 64 * 1024, &f ) ) &&
        FAILED( XFONT_OpenTrueTypeFont( szFallback, 64 * 1024, &f ) ) &&
        FAILED( XFONT_OpenDefaultFont( &f ) ) )
        return NULL;
    XFONT_SetTextAntialiasLevel( f, 4 );
    XFONT_SetTextColor( f, 0xFFFFFFFF );
    XFONT_SetBkMode( f, XFONT_TRANSPARENT );
    XFONT_SetTextAlignment( f, XFONT_LEFT | XFONT_TOP );
    return f;
}

//----------------------------------------------------------------------------
// The canvas
//----------------------------------------------------------------------------
static inline DWORD Blend( DWORD dst, DWORD src, UINT a )
{
    if( a >= 255 )
        return src | 0xFF000000;
    UINT ia = 255 - a;
    UINT r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
    UINT g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia) / 255;
    UINT b = ((src & 0xFF) * a + (dst & 0xFF) * ia) / 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

static VOID FillRect( INT x0, INT y0, INT x1, INT y1, DWORD argb )
{
    UINT a = argb >> 24;
    if( x0 < 0 ) x0 = 0;
    if( y0 < 0 ) y0 = 0;
    if( x1 > SCREEN_W ) x1 = SCREEN_W;
    if( y1 > SCREEN_H ) y1 = SCREEN_H;
    for( INT y = y0; y < y1; y++ )
        for( INT x = x0; x < x1; x++ )
            g_pCanvas[y * SCREEN_W + x] = Blend( g_pCanvas[y * SCREEN_W + x], argb & 0xFFFFFF, a );
}

// A disc centred on (cx, cy) in pixel-edge coordinates: a disc at (10, 10)
// with radius 10 covers pixels 0..19 exactly.  4x4 samples per pixel.
static VOID FillCircle( FLOAT cx, FLOAT cy, FLOAT r, DWORD argb )
{
    UINT a = argb >> 24;
    INT x0 = (INT)(cx - r) - 1, x1 = (INT)(cx + r) + 1;
    INT y0 = (INT)(cy - r) - 1, y1 = (INT)(cy + r) + 1;
    for( INT py = y0; py <= y1; py++ )
        for( INT px = x0; px <= x1; px++ )
        {
            if( px < 0 || py < 0 || px >= SCREEN_W || py >= SCREEN_H )
                continue;
            UINT cover = 0;
            for( INT sy = 0; sy < 4; sy++ )
                for( INT sx = 0; sx < 4; sx++ )
                {
                    FLOAT fx = px + (sx + 0.5f) / 4.0f - cx;
                    FLOAT fy = py + (sy + 0.5f) / 4.0f - cy;
                    if( fx * fx + fy * fy <= r * r )
                        cover++;
                }
            if( cover )
                g_pCanvas[py * SCREEN_W + px] = Blend( g_pCanvas[py * SCREEN_W + px], argb & 0xFFFFFF, a * cover / 16 );
        }
}

// Where XFONT puts a string's ink when drawn at (0, 0): the left, top,
// right and bottom of the pixels it covers (right and bottom exclusive).
static BOOL InkBox( XFONT* f, UINT uHeight, const WCHAR* sz, INT* pL, INT* pT, INT* pR, INT* pB )
{
    if( !f || !sz[0] )
        return FALSE;
    UINT w = 0;
    unsigned uCell = 0, uDescent = 0;
    XFONT_SetTextHeight( f, uHeight );
    XFONT_GetTextExtent( f, sz, (UINT)-1, &w );
    XFONT_GetFontMetrics( f, &uCell, &uDescent );
    INT W = (INT)w + 8, H = (INT)uCell + 8;
    if( W > SCREEN_W ) W = SCREEN_W;
    if( H > SCREEN_H ) H = SCREEN_H;
    for( INT r = 0; r < H; r++ )
        memset( g_pScratch + r * SCREEN_W, 0, W );
    XFONT_TextOutToMemory( f, g_pScratch, SCREEN_W, SCREEN_W, SCREEN_H, D3DFMT_LIN_A8, sz, (UINT)-1, 2, 2 );
    INT L = W, T = H, R = -1, B = -1;
    for( INT y = 0; y < H; y++ )
        for( INT x = 0; x < W; x++ )
            if( g_pScratch[y * SCREEN_W + x] > 24 )
            {
                if( x < L ) L = x;
                if( x > R ) R = x;
                if( y < T ) T = y;
                if( y > B ) B = y;
            }
    if( R < 0 )
        return FALSE;
    *pL = L - 2; *pT = T - 2; *pR = R + 1 - 2; *pB = B + 1 - 2;
    return TRUE;
}

static UINT TextWidth( XFONT* f, UINT uHeight, const WCHAR* sz )
{
    UINT w = 0;
    XFONT_SetTextHeight( f, uHeight );
    XFONT_GetTextExtent( f, sz, (UINT)-1, &w );
    return w;
}

// Draws text with its top left at (x, y), clipped to maxW pixels wide
// (an ellipsis when it does not fit); a 1-pixel shadow when shadow != 0.
static UINT DrawText( XFONT* f, UINT uHeight, INT x, INT y, const WCHAR* szIn, DWORD color,
                      DWORD shadow, UINT maxW )
{
    if( !f || !szIn[0] )
        return 0;
    WCHAR sz[TEXT_MAX + 4];
    wcsncpy( sz, szIn, TEXT_MAX );
    sz[TEXT_MAX] = 0;
    UINT w = TextWidth( f, uHeight, sz );
    if( maxW && w > maxW )
    {
        UINT n = wcslen( sz );
        while( n > 1 )
        {
            sz[--n] = 0;
            wcscpy( sz + n, L"..." );
            w = TextWidth( f, uHeight, sz );
            if( w <= maxW )
                break;
            sz[n] = 0;
        }
    }
    unsigned uCell = 0, uDescent = 0;
    XFONT_GetFontMetrics( f, &uCell, &uDescent );
    INT x0 = x - 2, y0 = y - 2, x1 = x + (INT)w + 4, y1 = y + (INT)uCell + 4;
    if( x0 < 0 ) x0 = 0;
    if( y0 < 0 ) y0 = 0;
    if( x1 > SCREEN_W ) x1 = SCREEN_W;
    if( y1 > SCREEN_H ) y1 = SCREEN_H;
    if( x0 >= x1 || y0 >= y1 )
        return w;
    for( INT r = y0; r < y1; r++ )
        memset( g_pScratch + r * SCREEN_W + x0, 0, x1 - x0 );
    XFONT_TextOutToMemory( f, g_pScratch, SCREEN_W, SCREEN_W, SCREEN_H, D3DFMT_LIN_A8, sz, (UINT)-1, x, y );
    for( INT pass = shadow ? 0 : 1; pass < 2; pass++ )
    {
        INT d = pass ? 0 : 1;
        DWORD c = pass ? color : shadow;
        for( INT r = y0; r < y1; r++ )
        {
            if( r + d >= SCREEN_H )
                continue;
            const BYTE* s = g_pScratch + r * SCREEN_W;
            DWORD* dst = g_pCanvas + (r + d) * SCREEN_W;
            for( INT cx = x0; cx < x1 && cx + d < SCREEN_W; cx++ )
                if( s[cx] )
                    dst[cx + d] = Blend( dst[cx + d], c & 0xFFFFFF, s[cx] * (c >> 24) / 255 );
        }
    }
    return w;
}

// A controller button: a coloured orb with its letter centred in it, and
// the label beside it with its capitals centred on the orb's middle.
#define ORB_R 10
static INT DrawButton( INT x, INT y, const WCHAR* szLetter, DWORD color, const WCHAR* szLabel )
{
    const FLOAT cx = (FLOAT)x + ORB_R, cy = (FLOAT)y + ORB_R;
    FillCircle( cx, cy, (FLOAT)ORB_R, color );
    INT L, T, R, B;
    if( InkBox( g_pFontBold, 14, szLetter, &L, &T, &R, &B ) )
    {
        // the ink's centre on the orb's: (L + R) / 2 at cx, rounded to the pixel
        INT lx = (INT)(cx - (L + R) / 2.0f + 0.5f);
        INT ly = (INT)(cy - (T + B) / 2.0f + 0.5f);
        DrawText( g_pFontBold, 14, lx, ly, szLetter, 0xFF101010, 0, 0 );
    }
    // The label: capitals ("H") centred on the orb's middle line.
    INT ty = y + 1;
    if( InkBox( g_pFont, 16, L"H", &L, &T, &R, &B ) )
        ty = (INT)(cy - (T + B) / 2.0f + 0.5f);
    UINT w = DrawText( g_pFont, 16, x + 2 * ORB_R + 6, ty, szLabel, COL_TEXT, COL_SHADOW, 0 );
    return x + 2 * ORB_R + 6 + (INT)w + 22;
}

static VOID Compose()
{
    memcpy( g_pCanvas, g_pBackground, SCREEN_W * SCREEN_H * 4 );
    WCHAR sz[TEXT_MAX];

    // Header: what this is, the clock on the right.
    DrawText( g_pFontBold, 16, SAFE_L, SAFE_T, L"CRIMSON DEVELOPMENT KIT", COL_ACCENT, COL_SHADOW, 0 );
    SYSTEMTIME st;
    GetLocalTime( &st );
    swprintf( sz, L"%04u-%02u-%02u  %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute );
    UINT w = TextWidth( g_pFont, 16, sz );
    DrawText( g_pFont, 16, SAFE_R - (INT)w, SAFE_T, sz, COL_DIM, COL_SHADOW, 0 );

    // The console's name and address.
    WCHAR szName[64], szIp[64];
    AtoW( szName, g_szHostName, 64 );
    AtoW( szIp, g_szHostIp, 64 );
    DrawText( g_pFontBold, 44, SAFE_L - 2, SAFE_T + 22, szName, COL_TEXT, COL_SHADOW, SAFE_R - SAFE_L );
    DrawText( g_pFont, 15, SAFE_L, SAFE_T + 82, L"IP ADDRESS", COL_DIM, COL_SHADOW, 0 );
    DrawText( g_pFontBold, 24, SAFE_L, SAFE_T + 100, szIp, COL_TEXT, COL_SHADOW, 0 );
    // Other interfaces, when there are more than one.
    if( g_uIfaces > 1 )
    {
        INT x = SAFE_L + 220;
        for( UINT i = 0; i < g_uIfaces && i < 3; i++ )
        {
            WCHAR szIf[64];
            AtoW( szIf, g_aszIfaces[i], 64 );
            DrawText( g_pFont, 15, x, SAFE_T + 84 + (INT)i * 17, szIf, COL_DIM, COL_SHADOW, SAFE_R - x );
        }
    }

    // The launcher.
    if( g_bShowList )
    {
        INT rows = g_uTitles < LIST_ROWS ? (INT)g_uTitles : LIST_ROWS;
        if( rows == 0 )
            rows = 1;
        INT panelB = LIST_Y + 28 + rows * ROW_H + 8;
        FillRect( LIST_X - 10, LIST_Y - 8, LIST_X + LIST_W + 10, panelB, COL_PANEL );
        swprintf( sz, L"APPLICATIONS  (%u)", g_uTitles );
        DrawText( g_pFont, 15, LIST_X, LIST_Y - 2, sz, COL_DIM, 0, 0 );
        if( g_uTitles == 0 )
            DrawText( g_pFont, 18, LIST_X, LIST_Y + 30, L"Nothing on the hard disk yet", COL_TEXT, 0, LIST_W );

        if( g_uSel < g_uTop )
            g_uTop = g_uSel;
        if( g_uSel >= g_uTop + LIST_ROWS )
            g_uTop = g_uSel - LIST_ROWS + 1;
        for( INT i = 0; i < rows && g_uTop + i < g_uTitles; i++ )
        {
            UINT idx = g_uTop + i;
            TITLE* t = &g_aTitles[idx];
            INT y = LIST_Y + 24 + i * ROW_H;
            BOOL sel = idx == g_uSel;
            if( sel )
                FillRect( LIST_X - 6, y, LIST_X + LIST_W + 6, y + ROW_H - 2, COL_HILITE );
            DWORD c = sel ? COL_HILITE_TXT : COL_TEXT;
            UINT nw = DrawText( g_pFontBold, 18, LIST_X + 2, y + 5, t->szName, c, 0, 210 );
            (void)nw;
            const WCHAR* szWhere = t->szWhere;
            UINT ww = TextWidth( g_pFont, 14, szWhere );
            UINT maxw = 130;
            INT wx = LIST_X + LIST_W - (INT)(ww < maxw ? ww : maxw);
            DrawText( g_pFont, 14, wx, y + 8, szWhere, sel ? 0xFFFFD6E2 : COL_DIM, 0, maxw );
        }
        // Scroll marks
        if( g_uTop > 0 )
            DrawText( g_pFont, 14, LIST_X + LIST_W - 8, LIST_Y - 2, L"\x25B2", COL_DIM, 0, 0 );
        if( g_uTop + LIST_ROWS < g_uTitles )
            DrawText( g_pFont, 14, LIST_X + LIST_W - 8, panelB - 18, L"\x25BC", COL_DIM, 0, 0 );
    }

    // Status line, then the legend.
    if( g_szStatus[0] && GetTickCount() < g_dwStatusUntil )
        DrawText( g_pFont, 16, SAFE_L, SAFE_B - 44, g_szStatus, 0xFFFFD27A, COL_SHADOW, SAFE_R - SAFE_L );
    else
        g_szStatus[0] = 0;

    INT x = SAFE_L, y = SAFE_B - 22;
    if( g_bShowList && g_uTitles && g_aTitles[g_uSel].kind == KIND_XBOXDASH )
        x = DrawButton( x, y, L"A", 0xFF6CC24A, L"Open" );
    else if( g_bShowList && g_uTitles )
    {
        WCHAR szDev[64];
        WCHAR szCmd[40];
        AtoW( szCmd, g_szCmdLine, 40 );
        swprintf( szDev, L"Launch %s", szCmd[0] ? szCmd : L"(dev)" );
        x = DrawButton( x, y, L"A", 0xFF6CC24A, szDev );
        x = DrawButton( x, y, L"X", 0xFF3A8EE6, L"Launch" );
    }
    DrawButton( x, y, L"Y", 0xFFF2C230, g_bShowList ? L"Hide list" : L"Applications" );
}

static VOID Upload()
{
    D3DLOCKED_RECT lr;
    if( FAILED( g_pCanvasTex->LockRect( 0, &lr, NULL, 0 ) ) )
        return;
    for( INT y = 0; y < SCREEN_H; y++ )
        memcpy( (BYTE*)lr.pBits + y * lr.Pitch, g_pCanvas + y * SCREEN_W, SCREEN_W * 4 );
    g_pCanvasTex->UnlockRect( 0 );
}

//----------------------------------------------------------------------------
// Direct3D
//----------------------------------------------------------------------------
struct QUADVERTEX { FLOAT x, y, z, rhw, u, v; };
#define FVF_QUAD (D3DFVF_XYZRHW | D3DFVF_TEX1)

static HRESULT CreateD3D()
{
    LPDIRECT3D8 pD3D = Direct3DCreate8( D3D_SDK_VERSION );
    if( !pD3D )
        return E_FAIL;
    D3DPRESENT_PARAMETERS pp;
    ZeroMemory( &pp, sizeof(pp) );
    pp.BackBufferWidth        = SCREEN_W;
    pp.BackBufferHeight       = SCREEN_H;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if( FAILED( pD3D->CreateDevice( 0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                    &pp, &g_pDevice ) ) )
        return E_FAIL;
    if( FAILED( g_pDevice->CreateTexture( SCREEN_W, SCREEN_H, 1, 0, D3DFMT_LIN_X8R8G8B8, 0, &g_pCanvasTex ) ) )
        return E_FAIL;

    g_pDevice->SetRenderState( D3DRS_LIGHTING, FALSE );
    g_pDevice->SetRenderState( D3DRS_ZENABLE, FALSE );
    g_pDevice->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    g_pDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
    g_pDevice->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 );
    g_pDevice->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
    g_pDevice->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE );
    g_pDevice->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
    g_pDevice->SetTextureStageState( 0, D3DTSS_MINFILTER, D3DTEXF_POINT );
    g_pDevice->SetTextureStageState( 0, D3DTSS_MAGFILTER, D3DTEXF_POINT );
    g_pDevice->SetTextureStageState( 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP );
    g_pDevice->SetTextureStageState( 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP );
    return S_OK;
}

static VOID Render()
{
    // A linear texture takes texel coordinates.
    static const QUADVERTEX v[4] = {
        { -0.5f,            -0.5f,            0.0f, 1.0f, 0.0f,            0.0f            },
        { SCREEN_W - 0.5f,  -0.5f,            0.0f, 1.0f, (FLOAT)SCREEN_W, 0.0f            },
        { -0.5f,            SCREEN_H - 0.5f,  0.0f, 1.0f, 0.0f,            (FLOAT)SCREEN_H },
        { SCREEN_W - 0.5f,  SCREEN_H - 0.5f,  0.0f, 1.0f, (FLOAT)SCREEN_W, (FLOAT)SCREEN_H },
    };
    g_pDevice->Clear( 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0 );
    g_pDevice->BeginScene();
    g_pDevice->SetTexture( 0, g_pCanvasTex );
    g_pDevice->SetVertexShader( FVF_QUAD );
    g_pDevice->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, v, sizeof(QUADVERTEX) );
    g_pDevice->SetTexture( 0, NULL );
    g_pDevice->EndScene();
    g_pDevice->Present( NULL, NULL, NULL, NULL );
}

//----------------------------------------------------------------------------
// Controllers
//----------------------------------------------------------------------------
static HANDLE g_ahPads[4];
static WORD   g_awPrevButtons[4];
static BYTE   g_abPrevAnalog[4][8];

enum { IN_UP = 1, IN_DOWN = 2, IN_A = 4, IN_X = 8, IN_Y = 16, IN_PAGEUP = 32, IN_PAGEDOWN = 64 };

static VOID OpenPads()
{
    XDEVICE_PREALLOC_TYPE prealloc[1] = { { XDEVICE_TYPE_GAMEPAD, 4 } };
    XInitDevices( 1, prealloc );
    DWORD dw = XGetDevices( XDEVICE_TYPE_GAMEPAD );
    for( UINT i = 0; i < 4; i++ )
        if( dw & (1 << i) )
            g_ahPads[i] = XInputOpen( XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL );
}

// Presses since the last call, with held up/down repeating.
static DWORD ReadPads()
{
    static DWORD s_dwHeldSince, s_dwLastRepeat;
    DWORD dwIns, dwRem;
    if( XGetDeviceChanges( XDEVICE_TYPE_GAMEPAD, &dwIns, &dwRem ) )
    {
        for( UINT i = 0; i < 4; i++ )
        {
            if( (dwRem & (1 << i)) && g_ahPads[i] )
            {
                XInputClose( g_ahPads[i] );
                g_ahPads[i] = NULL;
            }
            if( dwIns & (1 << i) )
                g_ahPads[i] = XInputOpen( XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL );
        }
    }

    DWORD dwPressed = 0, dwHeld = 0;
    for( UINT i = 0; i < 4; i++ )
    {
        XINPUT_STATE s;
        if( !g_ahPads[i] || XInputGetState( g_ahPads[i], &s ) != ERROR_SUCCESS )
            continue;
        WORD b = s.Gamepad.wButtons;
        WORD nb = b & ~g_awPrevButtons[i];
        g_awPrevButtons[i] = b;
        BOOL a  = s.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A] > 30;
        BOOL x  = s.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_X] > 30;
        BOOL y  = s.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_Y] > 30;
        BOOL lt = s.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] > 60;
        BOOL rt = s.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] > 60;
        BYTE* prev = g_abPrevAnalog[i];
        if( a && !prev[0] )  dwPressed |= IN_A;
        if( x && !prev[1] )  dwPressed |= IN_X;
        if( y && !prev[2] )  dwPressed |= IN_Y;
        if( lt && !prev[3] ) dwPressed |= IN_PAGEUP;
        if( rt && !prev[4] ) dwPressed |= IN_PAGEDOWN;
        prev[0] = (BYTE)a; prev[1] = (BYTE)x; prev[2] = (BYTE)y; prev[3] = (BYTE)lt; prev[4] = (BYTE)rt;
        if( nb & XINPUT_GAMEPAD_START )
            dwPressed |= IN_A;

        BOOL up   = (b & XINPUT_GAMEPAD_DPAD_UP) || s.Gamepad.sThumbLY > 16000;
        BOOL down = (b & XINPUT_GAMEPAD_DPAD_DOWN) || s.Gamepad.sThumbLY < -16000;
        if( up )   dwHeld |= IN_UP;
        if( down ) dwHeld |= IN_DOWN;
    }

    static DWORD s_dwPrevHeld;
    DWORD now = GetTickCount();
    DWORD dwNewHeld = dwHeld & ~s_dwPrevHeld;
    if( dwNewHeld )
    {
        dwPressed |= dwNewHeld;
        s_dwHeldSince = s_dwLastRepeat = now;
    }
    else if( dwHeld && now - s_dwHeldSince > 400 && now - s_dwLastRepeat > 90 )
    {
        dwPressed |= dwHeld;
        s_dwLastRepeat = now;
    }
    s_dwPrevHeld = dwHeld;
    return dwPressed;
}

//----------------------------------------------------------------------------
// main
//----------------------------------------------------------------------------
VOID __cdecl main()
{
    // The drives this dashboard reads, as the XDK launcher maps them.
    MakeLink( "\\??\\C:", "\\Device\\Harddisk0\\Partition2" );
    MakeLink( "\\??\\E:", "\\Device\\Harddisk0\\Partition1" );
    MakeLink( "\\??\\V:", "\\Device\\CdRom0" );

    g_pCanvas     = (DWORD*)LocalAlloc( LMEM_FIXED, SCREEN_W * SCREEN_H * 4 );
    g_pBackground = (DWORD*)LocalAlloc( LMEM_FIXED, SCREEN_W * SCREEN_H * 4 );
    g_pScratch    = (BYTE*)LocalAlloc( LMEM_FIXED, SCREEN_W * SCREEN_H );
    if( !g_pCanvas || !g_pBackground || !g_pScratch || FAILED( CreateD3D() ) )
    {
        HalReturnToFirmware( DK_HAL_REBOOT );
        return;
    }

    if( !LoadBackground( "D:\\media\\background.bmp" ) )
        DefaultBackground();
    g_pFont     = OpenFont( L"D:\\media\\font.ttf",  L"C:\\Fonts\\tahoma.ttf" );
    g_pFontBold = OpenFont( L"D:\\media\\fontb.ttf", L"D:\\media\\font.ttf" );
    if( !g_pFontBold )
        g_pFontBold = g_pFont;

    // A leftover request from a run that did not get to the stock dashboard.
    DeleteFileA( "E:\\DEVKIT\\run-xboxdash" );
    CreateDirectoryA( "E:\\DEVKIT", NULL );
    LoadSettings();
    LoadHostInfo();
    ScanTitles();
    for( UINT i = 0; i < g_uTitles; i++ )
    {
        CHAR sz[MAX_PATH + 64];
        _snprintf( sz, sizeof(sz), "%s|%s", g_aTitles[i].szDir, g_aTitles[i].szXbe );
        sz[sizeof(sz) - 1] = 0;
        if( !_stricmp( sz, g_szLast ) )
            g_uSel = i;
    }
    OpenPads();

    DWORD dwNextPoll = GetTickCount() + 2000;
    WORD  wLastMinute = 0xFFFF;
    for( ;; )
    {
        DWORD in = ReadPads();
        if( g_uTitles && g_bShowList )
        {
            if( (in & IN_UP) && g_uSel > 0 )
                { g_uSel--; g_bDirty = TRUE; }
            if( (in & IN_DOWN) && g_uSel + 1 < g_uTitles )
                { g_uSel++; g_bDirty = TRUE; }
            if( in & IN_PAGEUP )
                { g_uSel = g_uSel > LIST_ROWS ? g_uSel - LIST_ROWS : 0; g_bDirty = TRUE; }
            if( in & IN_PAGEDOWN )
                { g_uSel = g_uSel + LIST_ROWS < g_uTitles ? g_uSel + LIST_ROWS : g_uTitles - 1; g_bDirty = TRUE; }
            if( in & IN_A )
                Launch( TRUE );
            else if( in & IN_X )
                Launch( FALSE );
        }
        if( in & IN_Y )
        {
            g_bShowList = !g_bShowList;
            g_bDirty = TRUE;
            SaveSettings();
        }

        DWORD now = GetTickCount();
        if( (LONG)(now - dwNextPoll) >= 0 )
        {
            LoadHostInfo();
            ScanTitles();
            dwNextPoll = now + 2000;
        }
        SYSTEMTIME st;
        GetLocalTime( &st );
        if( st.wMinute != wLastMinute )
        {
            wLastMinute = st.wMinute;
            g_bDirty = TRUE;
        }
        if( g_szStatus[0] && now >= g_dwStatusUntil )
            g_bDirty = TRUE;

        if( g_bDirty )
        {
            g_bDirty = FALSE;
            Compose();
            Upload();
        }
        Render();
    }
}
