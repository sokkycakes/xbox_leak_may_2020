// GameCollection: the dashboard's Games area.  The list starts with the
// media slots: the game disc in the tray and the carts on the game card (a
// Kazeta-style SD card or USB drive: a .kzi info file per cart at the top of
// the card, which xbcompat mounts as CARD0:), or one empty slot when there
// is neither.  After them come the titles installed on the hard disk as
// E:\Games\<folder>\default.xbe, named from each XBE's certificate.  A game
// is launched by pointing D: at its directory.
// It also supplies the details screen: the XBE's title image, its
// certificate details and the title's saved games, and installs a disc or
// cart to E:\Games on a background thread (OnInstallProgress,
// OnInstallComplete and OnInstallError are called on the node while it
// runs; OnMediaChanged when a card comes or goes).  Not part of Microsoft's
// dashboard; added by xbcompat's dashbuild.

#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"

#define MAX_GAMES 256

enum { KIND_EMPTY, KIND_DISC, KIND_CARD, KIND_HDD };

struct GAMEINFO
{
    int m_nKind;
    bool m_bXbox;               // has an XBE the dashboard can launch
    CHAR m_szDir[160];          // its directory: CDROM0:, CARD0:[\dir] or E:\Games\<folder>
    CHAR m_szXbe[64];           // the XBE in m_szDir
    CHAR m_szFolder[64];        // E:\Games\<m_szFolder> (installed games)
    CHAR m_szCart[64];          // the cart's .kzi or .kzp file on the card
    CHAR m_szRuntime[32];       // the cart's Kazeta runtime
    CHAR m_szId[64];            // the cart's Kazeta save Id
    CHAR m_szPlatform[32];      // the cart's Platform key, when its .kzi names one
    CHAR m_szPublisher[48];     // the cart's Publisher (or Developer) key, when its .kzi names one
    WCHAR m_szName[41];
    DWORD m_dwTitleID;
    DWORD m_dwTimeDate;         // XBE build time (seconds since 1970)
    DWORD m_dwRegion;
    DWORD m_dwDiscNumber;
    DWORD m_dwVersion;
    DWORD m_dwImageOffset;      // $$XTIMAGE section in the file, if any
    DWORD m_dwImageSize;
};

extern const TCHAR* g_szSelTitleImage;  // MaxMat.cpp: the "SelectedIcon" material's image
extern void TitleArray_GamesChanged();  // TitleCollection.cpp: the hard disk's titles are out of date

static GAMEINFO* c_rgGames = NULL;
static int c_nGameCount = 0;
static int c_nMediaCount = 0;   // the disc and card slots at the top
static CHAR c_szCardSig [512];  // the card's carts at the last Scan

static const OBJECT_STRING c_eDrive = CONSTANT_OBJECT_STRING("\\??\\E:");
static const OBJECT_STRING c_ePath  = CONSTANT_OBJECT_STRING("\\Device\\Harddisk0\\Partition1");

class CGameCollection : public CNode
{
    DECLARE_NODE(CGameCollection, CNode)
public:
    CGameCollection();
    ~CGameCollection();

    int Scan();
    int GetGameCount();
    int GetMediaCount();
    int GetGameKind(int nGame);
    int IsPlayable(int nGame);
    CStrObject* GetNotPlayableReason(int nGame);
    CStrObject* GetGameName(int nGame);
    CStrObject* GetGameFolder(int nGame);
    CStrObject* GetGameTitleID(int nGame);
    CStrObject* GetGamePlatform(int nGame);
    CStrObject* GetGamePublisher(int nGame);
    int GetRecentCount();
    int GetRecentGame(int n);
    CStrObject* GetGameRuntime(int nGame);
    CStrObject* GetGameInfo(int nGame);
    CStrObject* GetSavedGames(int nGame);
    int GetSavedGameCount(int nGame);
    int SelectGameImage(int nGame);
    CStrObject* GetGameImage(int nGame);
    void LaunchGame(int nGame);
    void LaunchXOnlineDash();

    int IsInstalled(int nGame);
    int StartInstall(int nGame);
    CStrObject* GetInstallStatus();
    CStrObject* GetInstallError();
    CStrObject* GetInstallFolder();
    float m_installProgress;

    void Advance(float nSeconds);

    DECLARE_NODE_PROPS()
    DECLARE_NODE_FUNCTIONS()
};

IMPLEMENT_NODE("GameCollection", CGameCollection, CNode)

START_NODE_PROPS(CGameCollection, CNode)
    NODE_PROP(pt_number, CGameCollection, installProgress)
END_NODE_PROPS()

START_NODE_FUN(CGameCollection, CNode)
    NODE_FUN_IV(Scan)
    NODE_FUN_IV(GetGameCount)
    NODE_FUN_IV(GetMediaCount)
    NODE_FUN_II(GetGameKind)
    NODE_FUN_II(IsPlayable)
    NODE_FUN_SI(GetNotPlayableReason)
    NODE_FUN_SI(GetGameName)
    NODE_FUN_SI(GetGameFolder)
    NODE_FUN_SI(GetGameTitleID)
    NODE_FUN_SI(GetGamePlatform)
    NODE_FUN_SI(GetGamePublisher)
    NODE_FUN_IV(GetRecentCount)
    NODE_FUN_II(GetRecentGame)
    NODE_FUN_SI(GetGameRuntime)
    NODE_FUN_SI(GetGameInfo)
    NODE_FUN_SI(GetSavedGames)
    NODE_FUN_II(GetSavedGameCount)
    NODE_FUN_II(SelectGameImage)
    NODE_FUN_SI(GetGameImage)
    NODE_FUN_VI(LaunchGame)
    NODE_FUN_VV(LaunchXOnlineDash)
    NODE_FUN_II(IsInstalled)
    NODE_FUN_II(StartInstall)
    NODE_FUN_SV(GetInstallStatus)
    NODE_FUN_SV(GetInstallError)
    NODE_FUN_SV(GetInstallFolder)
END_NODE_FUN()

CGameCollection::CGameCollection() :
    m_installProgress(0.0f)
{
    // The retail dashboard does not map E: (the data partition); games live there.
    IoCreateSymbolicLink((POBJECT_STRING)&c_eDrive, (POBJECT_STRING)&c_ePath);
}

CGameCollection::~CGameCollection()
{
}

