// GameCollection: the dashboard's Games area.  Entry 0 is the disc tray (the
// game disc in the drive, if any); after it come the titles installed on the
// hard disk as E:\Games\<folder>\default.xbe, named from each XBE's
// certificate.  A hard disk game is launched by pointing D: at its folder.
// It also supplies the details screen: the XBE's title image, its
// certificate details and the title's saved games, and installs the disc
// to E:\Games on a background thread (OnInstallProgress, OnInstallComplete
// and OnInstallError are called on the node while it runs).  Not part of Microsoft's
// dashboard; added by xbcompat's dashbuild.

#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"

#define MAX_GAMES 256

struct GAMEINFO
{
    CHAR m_szFolder[64];        // empty for the disc
    WCHAR m_szName[41];         // empty for an empty tray
    DWORD m_dwTitleID;
    DWORD m_dwTimeDate;         // XBE build time (seconds since 1970)
    DWORD m_dwRegion;
    DWORD m_dwDiscNumber;
    DWORD m_dwVersion;
    DWORD m_dwImageOffset;      // $$XTIMAGE section in the file, if any
    DWORD m_dwImageSize;
};

extern const TCHAR* g_szSelTitleImage;  // MaxMat.cpp: the "SelectedIcon" material's image

static GAMEINFO* c_rgGames = NULL;
static int c_nGameCount = 0;

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
    int HasDisc();
    CStrObject* GetGameName(int nGame);
    CStrObject* GetGameFolder(int nGame);
    CStrObject* GetGameTitleID(int nGame);
    CStrObject* GetGameInfo(int nGame);
    CStrObject* GetSavedGames(int nGame);
    int GetSavedGameCount(int nGame);
    int SelectGameImage(int nGame);
    void LaunchGame(int nGame);

    int IsDiscInstalled();
    int StartInstall();
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
    NODE_FUN_IV(HasDisc)
    NODE_FUN_SI(GetGameName)
    NODE_FUN_SI(GetGameFolder)
    NODE_FUN_SI(GetGameTitleID)
    NODE_FUN_SI(GetGameInfo)
    NODE_FUN_SI(GetSavedGames)
    NODE_FUN_II(GetSavedGameCount)
    NODE_FUN_II(SelectGameImage)
    NODE_FUN_VI(LaunchGame)
    NODE_FUN_IV(IsDiscInstalled)
    NODE_FUN_IV(StartInstall)
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

int CGameCollection::Scan()
{
    if (c_rgGames == NULL)
        c_rgGames = new GAMEINFO [MAX_GAMES];

    // Entry 0: the disc tray.  An empty tray (or a disc that is not a game)
    // leaves the name empty.
    ZeroMemory(&c_rgGames[0], sizeof (GAMEINFO));
    if (!ReadXbeTitle("CDROM0:\\default.xbe", &c_rgGames[0]))
        ZeroMemory(&c_rgGames[0], sizeof (GAMEINFO));
    else if (c_rgGames[0].m_szName[0] == 0)
        lstrcpyW(c_rgGames[0].m_szName, L"Game Disc");
    c_nGameCount = 1;

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("E:\\Games\\*", &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return c_nGameCount;

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' ||
            strlen(fd.cFileName) >= sizeof (c_rgGames[0].m_szFolder))
            continue;

        GAMEINFO* pGame = &c_rgGames[c_nGameCount];
        CHAR szPath [MAX_PATH];
        sprintf(szPath, "E:\\Games\\%s\\default.xbe", fd.cFileName);
        if (!ReadXbeTitle(szPath, pGame))
            continue;

        strcpy(pGame->m_szFolder, fd.cFileName);
        if (pGame->m_szName[0] == 0)
        {
            for (int i = 0; i < 40 && fd.cFileName[i]; i += 1)
                pGame->m_szName[i] = fd.cFileName[i], pGame->m_szName[i + 1] = 0;
        }
        c_nGameCount += 1;
    }
    while (c_nGameCount < MAX_GAMES && FindNextFileA(hFind, &fd));

    FindClose(hFind);
    qsort(c_rgGames + 1, c_nGameCount - 1, sizeof (GAMEINFO), CompareGames);
    return c_nGameCount;
}

int CGameCollection::GetGameCount()
{
    return c_nGameCount;
}

