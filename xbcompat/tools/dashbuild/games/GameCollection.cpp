// GameCollection: the dashboard's Games area.  Lists the titles installed on
// the hard disk as E:\Games\<folder>\default.xbe (named from each XBE's
// certificate) and launches one by pointing D: at its folder.  Not part of
// Microsoft's dashboard; added by xbcompat's dashbuild.

#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"

#define MAX_GAMES 256

struct GAMEINFO
{
    CHAR m_szFolder[64];
    WCHAR m_szName[41];
    DWORD m_dwTitleID;
};

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
    CStrObject* GetGameName(int nGame);
    CStrObject* GetGameFolder(int nGame);
    CStrObject* GetGameTitleID(int nGame);
    void LaunchGame(int nGame);

    DECLARE_NODE_PROPS()
    DECLARE_NODE_FUNCTIONS()
};

IMPLEMENT_NODE("GameCollection", CGameCollection, CNode)

START_NODE_PROPS(CGameCollection, CNode)
END_NODE_PROPS()

START_NODE_FUN(CGameCollection, CNode)
    NODE_FUN_IV(Scan)
    NODE_FUN_IV(GetGameCount)
    NODE_FUN_SI(GetGameName)
    NODE_FUN_SI(GetGameFolder)
    NODE_FUN_SI(GetGameTitleID)
    NODE_FUN_VI(LaunchGame)
END_NODE_FUN()

CGameCollection::CGameCollection()
{
    // The retail dashboard does not map E: (the data partition); games live there.
    IoCreateSymbolicLink((POBJECT_STRING)&c_eDrive, (POBJECT_STRING)&c_ePath);
}

CGameCollection::~CGameCollection()
{
}

// Title name and ID from the XBE certificate; false if it is not an XBE.
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
        BYTE rgbCert[0x8C];
        if (SetFilePointer(hFile, dwCert, NULL, FILE_BEGIN) == dwCert &&
            ReadFile(hFile, rgbCert, sizeof (rgbCert), &cb, NULL) && cb == sizeof (rgbCert))
        {
            pGame->m_dwTitleID = *(DWORD*)(rgbCert + 0x08);
            CopyMemory(pGame->m_szName, rgbCert + 0x0C, 40 * sizeof (WCHAR));
            pGame->m_szName[40] = 0;
            bOK = true;
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
    c_nGameCount = 0;

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("E:\\Games\\*", &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return 0;

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
    qsort(c_rgGames, c_nGameCount, sizeof (GAMEINFO), CompareGames);
    return c_nGameCount;
}

int CGameCollection::GetGameCount()
{
    return c_nGameCount;
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

void CGameCollection::LaunchGame(int nGame)
{
    if (nGame < 0 || nGame >= c_nGameCount)
        return;

    // XLaunchNewImage keeps D:'s mapping across the reboot.
    CHAR szTarget [MAX_PATH];
    sprintf(szTarget, "\\Device\\Harddisk0\\Partition1\\Games\\%s", c_rgGames[nGame].m_szFolder);
    OBJECT_STRING dDrive = CONSTANT_OBJECT_STRING("\\??\\D:");
    OBJECT_STRING target;
    RtlInitObjectString(&target, szTarget);
    IoDeleteSymbolicLink(&dDrive);
    IoCreateSymbolicLink(&dDrive, &target);

    XAppGetD3DDev()->PersistDisplay();
    XLaunchNewImage("D:\\default.xbe", NULL);
}