// The certificate details and title image location of an XBE; false if it
// is not one.
static bool ReadXbeTitle(const CHAR* szPath, GAMEINFO* pGame)
{
    HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    bool bOK = false;
    DWORD rgdw[0x80];
    DWORD cb = 0;
    if (ReadFile(hFile, rgdw, sizeof (rgdw), &cb, NULL) && cb >= 0x120 && rgdw[0] == 'HEBX')
    {
        DWORD dwBase = rgdw[0x104 / 4];
        DWORD dwCert = rgdw[0x118 / 4] - dwBase;
        BYTE rgbCert[0xB0];
        pGame->m_dwTimeDate = rgdw[0x114 / 4];
        if (SetFilePointer(hFile, dwCert, NULL, FILE_BEGIN) == dwCert &&
            ReadFile(hFile, rgbCert, sizeof (rgbCert), &cb, NULL) && cb == sizeof (rgbCert))
        {
            pGame->m_dwTitleID = *(DWORD*)(rgbCert + 0x08);
            CopyMemory(pGame->m_szName, rgbCert + 0x0C, 40 * sizeof (WCHAR));
            pGame->m_szName[40] = 0;
            pGame->m_dwRegion = *(DWORD*)(rgbCert + 0xA0);
            pGame->m_dwDiscNumber = *(DWORD*)(rgbCert + 0xA8);
            pGame->m_dwVersion = *(DWORD*)(rgbCert + 0xAC);
            bOK = true;
        }

        // BLOCK: find the $$XTIMAGE section (56-byte headers, names by address)
        pGame->m_dwImageOffset = pGame->m_dwImageSize = 0;
        DWORD nSections = rgdw[0x11C / 4];
        DWORD dwHeaders = rgdw[0x120 / 4] - dwBase;
        for (DWORD i = 0; bOK && i < nSections && i < 64; i += 1)
        {
            DWORD rgdwSec[14];
            CHAR szName[16];
            DWORD dwPos = dwHeaders + i * sizeof (rgdwSec);
            if (SetFilePointer(hFile, dwPos, NULL, FILE_BEGIN) != dwPos ||
                !ReadFile(hFile, rgdwSec, sizeof (rgdwSec), &cb, NULL) || cb != sizeof (rgdwSec))
                break;
            DWORD dwName = rgdwSec[5] - dwBase;
            if (SetFilePointer(hFile, dwName, NULL, FILE_BEGIN) != dwName ||
                !ReadFile(hFile, szName, sizeof (szName), &cb, NULL) || cb != sizeof (szName))
                break;
            if (memcmp(szName, "$$XTIMAGE", 10) == 0)
            {
                pGame->m_dwImageOffset = rgdwSec[3];
                pGame->m_dwImageSize = rgdwSec[4];
                break;
            }
        }
    }

    CloseHandle(hFile);
    return bOK;
}

static int __cdecl CompareGames(const void* p1, const void* p2)
{
    return lstrcmpiW(((const GAMEINFO*)p1)->m_szName, ((const GAMEINFO*)p2)->m_szName);
}

static bool EndsWith(const CHAR* sz, const CHAR* szEnd)
{
    int cch = strlen(sz), cchEnd = strlen(szEnd);
    return cch >= cchEnd && _stricmp(sz + cch - cchEnd, szEnd) == 0;
}

// The DVD tray's state and the carts' file names on the game card, to
// notice either changing (a disc going in or out of a USB DVD drive).
static void GetCardSignature(CHAR* szSig, int cchSig)
{
    ULONG TrayState = 0;
    HalReadSMCTrayState(&TrayState, NULL);
    _snprintf(szSig, cchSig, "%02x|", TrayState & 0x70);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("CARD0:\\*", &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;
    int cch = strlen(szSig);
    do
    {
        int cchName = strlen(fd.cFileName);
        if ((EndsWith(fd.cFileName, ".kzi") || EndsWith(fd.cFileName, ".kzp")) && cch + cchName + 2 < cchSig)
        {
            strcpy(szSig + cch, fd.cFileName);
            cch += cchName;
            szSig[cch++] = '|';
            szSig[cch] = 0;
        }
    }
    while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

// A Kazeta cart info file: Name, Id, Exec, Runtime (key=value lines, UTF-8).
static void ReadCart(const CHAR* szKzi, GAMEINFO* pGame)
{
    CHAR szPath [MAX_PATH];
    sprintf(szPath, "CARD0:\\%s", szKzi);
    CHAR rgch [2048];
    DWORD cb = 0;
    HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    if (!ReadFile(hFile, rgch, sizeof (rgch) - 1, &cb, NULL))
        cb = 0;
    CloseHandle(hFile);
    rgch[cb] = 0;

    CHAR szExec [128];
    szExec[0] = 0;
    for (CHAR* pchLine = rgch; *pchLine != 0; )
    {
        CHAR* pchEnd = pchLine;
        while (*pchEnd != 0 && *pchEnd != '\n' && *pchEnd != '\r')
            pchEnd += 1;
        CHAR chEnd = *pchEnd;
        *pchEnd = 0;

        CHAR* pchValue = strchr(pchLine, '=');
        if (pchValue != NULL)
        {
            *pchValue++ = 0;
            // values may be quoted
            int cch = strlen(pchValue);
            if (cch >= 2 && pchValue[0] == '"' && pchValue[cch - 1] == '"')
                pchValue[cch - 1] = 0, pchValue += 1;

            if (_stricmp(pchLine, "Name") == 0)
                MultiByteToWideChar(CP_UTF8, 0, pchValue, -1, pGame->m_szName, 40);
            else if (_stricmp(pchLine, "Id") == 0)
                lstrcpynA(pGame->m_szId, pchValue, sizeof (pGame->m_szId));
            else if (_stricmp(pchLine, "Runtime") == 0)
                lstrcpynA(pGame->m_szRuntime, pchValue, sizeof (pGame->m_szRuntime));
            else if (_stricmp(pchLine, "Platform") == 0)
                lstrcpynA(pGame->m_szPlatform, pchValue, sizeof (pGame->m_szPlatform));
            else if (_stricmp(pchLine, "Publisher") == 0)
                lstrcpynA(pGame->m_szPublisher, pchValue, sizeof (pGame->m_szPublisher));
            else if (_stricmp(pchLine, "Developer") == 0 && pGame->m_szPublisher[0] == 0)
                lstrcpynA(pGame->m_szPublisher, pchValue, sizeof (pGame->m_szPublisher));
            else if (_stricmp(pchLine, "Exec") == 0)
                lstrcpynA(szExec, pchValue, sizeof (szExec));
        }

        pchLine = pchEnd;
        if (chEnd != 0)
            pchLine += 1;
    }

    // An Xbox cart (runtime "xbox", or an XBE to run): D: is the XBE's directory.
    if (_stricmp(pGame->m_szRuntime, "xbox") == 0 || EndsWith(szExec, ".xbe"))
    {
        for (CHAR* pch = szExec; *pch != 0; pch += 1)
        {
            if (*pch == '/')
                *pch = '\\';
        }
        CHAR* pchSlash = strrchr(szExec, '\\');
        if (pchSlash != NULL)
        {
            *pchSlash = 0;
            sprintf(pGame->m_szDir, "CARD0:\\%s", szExec);
            lstrcpynA(pGame->m_szXbe, pchSlash + 1, sizeof (pGame->m_szXbe));
        }
        else
        {
            strcpy(pGame->m_szDir, "CARD0:");
            lstrcpynA(pGame->m_szXbe, szExec[0] != 0 ? szExec : "default.xbe", sizeof (pGame->m_szXbe));
        }

        WCHAR szName [41];
        lstrcpyW(szName, pGame->m_szName);
        sprintf(szPath, "%s\\%s", pGame->m_szDir, pGame->m_szXbe);
        pGame->m_bXbox = ReadXbeTitle(szPath, pGame);
        if (szName[0] != 0)
            lstrcpyW(pGame->m_szName, szName);   // the cart's own name wins
    }
}

int CGameCollection::Scan()
{
    if (c_rgGames == NULL)
        c_rgGames = new GAMEINFO [MAX_GAMES];
    c_nGameCount = 0;

    // The game disc in the tray.
    GAMEINFO* pGame = &c_rgGames[0];
    ZeroMemory(pGame, sizeof (GAMEINFO));
    if (ReadXbeTitle("CDROM0:\\default.xbe", pGame))
    {
        pGame->m_nKind = KIND_DISC;
        pGame->m_bXbox = true;
        strcpy(pGame->m_szDir, "CDROM0:");
        strcpy(pGame->m_szXbe, "default.xbe");
        if (pGame->m_szName[0] == 0)
            lstrcpyW(pGame->m_szName, L"Game Disc");
        c_nGameCount += 1;
    }

    // The carts on the game card, by name.
    int nFirstCart = c_nGameCount;
    GetCardSignature(c_szCardSig, sizeof (c_szCardSig));
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("CARD0:\\*", &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            bool bKzi = EndsWith(fd.cFileName, ".kzi");
            if ((!bKzi && !EndsWith(fd.cFileName, ".kzp")) || strlen(fd.cFileName) >= sizeof (pGame->m_szCart))
                continue;

            pGame = &c_rgGames[c_nGameCount];
            ZeroMemory(pGame, sizeof (GAMEINFO));
            pGame->m_nKind = KIND_CARD;
            strcpy(pGame->m_szCart, fd.cFileName);
            if (bKzi)
                ReadCart(fd.cFileName, pGame);
            else
                strcpy(pGame->m_szRuntime, "packaged cart (.kzp)");
            if (pGame->m_szName[0] == 0)
            {
                int i = 0;
                for (; i < 40 && fd.cFileName[i] != 0 && fd.cFileName[i] != '.'; i += 1)
                    pGame->m_szName[i] = fd.cFileName[i];
                pGame->m_szName[i] = 0;
            }
            c_nGameCount += 1;
        }
        while (c_nGameCount < 16 && FindNextFileA(hFind, &fd));
        FindClose(hFind);
        qsort(c_rgGames + nFirstCart, c_nGameCount - nFirstCart, sizeof (GAMEINFO), CompareGames);
    }

    // Neither: one empty slot.
    if (c_nGameCount == 0)
    {
        ZeroMemory(&c_rgGames[0], sizeof (GAMEINFO));
        c_nGameCount = 1;
    }
    c_nMediaCount = c_nGameCount;

    // The games on the hard disk: default.xbe, or else the folder's one XBE.
    hFind = FindFirstFileA("E:\\Games\\*", &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return c_nGameCount;

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' ||
            strlen(fd.cFileName) >= sizeof (pGame->m_szFolder))
            continue;

        pGame = &c_rgGames[c_nGameCount];
        ZeroMemory(pGame, sizeof (GAMEINFO));
        pGame->m_nKind = KIND_HDD;
        pGame->m_bXbox = true;
        strcpy(pGame->m_szFolder, fd.cFileName);
        sprintf(pGame->m_szDir, "E:\\Games\\%s", fd.cFileName);
        strcpy(pGame->m_szXbe, "default.xbe");

        CHAR szPath [MAX_PATH];
        sprintf(szPath, "%s\\default.xbe", pGame->m_szDir);
        if (GetFileAttributesA(szPath) == (DWORD)-1)
        {
            WIN32_FIND_DATAA fdXbe;
            sprintf(szPath, "%s\\*.xbe", pGame->m_szDir);
            HANDLE hXbe = FindFirstFileA(szPath, &fdXbe);
            if (hXbe == INVALID_HANDLE_VALUE)
                continue;
            lstrcpynA(pGame->m_szXbe, fdXbe.cFileName, sizeof (pGame->m_szXbe));
            FindClose(hXbe);
        }
        sprintf(szPath, "%s\\%s", pGame->m_szDir, pGame->m_szXbe);
        if (!ReadXbeTitle(szPath, pGame))
            continue;

        if (pGame->m_szName[0] == 0)
        {
            for (int i = 0; i < 40 && fd.cFileName[i]; i += 1)
                pGame->m_szName[i] = fd.cFileName[i], pGame->m_szName[i + 1] = 0;
        }
        c_nGameCount += 1;
    }
    while (c_nGameCount < MAX_GAMES && FindNextFileA(hFind, &fd));

    FindClose(hFind);
    qsort(c_rgGames + c_nMediaCount, c_nGameCount - c_nMediaCount, sizeof (GAMEINFO), CompareGames);
    return c_nGameCount;
}