int CGameCollection::HasDisc()
{
    return c_nGameCount > 0 && c_rgGames[0].m_szName[0] != 0;
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
    if (nGame == 0)
        strcpy(szPath, "CDROM0:\\default.xbe");
    else
        sprintf(szPath, "E:\\Games\\%s\\default.xbe", c_rgGames[nGame].m_szFolder);
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

    if (nGame == 0)
        pch += _stprintf(pch, _T("Disc %u in the tray\n"), pGame->m_dwDiscNumber + 1);
    else
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
    if (nGame < 0 || nGame >= c_nGameCount || c_rgGames[nGame].m_szName[0] == 0)
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

// Points the "SelectedIcon" material at the game's title image, copied out of
// its XBE into T:\GameImages (the dashboard's own title data), or at the
// Xbox logo when it has none.  Returns whether the game has its own image.
int CGameCollection::SelectGameImage(int nGame)
{
    static TCHAR szImage [MAX_PATH];
    g_szSelTitleImage = _T("xboxlogo128.xbx");

    if (nGame < 0 || nGame >= c_nGameCount || c_rgGames[nGame].m_dwImageSize == 0)
        return 0;

    const GAMEINFO* pGame = &c_rgGames[nGame];
    CHAR szCache [MAX_PATH];
    CreateDirectoryA("T:\\GameImages", NULL);
    sprintf(szCache, "T:\\GameImages\\%08X%08X.xbx", pGame->m_dwTitleID, pGame->m_dwTimeDate);

    if (GetFileAttributesA(szCache) == (DWORD)-1)
    {
        CHAR szPath [MAX_PATH];
        GetXbePath(nGame, szPath);
        HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return 0;

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
            return 0;
    }

    _stprintf(szImage, _T("%hs"), szCache);
    g_szSelTitleImage = szImage;
    return 1;
}

void CGameCollection::LaunchGame(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount || c_rgGames[nGame].m_szName[0] == 0)
        return;

    XAppGetD3DDev()->PersistDisplay();

    // The disc: D: is already the DVD drive.
    if (nGame == 0)
    {
        XLaunchNewImage("D:\\default.xbe", NULL);
        return;
    }

    // XLaunchNewImage keeps D:'s mapping across the reboot.
    CHAR szTarget [MAX_PATH];
    sprintf(szTarget, "\\Device\\Harddisk0\\Partition1\\Games\\%s", c_rgGames[nGame].m_szFolder);
    OBJECT_STRING dDrive = CONSTANT_OBJECT_STRING("\\??\\D:");
    OBJECT_STRING target;
    RtlInitObjectString(&target, szTarget);
    IoDeleteSymbolicLink(&dDrive);
    IoCreateSymbolicLink(&dDrive, &target);

    XLaunchNewImage("D:\\default.xbe", NULL);
}

////////////////////////////////////////////////////////////////////////////
// Installing the disc: the whole disc is copied to E:\Games\<title>, which
// GameCollection then lists and launches like any other installed game.

struct INSTALL
{
    HANDLE m_hThread;
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
    if (!CopyTree("CDROM0:", szDest, pbBuf, cbBuf))
    {
        c_install.m_bFailed = true;
        DeleteTree(szDest);
    }
    delete [] pbBuf;
    return 0;
}

// The installed copy of the disc in the tray, if there is one.
static int FindInstalledDisc()
{
    if (c_nGameCount == 0 || c_rgGames[0].m_szName[0] == 0)
        return -1;
    for (int i = 1; i < c_nGameCount; i += 1)
    {
        if (c_rgGames[i].m_dwTitleID == c_rgGames[0].m_dwTitleID &&
            c_rgGames[i].m_dwTimeDate == c_rgGames[0].m_dwTimeDate)
            return i;
    }
    return -1;
}

int CGameCollection::IsDiscInstalled()
{
    return FindInstalledDisc() >= 0;
}

int CGameCollection::StartInstall()
{
    if (c_install.m_hThread != NULL || c_nGameCount == 0 || c_rgGames[0].m_szName[0] == 0 || FindInstalledDisc() >= 0)
        return 0;

    // The folder is the game's name, kept to characters FATX allows.
    CHAR szName [43];
    int cch = 0;
    for (const WCHAR* pch = c_rgGames[0].m_szName; *pch != 0 && cch < 36; pch += 1)
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
    if (!SizeTree("CDROM0:", &c_install.m_qwTotal))
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
    return new CStrObject(_T("The game couldn't be installed. Check the disc and try again."));
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

    if (c_install.m_hThread == NULL)
        return;

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
    if (c_install.m_bNoSpace || c_install.m_bFailed)
        CallFunction(this, _T("OnInstallError"));
    else
        CallFunction(this, _T("OnInstallComplete"));
}
