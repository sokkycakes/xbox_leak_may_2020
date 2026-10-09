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
    CStrObject* GetGameInfo(int nGame);
    CStrObject* GetSavedGames(int nGame);
    int GetSavedGameCount(int nGame);
    int SelectGameImage(int nGame);
    void LaunchGame(int nGame);

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
    NODE_FUN_SI(GetGameInfo)
    NODE_FUN_SI(GetSavedGames)
    NODE_FUN_II(GetSavedGameCount)
    NODE_FUN_II(SelectGameImage)
    NODE_FUN_VI(LaunchGame)
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

// The carts' file names on the game card, to notice it changing.
static void GetCardSignature(CHAR* szSig, int cchSig)
{
    szSig[0] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("CARD0:\\*", &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;
    int cch = 0;
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

// The XBE's path, for reading it again.
static void GetXbePath(int nGame, CHAR* szPath)
{
    sprintf(szPath, "%s\\%s", c_rgGames[nGame].m_szDir, c_rgGames[nGame].m_szXbe);
}

static const TCHAR* GetPublisher(DWORD dwTitleID)
{
    static const struct { CHAR sz[3]; const TCHAR* szName; } rgPub [] =
    {
        { "MS", _T("Microsoft") }, { "EA", _T("Electronic Arts") }, { "AC", _T("Acclaim") },
        { "AV", _T("Activision") }, { "CM", _T("Capcom") }, { "KN", _T("Konami") },
        { "NM", _T("Namco") }, { "SE", _T("Sega") }, { "TC", _T("Tecmo") },
        { "TQ", _T("THQ") }, { "UB", _T("Ubisoft") }, { "EM", _T("Eidos") },
        { "IF", _T("Infogrames") }, { "MW", _T("Midway") }, { "SQ", _T("Square Enix") },
    };
    CHAR sz[3] = { (CHAR)(dwTitleID >> 24), (CHAR)(dwTitleID >> 16), 0 };
    for (int i = 0; i < countof(rgPub); i += 1)
    {
        if (strcmp(sz, rgPub[i].sz) == 0)
            return rgPub[i].szName;
    }
    return NULL;
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

void CGameCollection::LaunchGame(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount || !c_rgGames[nGame].m_bXbox)
        return;

    // D: is the game's directory (XLaunchNewImage keeps D:'s mapping across
    // the reboot): CDROM0:, CARD0: and E: as device paths.
    const GAMEINFO* pGame = &c_rgGames[nGame];
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