int CGameCollection::GetGameCount()
{
    return c_nGameCount;
}

int CGameCollection::GetMediaCount()
{
    return c_nMediaCount;
}

int CGameCollection::GetGameKind(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount)
        return KIND_EMPTY;
    return c_rgGames[nGame].m_nKind;
}

int CGameCollection::IsPlayable(int nGame)
{
    return nGame >= 0 && nGame < c_nGameCount && c_rgGames[nGame].m_bXbox;
}

CStrObject* CGameCollection::GetNotPlayableReason(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount || c_rgGames[nGame].m_nKind != KIND_CARD)
        return new CStrObject;
    const GAMEINFO* pGame = &c_rgGames[nGame];
    TCHAR sz [256];
    if (_stricmp(pGame->m_szRuntime, "xbox") == 0)
        _stprintf(sz, _T("The cart's Xbox game (%hs) couldn't be read."), pGame->m_szXbe);
    else if (EndsWith(pGame->m_szCart, ".kzp"))
        _stprintf(sz, _T("Packaged carts (.kzp) are started by Sion's cart launcher, not the dashboard."));
    else
        _stprintf(sz, _T("This cart uses the %hs runtime. Sion's cart launcher starts it, not the dashboard."), pGame->m_szRuntime[0] != 0 ? pGame->m_szRuntime : "none");
    return new CStrObject(sz);
}

CStrObject* CGameCollection::GetGameName(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount)
        return new CStrObject;
    return new CStrObject(c_rgGames[nGame].m_szName);
}

CStrObject* CGameCollection::GetGameFolder(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount)
        return new CStrObject;
    TCHAR sz [64];
    _stprintf(sz, _T("%hs"), c_rgGames[nGame].m_szFolder);
    return new CStrObject(sz);
}

CStrObject* CGameCollection::GetGameTitleID(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount)
        return new CStrObject;
    TCHAR sz [16];
    _stprintf(sz, _T("%08X"), c_rgGames[nGame].m_dwTitleID);
    return new CStrObject(sz);
}

// The platform a game is for and the runtime that runs it, the way a
// Kazeta cart names them (the .kzi's Runtime key: a platform, such as
// windows, xbox or megadrive, or one with a version, windows-proton-ge-9),
// for the game's page.  An Xbox game, wherever it sits, is Xbox on
// xbcompat.  A cart's runtime image is looked for on the card the way
// Kazeta resolves it (<runtime>.kzr, <runtime>-*.kzr), and that file's
// name is the runtime shown; a cart whose runtime lives on the console
// only names it.
static const struct { const CHAR* szRuntime; const TCHAR* szPlatform; } c_rgPlatforms [] =
{
    { "xbox", _T("Xbox") }, { "windows", _T("Windows") }, { "linux", _T("Linux") }, { "none", _T("Linux") },
    { "nes", _T("NES") }, { "snes", _T("Super NES") }, { "n64", _T("Nintendo 64") }, { "gb", _T("Game Boy") },
    { "gbc", _T("Game Boy Color") }, { "gba", _T("Game Boy Advance") }, { "nds", _T("Nintendo DS") },
    { "gamecube", _T("GameCube") }, { "wii", _T("Wii") }, { "megadrive", _T("Sega Genesis") },
    { "genesis", _T("Sega Genesis") }, { "mastersystem", _T("Sega Master System") }, { "gamegear", _T("Game Gear") },
    { "saturn", _T("Sega Saturn") }, { "dreamcast", _T("Dreamcast") }, { "psx", _T("PlayStation") },
    { "ps1", _T("PlayStation") }, { "ps2", _T("PlayStation 2") }, { "psp", _T("PlayStation Portable") },
    { "arcade", _T("Arcade") }, { "mame", _T("Arcade") }, { "dos", _T("DOS") }, { "scummvm", _T("ScummVM") },
};

// The platform part of a Runtime value: up to its first '-' (windows-proton-ge-9 -> windows).
static void RuntimePlatform(const CHAR* szRuntime, CHAR* szPlat, int cch)
{
    int i = 0;
    for (; i < cch - 1 && szRuntime[i] != 0 && szRuntime[i] != '-'; i += 1)
        szPlat[i] = szRuntime[i];
    szPlat[i] = 0;
}

CStrObject* CGameCollection::GetGamePlatform(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount)
        return new CStrObject;
    const GAMEINFO* pGame = &c_rgGames[nGame];
    if (pGame->m_nKind != KIND_CARD || pGame->m_bXbox)
        return new CStrObject(_T("Xbox"));
    if (pGame->m_szPlatform[0] != 0)
    {
        // a Platform key in the .kzi says it outright
        TCHAR szOwn [32];
        _stprintf(szOwn, _T("%hs"), pGame->m_szPlatform);
        return new CStrObject(szOwn);
    }
    if (EndsWith(pGame->m_szCart, ".kzp"))
        return new CStrObject(_T("Packaged cart"));
    CHAR szPlat [32];
    RuntimePlatform(pGame->m_szRuntime, szPlat, sizeof (szPlat));
    if (szPlat[0] == 0)
        return new CStrObject(_T("Linux"));      // Kazeta runs a cart with no Runtime on the none runtime
    for (int i = 0; i < countof(c_rgPlatforms); i += 1)
    {
        if (_stricmp(szPlat, c_rgPlatforms[i].szRuntime) == 0)
            return new CStrObject(c_rgPlatforms[i].szPlatform);
    }
    // an unlisted runtime: its name, capitalised
    TCHAR sz [32];
    _stprintf(sz, _T("%hs"), szPlat);
    if (sz[0] >= 'a' && sz[0] <= 'z')
        sz[0] = sz[0] - 'a' + 'A';
    return new CStrObject(sz);
}

CStrObject* CGameCollection::GetGameRuntime(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount)
        return new CStrObject;
    const GAMEINFO* pGame = &c_rgGames[nGame];
    if (pGame->m_nKind != KIND_CARD || pGame->m_bXbox)
        return new CStrObject(_T("xbcompat"));
    if (EndsWith(pGame->m_szCart, ".kzp"))
        return new CStrObject(_T("inside the cart"));
    if (pGame->m_szRuntime[0] == 0)
        return new CStrObject(_T("none"));

    // the runtime image on the card: <runtime>.kzr, then <runtime>-*.kzr
    CHAR szPattern [96];
    WIN32_FIND_DATAA fd;
    for (int nTry = 0; nTry < 2; nTry += 1)
    {
        sprintf(szPattern, nTry == 0 ? "CARD0:\\%s.kzr" : "CARD0:\\%s-*.kzr", pGame->m_szRuntime);
        HANDLE hFind = FindFirstFileA(szPattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            FindClose(hFind);
            CHAR* pchDot = strrchr(fd.cFileName, '.');
            if (pchDot != NULL)
                *pchDot = 0;
            TCHAR sz [80];
            _stprintf(sz, _T("%hs (on the card)"), fd.cFileName);
            return new CStrObject(sz);
        }
    }
    TCHAR sz [48];
    _stprintf(sz, _T("%hs"), pGame->m_szRuntime);
    return new CStrObject(sz);
}

// The XBE's path, for reading it again.
static void GetXbePath(int nGame, CHAR* szPath)
{
    sprintf(szPath, "%s\\%s", c_rgGames[nGame].m_szDir, c_rgGames[nGame].m_szXbe);
}

// The publisher behind a title ID: its top two bytes are two ASCII letters
// the publisher was assigned.  The names are the Title ID table of the
// XboxDev wiki's Xbe page (https://xboxdevwiki.net/Xbe, read 2026-10-20);
// a code not in it comes back as its two letters.
static const struct { CHAR sz[3]; const TCHAR* szName; } c_rgPublishers [] =
{
    { "AC", _T("Acclaim Entertainment") }, { "AH", _T("ARUSH Entertainment") }, { "AP", _T("Aquaplus") },
    { "AQ", _T("Aqua System") }, { "AS", _T("ASK") }, { "AT", _T("Atlus") }, { "AV", _T("Activision") },
    { "AY", _T("Aspyr Media") }, { "BA", _T("Bandai") }, { "BL", _T("Black Box") },
    { "BM", _T("BAM! Entertainment") }, { "BR", _T("Broccoli Co.") }, { "BS", _T("Bethesda Softworks") },
    { "BU", _T("Bunkasha Co.") }, { "BV", _T("Buena Vista Games") }, { "BW", _T("BBC Multimedia") },
    { "BZ", _T("Blizzard") }, { "CC", _T("Capcom") }, { "CK", _T("Kemco Corporation") },
    { "CM", _T("Codemasters") }, { "CT", _T("CTO S.p.A.") }, { "CV", _T("Crave Entertainment") },
    { "DC", _T("DreamCatcher Interactive") }, { "DX", _T("Davilex") }, { "EA", _T("Electronic Arts") },
    { "EC", _T("Encore") }, { "EL", _T("Enlight Software") }, { "EM", _T("Empire Interactive") },
    { "ES", _T("Eidos Interactive") }, { "FI", _T("Fox Interactive") }, { "FS", _T("From Software") },
    { "GE", _T("Genki Co.") }, { "GV", _T("Groove Games") }, { "HE", _T("Tru Blu") }, { "HP", _T("Hip Games") },
    { "HU", _T("Hudson Soft") }, { "HW", _T("Highwaystar") }, { "IA", _T("Mad Catz Interactive") },
    { "IF", _T("Idea Factory") }, { "IG", _T("Infogrames") }, { "IL", _T("Interlex Corporation") },
    { "IM", _T("Imagine Media") }, { "IO", _T("Ignition Entertainment") }, { "IP", _T("Interplay Entertainment") },
    { "IX", _T("InXile Entertainment") }, { "JA", _T("Jaleco") }, { "JW", _T("JoWooD") }, { "KB", _T("Kemco") },
    { "KI", _T("Kids Station Inc.") }, { "KK", _T("KiKi") }, { "KN", _T("Konami") }, { "KO", _T("KOEI") },
    { "KT", _T("Konami Tokyo") }, { "KU", _T("Kobi / GAE") }, { "LA", _T("LucasArts") },
    { "LS", _T("Black Bean Games") }, { "MD", _T("Metro3D") }, { "ME", _T("Medix") }, { "MI", _T("Microids") },
    { "MJ", _T("Majesco Entertainment") }, { "MM", _T("Myelin Media") }, { "MP", _T("MediaQuest") },
    { "MS", _T("Microsoft Game Studios") }, { "MW", _T("Midway Games") }, { "MX", _T("Empire Interactive") },
    { "NK", _T("NewKidCo") }, { "NL", _T("NovaLogic") }, { "NM", _T("Namco") }, { "OX", _T("Oxygen Interactive") },
    { "PC", _T("Playlogic Entertainment") }, { "PL", _T("Phantagram Co., Ltd.") }, { "RA", _T("Rage") },
    { "SA", _T("Sammy") }, { "SC", _T("SCi Games") }, { "SE", _T("SEGA") }, { "SN", _T("SNK") },
    { "SS", _T("Simon & Schuster") }, { "SU", _T("Success Corporation") }, { "SW", _T("Swing! Deutschland") },
    { "TA", _T("Takara") }, { "TC", _T("Tecmo") }, { "TD", _T("The 3DO Company") }, { "TK", _T("Takuyo") },
    { "TM", _T("TDK Mediactive") }, { "TQ", _T("THQ") }, { "TS", _T("Titus Interactive") },
    { "TT", _T("Take-Two Interactive Software") }, { "US", _T("Ubisoft") }, { "VC", _T("Victor Interactive Software") },
    { "VN", _T("Vivendi Universal") }, { "VU", _T("Vivendi Universal Games") }, { "VV", _T("Vivendi Universal Games") },
    { "WE", _T("Wanadoo Edition") }, { "WR", _T("Warner Bros. Interactive Entertainment") },
    { "XM", _T("Evolved Games") }, { "XP", _T("XPEC Entertainment") }, { "XR", _T("Panorama") },
    { "YB", _T("YBM Sisa") }, { "ZD", _T("Zushi Games") },
};

static const TCHAR* GetPublisher(DWORD dwTitleID)
{
    CHAR sz[3] = { (CHAR)(dwTitleID >> 24), (CHAR)(dwTitleID >> 16), 0 };
    for (int i = 0; i < countof(c_rgPublishers); i += 1)
    {
        if (strcmp(sz, c_rgPublishers[i].sz) == 0)
            return c_rgPublishers[i].szName;
    }
    return NULL;
}

// The publisher as the library tile shows it: an Xbox game's from its
// title ID (the two letters themselves when the table has no name for
// them), a cart's from its .kzi Publisher (or Developer) key, else nothing.
CStrObject* CGameCollection::GetGamePublisher(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount)
        return new CStrObject;
    const GAMEINFO* pGame = &c_rgGames[nGame];
    if (pGame->m_szPublisher[0] != 0)
    {
        TCHAR szOwn [48];
        _stprintf(szOwn, _T("%hs"), pGame->m_szPublisher);
        return new CStrObject(szOwn);
    }
    if (pGame->m_nKind != KIND_CARD || pGame->m_bXbox)
    {
        const TCHAR* szPub = GetPublisher(pGame->m_dwTitleID);
        if (szPub != NULL)
            return new CStrObject(szPub);
        TCHAR szCode [3] = { (TCHAR)(BYTE)(pGame->m_dwTitleID >> 24), (TCHAR)(BYTE)(pGame->m_dwTitleID >> 16), 0 };
        return new CStrObject(szCode);
    }
    return new CStrObject;
}

// Days since 1970-01-01 to a calendar date.
static void DaysToDate(DWORD dwDays, int* pnYear, int* pnMonth, int* pnDay)
{
    int z = dwDays + 719468;
    int era = z / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *pnDay = doy - (153 * mp + 2) / 5 + 1;
    *pnMonth = mp < 10 ? mp + 3 : mp - 9;
    *pnYear = yoe + era * 400 + (*pnMonth <= 2);
}

CStrObject* CGameCollection::GetGameInfo(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount || c_rgGames[nGame].m_szName[0] == 0)
        return new CStrObject;

    const GAMEINFO* pGame = &c_rgGames[nGame];
    TCHAR sz [1024];
    TCHAR* pch = sz;

    if (pGame->m_nKind == KIND_CARD)
    {
        pch += _stprintf(pch, _T("Game card: %hs\n"), pGame->m_szCart);
        if (pGame->m_szRuntime[0] != 0)
            pch += _stprintf(pch, _T("Runtime: %hs\n"), pGame->m_szRuntime);
        if (pGame->m_szId[0] != 0)
            pch += _stprintf(pch, _T("Cart Id: %hs\n"), pGame->m_szId);
        if (!pGame->m_bXbox)
            return new CStrObject(sz);
    }

    pch += _stprintf(pch, _T("Title ID: %08X\n"), pGame->m_dwTitleID);
    const TCHAR* szPub = GetPublisher(pGame->m_dwTitleID);
    if (szPub != NULL)
        pch += _stprintf(pch, _T("Publisher: %s\n"), szPub);
    else if (pGame->m_dwTitleID >> 16 == 0xFFFF)
        pch += _stprintf(pch, _T("Publisher: Sample / test title\n"));
    else
        pch += _stprintf(pch, _T("Publisher code: %hc%hc\n"), (CHAR)(pGame->m_dwTitleID >> 24), (CHAR)(pGame->m_dwTitleID >> 16));

    pch += _stprintf(pch, _T("Version: %u\n"), pGame->m_dwVersion);

    pch += _stprintf(pch, _T("Region:"));
    if ((pGame->m_dwRegion & 7) == 7)
        pch += _stprintf(pch, _T(" All"));
    else
    {
        if (pGame->m_dwRegion & 1)
            pch += _stprintf(pch, _T(" North America"));
        if (pGame->m_dwRegion & 2)
            pch += _stprintf(pch, _T(" Japan"));
        if (pGame->m_dwRegion & 4)
            pch += _stprintf(pch, _T(" Rest of world"));
    }
    pch += _stprintf(pch, _T("\n"));

    if (pGame->m_nKind == KIND_DISC)
        pch += _stprintf(pch, _T("Disc %u in the tray\n"), pGame->m_dwDiscNumber + 1);
    else if (pGame->m_nKind == KIND_HDD)
        pch += _stprintf(pch, _T("Installed: E:\\Games\\%hs\n"), pGame->m_szFolder);

    int nYear, nMonth, nDay;
    DaysToDate(pGame->m_dwTimeDate / 86400, &nYear, &nMonth, &nDay);
    pch += _stprintf(pch, _T("Built: %04d-%02d-%02d\n"), nYear, nMonth, nDay);

    pch += _stprintf(pch, _T("Saved games: %d"), GetSavedGameCount(nGame));

    return new CStrObject(sz);
}

// Calls pfn for each saved game of the title (E:\UDATA\<title id>\<save>).
static int EnumSavedGames(DWORD dwTitleID, void (*pfn)(const CHAR* szDir, const WIN32_FIND_DATAA* pfd, void* pv), void* pv)
{
    CHAR szDir [MAX_PATH];
    sprintf(szDir, "E:\\UDATA\\%08x\\*", dwTitleID);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(szDir, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return 0;

    int nSaves = 0;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
            continue;
        if (pfn != NULL)
        {
            sprintf(szDir, "E:\\UDATA\\%08x\\%s", dwTitleID, fd.cFileName);
            pfn(szDir, &fd, pv);
        }
        nSaves += 1;
    }
    while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return nSaves;
}

int CGameCollection::GetSavedGameCount(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount || !c_rgGames[nGame].m_bXbox)
        return 0;
    return EnumSavedGames(c_rgGames[nGame].m_dwTitleID, NULL, NULL);
}

struct SAVELIST
{
    TCHAR* m_pch;
    TCHAR* m_pchLim;
};

// One line per save: its name (SaveMeta.xbx, "Name=" in UTF-16) and date.
static void AddSavedGame(const CHAR* szDir, const WIN32_FIND_DATAA* pfd, void* pv)
{
    SAVELIST* pList = (SAVELIST*)pv;
    if (pList->m_pchLim - pList->m_pch < 80)
        return;

    WCHAR szName [64];
    szName[0] = 0;

    CHAR szPath [MAX_PATH];
    sprintf(szPath, "%s\\SaveMeta.xbx", szDir);
    HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        WCHAR rgch [256];
        DWORD cb = 0;
        if (ReadFile(hFile, rgch, sizeof (rgch) - sizeof (WCHAR), &cb, NULL))
        {
            rgch[cb / sizeof (WCHAR)] = 0;
            WCHAR* pchName = wcsstr(rgch, L"Name=");
            if (pchName != NULL)
            {
                pchName += 5;
                int i = 0;
                while (i < 40 && pchName[i] != 0 && pchName[i] != '\r' && pchName[i] != '\n')
                    szName[i] = pchName[i], i += 1;
                szName[i] = 0;
            }
        }
        CloseHandle(hFile);
    }
    if (szName[0] == 0)
    {
        int i = 0;
        for (; i < 40 && pfd->cFileName[i]; i += 1)
            szName[i] = pfd->cFileName[i];
        szName[i] = 0;
    }

    SYSTEMTIME st;
    FileTimeToSystemTime(&pfd->ftLastWriteTime, &st);
    pList->m_pch += _stprintf(pList->m_pch, _T("%s\n    %04d-%02d-%02d\n"), szName, st.wYear, st.wMonth, st.wDay);
}

CStrObject* CGameCollection::GetSavedGames(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount || c_rgGames[nGame].m_szName[0] == 0)
        return new CStrObject;

    TCHAR sz [2048];
    SAVELIST list = { sz, sz + countof(sz) };
    sz[0] = 0;
    if (EnumSavedGames(c_rgGames[nGame].m_dwTitleID, AddSavedGame, &list) == 0)
        _tcscpy(sz, _T("No saved games."));
    return new CStrObject(sz);
}

// Copies a game's title image out of its XBE into T:\GameImages (the
// dashboard's own title data), unless it is there already.
static bool CacheTitleImage(const CHAR* szPath, const GAMEINFO* pGame, CHAR* szCache)
{
    CreateDirectoryA("T:\\GameImages", NULL);
    sprintf(szCache, "T:\\GameImages\\%08X%08X.xbx", pGame->m_dwTitleID, pGame->m_dwTimeDate);

    if (GetFileAttributesA(szCache) == (DWORD)-1)
    {
        if (pGame->m_dwImageSize == 0)
            return false;

        HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        bool bOK = false;
        BYTE* pb = new BYTE [pGame->m_dwImageSize];
        DWORD cb = 0;
        if (SetFilePointer(hFile, pGame->m_dwImageOffset, NULL, FILE_BEGIN) == pGame->m_dwImageOffset &&
            ReadFile(hFile, pb, pGame->m_dwImageSize, &cb, NULL) && cb == pGame->m_dwImageSize &&
            *(DWORD*)pb == 0x30525058)  // "XPR0"
        {
            HANDLE hOut = CreateFileA(szCache, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (hOut != INVALID_HANDLE_VALUE)
            {
                bOK = WriteFile(hOut, pb, cb, &cb, NULL) && cb == pGame->m_dwImageSize;
                CloseHandle(hOut);
                if (!bOK)
                    DeleteFileA(szCache);
            }
        }
        delete [] pb;
        CloseHandle(hFile);
        if (!bOK)
            return false;
    }

    return true;
}

// For the Memory screen (TitleCollection.cpp): an installed game's title ID,
// build time and name, and its cached title image.
bool ReadXbeTitleInfo(const CHAR* szXbe, DWORD* pdwTitleID, DWORD* pdwTimeDate, WCHAR* szName /*[41]*/)
{
    GAMEINFO game;
    ZeroMemory(&game, sizeof (game));
    if (!ReadXbeTitle(szXbe, &game))
        return false;

    *pdwTitleID = game.m_dwTitleID;
    *pdwTimeDate = game.m_dwTimeDate;
    CopyMemory(szName, game.m_szName, sizeof (game.m_szName));
    return true;
}

bool CacheXbeTitleImage(const CHAR* szXbe, CHAR* szCache /*[MAX_PATH]*/)
{
    GAMEINFO game;
    ZeroMemory(&game, sizeof (game));
    return ReadXbeTitle(szXbe, &game) && CacheTitleImage(szXbe, &game, szCache);
}

// The game's title image as a file (T:\GameImages, cached out of the XBE),
// for Frolick's belt covers; "" when the XBE has none.
CStrObject* CGameCollection::GetGameImage(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount || c_rgGames[nGame].m_dwImageSize == 0)
        return new CStrObject;
    CHAR szPath [MAX_PATH];
    CHAR szCache [MAX_PATH];
    GetXbePath(nGame, szPath);
    if (!CacheTitleImage(szPath, &c_rgGames[nGame], szCache))
        return new CStrObject;
    TCHAR sz [MAX_PATH];
    _stprintf(sz, _T("%hs"), szCache);
    return new CStrObject(sz);
}

// Points the "SelectedIcon" material at the game's title image, or at the
// Xbox logo when it has none.  Returns whether the game has its own image.
int CGameCollection::SelectGameImage(int nGame)
{
    static TCHAR szImage [MAX_PATH];
    g_szSelTitleImage = _T("xboxlogo128.xbx");

    if (nGame < 0 || nGame >= c_nGameCount || c_rgGames[nGame].m_dwImageSize == 0)
        return 0;

    CHAR szPath [MAX_PATH];
    CHAR szCache [MAX_PATH];
    GetXbePath(nGame, szPath);
    if (!CacheTitleImage(szPath, &c_rgGames[nGame], szCache))
        return 0;

    _stprintf(szImage, _T("%hs"), szCache);
    g_szSelTitleImage = szImage;
    return 1;
}

// ---- recently played ------------------------------------------------------
// The library pad's X: the games launched most recently, newest first.
// Each launch writes its game's key to the front of T:\recent.txt (one key
// a line, an XBE's title ID or a cart's Id, 20 at most); the list is the
// scanned games those keys name, in that order.

#define RECENT_MAX 20
static const CHAR c_szRecentFile[] = "T:\\recent.txt";

static void GameKey(const GAMEINFO* pGame, CHAR* sz, int cch)
{
    if (pGame->m_bXbox || pGame->m_szId[0] == 0)
        _snprintf(sz, cch, "%08X", pGame->m_dwTitleID);
    else
        _snprintf(sz, cch, "%s", pGame->m_szId);
    sz[cch - 1] = 0;
}

static int ReadRecent(CHAR rgsz[RECENT_MAX][64])
{
    int n = 0;
    HANDLE hFile = CreateFileA(c_szRecentFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return 0;
    CHAR szBuf [RECENT_MAX * 64 + 1];
    DWORD cb = 0;
    ReadFile(hFile, szBuf, sizeof (szBuf) - 1, &cb, NULL);
    CloseHandle(hFile);
    szBuf[cb] = 0;
    CHAR* pch = szBuf;
    while (*pch != 0 && n < RECENT_MAX)
    {
        CHAR* pchEnd = pch;
        while (*pchEnd != 0 && *pchEnd != '\n' && *pchEnd != '\r')
            pchEnd += 1;
        int cch = pchEnd - pch;
        if (cch > 0 && cch < 64)
        {
            memcpy(rgsz[n], pch, cch);
            rgsz[n][cch] = 0;
            n += 1;
        }
        while (*pchEnd == '\n' || *pchEnd == '\r')
            pchEnd += 1;
        pch = pchEnd;
    }
    return n;
}

static void RecordLaunch(const GAMEINFO* pGame)
{
    CHAR rgsz [RECENT_MAX][64];
    CHAR szKey [64];
    GameKey(pGame, szKey, sizeof (szKey));
    int n = ReadRecent(rgsz);
    HANDLE hFile = CreateFileA(c_szRecentFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    DWORD cb;
    WriteFile(hFile, szKey, strlen(szKey), &cb, NULL);
    WriteFile(hFile, "\r\n", 2, &cb, NULL);
    int kept = 1;
    for (int i = 0; i < n && kept < RECENT_MAX; i += 1)
    {
        if (strcmp(rgsz[i], szKey) == 0)
            continue;
        WriteFile(hFile, rgsz[i], strlen(rgsz[i]), &cb, NULL);
        WriteFile(hFile, "\r\n", 2, &cb, NULL);
        kept += 1;
    }
    CloseHandle(hFile);
}

static int c_rgRecent [RECENT_MAX];
static int c_nRecent;

// The recently played games among those the last Scan found, newest first.
int CGameCollection::GetRecentCount()
{
    CHAR rgsz [RECENT_MAX][64];
    CHAR szKey [64];
    int n = ReadRecent(rgsz);
    c_nRecent = 0;
    for (int i = 0; i < n; i += 1)
    {
        for (int g = 0; g < c_nGameCount; g += 1)
        {
            if (c_rgGames[g].m_szName[0] == 0)
                continue;
            GameKey(&c_rgGames[g], szKey, sizeof (szKey));
            if (strcmp(szKey, rgsz[i]) == 0)
            {
                c_rgRecent[c_nRecent++] = g;
                break;
            }
        }
    }
    return c_nRecent;
}

int CGameCollection::GetRecentGame(int n)
{
    if (n < 0 || n >= c_nRecent)
        return -1;
    return c_rgRecent[n];
}

void CGameCollection::LaunchGame(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount || !c_rgGames[nGame].m_bXbox)
        return;

    // D: is the game's directory (XLaunchNewImage keeps D:'s mapping across
    // the reboot): CDROM0:, CARD0: and E: as device paths.
    const GAMEINFO* pGame = &c_rgGames[nGame];
    RecordLaunch(pGame);
    CHAR szTarget [MAX_PATH];
    if (pGame->m_nKind == KIND_DISC)
        strcpy(szTarget, "\\Device\\CdRom0");
    else if (pGame->m_nKind == KIND_CARD)
        sprintf(szTarget, "\\Device\\GameCard0%s", pGame->m_szDir + 6);
    else
        sprintf(szTarget, "\\Device\\Harddisk0\\Partition1\\Games\\%s", pGame->m_szFolder);
    OBJECT_STRING dDrive = CONSTANT_OBJECT_STRING("\\??\\D:");
    OBJECT_STRING target;
    RtlInitObjectString(&target, szTarget);
    IoDeleteSymbolicLink(&dDrive);
    IoCreateSymbolicLink(&dDrive, &target);

    CHAR szImage [MAX_PATH];
    sprintf(szImage, "D:\\%s", pGame->m_szXbe);
    XAppGetD3DDev()->PersistDisplay();
    XLaunchNewImage(szImage, NULL);
}

// The Live dashboard, where the retail dashboards keep it: C:\xodash, run
// with D: there.
void CGameCollection::LaunchXOnlineDash()
{
    // The retail online dashboard (5849) returns straight to the dashboard
    // unless it gets LDT_LAUNCH_DASHBOARD with a reason of its own, 0xc to
    // 0x13; the retail 5960 dashboard's Xbox Live item sends 0xc.
    LD_LAUNCH_DASHBOARD ld;
    ZeroMemory(&ld, sizeof(ld));
    ld.dwReason = 0xc;
    XAppGetD3DDev()->PersistDisplay();
    XWriteTitleInfoAndReboot("xonlinedash.xbe", "\\Device\\Harddisk0\\Partition2\\xodash",
        LDT_LAUNCH_DASHBOARD, 0xfffe0000, (PLAUNCH_DATA)&ld);
}

////////////////////////////////////////////////////////////////////////////
// Installing a disc or cart: the game's directory (the whole disc, or the
// cart's XBE directory on the card) is copied to E:\Games\<title>, which
// GameCollection then lists and launches like any other installed game.

struct INSTALL
{
    HANDLE m_hThread;
    CHAR m_szSource[160];
    CHAR m_szFolder[64];        // E:\Games\<m_szFolder>
    ULONGLONG m_qwTotal;
    volatile ULONGLONG m_qwCopied;
    volatile bool m_bNoSpace;
    volatile bool m_bFailed;
};

static INSTALL c_install;

// Total size of a directory tree; false if it can't be read.
static bool SizeTree(const CHAR* szDir, ULONGLONG* pqw)
{
    CHAR szPath [MAX_PATH];
    sprintf(szPath, "%s\\*", szDir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(szPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_NO_MORE_FILES;

    bool bOK = true;
    do
    {
        if (fd.cFileName[0] == '.')
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            sprintf(szPath, "%s\\%s", szDir, fd.cFileName);
            bOK = SizeTree(szPath, pqw);
        }
        else
        {
            *pqw += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    }
    while (bOK && FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return bOK;
}

static bool CopyOneFile(const CHAR* szSrc, const CHAR* szDest, BYTE* pbBuf, DWORD cbBuf)
{
    HANDLE hSrc = CreateFileA(szSrc, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hSrc == INVALID_HANDLE_VALUE)
        return false;
    HANDLE hDest = CreateFileA(szDest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hDest == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hSrc);
        return false;
    }

    bool bOK = true;
    for (;;)
    {
        DWORD cbRead = 0, cbWritten = 0;
        if (!ReadFile(hSrc, pbBuf, cbBuf, &cbRead, NULL))
        {
            bOK = false;
            break;
        }
        if (cbRead == 0)
            break;
        if (!WriteFile(hDest, pbBuf, cbRead, &cbWritten, NULL) || cbWritten != cbRead)
        {
            bOK = false;
            break;
        }
        c_install.m_qwCopied += cbRead;
    }

    CloseHandle(hDest);
    CloseHandle(hSrc);
    return bOK;
}

static bool CopyTree(const CHAR* szSrc, const CHAR* szDest, BYTE* pbBuf, DWORD cbBuf)
{
    if (!CreateDirectoryA(szDest, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    CHAR szPath [MAX_PATH], szTo [MAX_PATH];
    sprintf(szPath, "%s\\*", szSrc);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(szPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return true;   // empty directory

    bool bOK = true;
    do
    {
        if (fd.cFileName[0] == '.')
            continue;
        sprintf(szPath, "%s\\%s", szSrc, fd.cFileName);
        sprintf(szTo, "%s\\%s", szDest, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            bOK = CopyTree(szPath, szTo, pbBuf, cbBuf);
        else
            bOK = CopyOneFile(szPath, szTo, pbBuf, cbBuf);
    }
    while (bOK && FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return bOK;
}

// Removes a partly copied install.
static void DeleteTree(const CHAR* szDir)
{
    CHAR szPath [MAX_PATH];
    sprintf(szPath, "%s\\*", szDir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(szPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.cFileName[0] == '.')
                continue;
            sprintf(szPath, "%s\\%s", szDir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteTree(szPath);
            else
                DeleteFileA(szPath);
        }
        while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    RemoveDirectoryA(szDir);
}

static DWORD WINAPI InstallThread(LPVOID)
{
    CHAR szDest [MAX_PATH];
    sprintf(szDest, "E:\\Games\\%s", c_install.m_szFolder);

    ULARGE_INTEGER qwAvail, qwTotal, qwFree;
    if (GetDiskFreeSpaceExA("E:\\", &qwAvail, &qwTotal, &qwFree) && qwAvail.QuadPart < c_install.m_qwTotal)
    {
        c_install.m_bNoSpace = true;
        return 0;
    }

    const DWORD cbBuf = 256 * 1024;
    BYTE* pbBuf = new BYTE [cbBuf];
    CreateDirectoryA("E:\\Games", NULL);
    if (!CopyTree(c_install.m_szSource, szDest, pbBuf, cbBuf))
    {
        c_install.m_bFailed = true;
        DeleteTree(szDest);
    }
    delete [] pbBuf;
    return 0;
}

// The installed copy of a disc or cart game, if there is one.
static int FindInstalled(int nGame)
{
    if (nGame < 0 || nGame >= c_nMediaCount || !c_rgGames[nGame].m_bXbox)
        return -1;
    for (int i = c_nMediaCount; i < c_nGameCount; i += 1)
    {
        if (c_rgGames[i].m_dwTitleID == c_rgGames[nGame].m_dwTitleID &&
            c_rgGames[i].m_dwTimeDate == c_rgGames[nGame].m_dwTimeDate)
            return i;
    }
    return -1;
}

int CGameCollection::IsInstalled(int nGame)
{
    return FindInstalled(nGame) >= 0;
}

int CGameCollection::StartInstall(int nGame)
{
    if (c_install.m_hThread != NULL || nGame < 0 || nGame >= c_nMediaCount || !c_rgGames[nGame].m_bXbox ||
        FindInstalled(nGame) >= 0)
        return 0;

    // The folder is the game's name, kept to characters FATX allows.
    CHAR szName [43];
    int cch = 0;
    for (const WCHAR* pch = c_rgGames[nGame].m_szName; *pch != 0 && cch < 36; pch += 1)
    {
        WCHAR ch = *pch;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == ' ' || ch == '-' || ch == '_' || ch == '.' || ch == '(' || ch == ')' || ch == '!' || ch == '\'')
            szName[cch++] = (CHAR)ch;
    }
    while (cch > 0 && (szName[cch - 1] == ' ' || szName[cch - 1] == '.'))
        cch -= 1;
    szName[cch] = 0;
    if (cch == 0)
        strcpy(szName, "Game");

    // A different game (or version) may already have that name.
    CHAR szPath [MAX_PATH];
    strcpy(c_install.m_szFolder, szName);
    for (int n = 2; n < 100; n += 1)
    {
        sprintf(szPath, "E:\\Games\\%s", c_install.m_szFolder);
        if (GetFileAttributesA(szPath) == (DWORD)-1)
            break;
        sprintf(c_install.m_szFolder, "%s %d", szName, n);
    }

    c_install.m_qwTotal = 0;
    c_install.m_qwCopied = 0;
    c_install.m_bNoSpace = false;
    c_install.m_bFailed = false;
    strcpy(c_install.m_szSource, c_rgGames[nGame].m_szDir);
    if (!SizeTree(c_install.m_szSource, &c_install.m_qwTotal))
        return 0;

    m_installProgress = 0.0f;
    c_install.m_hThread = CreateThread(NULL, 0, InstallThread, NULL, 0, NULL);
    return c_install.m_hThread != NULL;
}

CStrObject* CGameCollection::GetInstallStatus()
{
    TCHAR sz [64];
    _stprintf(sz, _T("%u of %u MB"), (DWORD)(c_install.m_qwCopied >> 20), (DWORD)((c_install.m_qwTotal + 0xFFFFF) >> 20));
    return new CStrObject(sz);
}

CStrObject* CGameCollection::GetInstallError()
{
    if (c_install.m_bNoSpace)
        return new CStrObject(_T("There isn't enough free space on the hard disk to install this game."));
    return new CStrObject(_T("The game couldn't be installed. Check the disc or card and try again."));
}

CStrObject* CGameCollection::GetInstallFolder()
{
    TCHAR sz [80];
    _stprintf(sz, _T("E:\\Games\\%hs"), c_install.m_szFolder);
    return new CStrObject(sz);
}

void CGameCollection::Advance(float nSeconds)
{
    CNode::Advance(nSeconds);

    // Cards come and go: look for a change once a second.
    if (c_install.m_hThread == NULL)
    {
        static XTIME timeLastPoll = 0.0f;
        if (c_rgGames == NULL || XAppGetNow() - timeLastPoll < 1.0f)
            return;
        timeLastPoll = XAppGetNow();

        CHAR szSig [sizeof (c_szCardSig)];
        GetCardSignature(szSig, sizeof (szSig));
        if (strcmp(szSig, c_szCardSig) != 0)
        {
            Scan();
            CallFunction(this, _T("OnMediaChanged"));
        }
        return;
    }

    if (c_install.m_qwTotal != 0)
    {
        float progress = (float)((double)(LONGLONG)c_install.m_qwCopied / (double)(LONGLONG)c_install.m_qwTotal);
        if (progress - m_installProgress >= 0.01f || (progress >= 1.0f && m_installProgress < 1.0f))
        {
            m_installProgress = progress;
            CallFunction(this, _T("OnInstallProgress"));
        }
    }

    if (WaitForSingleObject(c_install.m_hThread, 0) != WAIT_OBJECT_0)
        return;

    CloseHandle(c_install.m_hThread);
    c_install.m_hThread = NULL;
    Scan();
    TitleArray_GamesChanged();  // the Memory screen lists it now (or what a failed install left)
    if (c_install.m_bNoSpace || c_install.m_bFailed)
        CallFunction(this, _T("OnInstallError"));
    else
        CallFunction(this, _T("OnInstallComplete"));
}
