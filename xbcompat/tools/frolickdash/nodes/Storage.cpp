// Storage: the console's saved-game data for Frolick's Memory pages, the
// way the dashboard's Memory screen keeps it.  The data and the file work
// are the engine's own: CTitleArray (TitleCollection.cpp, the titles under
// UDATA/TDATA of a device, their saves, downloads and installed games),
// CGameCopier (CopyGames.cpp, the copier the Memory screen uses, which also
// deletes), CSettingsFile for the saves' SaveMeta.xbx, the MemoryMonitor
// node for the memory units.  This node only hands that data to script, a
// row at a time, and drives the copy and delete the way SavedGameGrid.cpp
// does (its thread and its events), without SavedGameGrid's pod geometry.
//
//   DEF theStorage Storage
//   {
//       function OnCopyProgress() { }   // copyProgress changed
//       function OnCopyComplete() { }
//       function OnCopyError() { }
//       function OnDeleteEnd() { }
//   }
//
// Properties: device (a device unit: 8 the hard disk, 0..7 the memory
// units, as MemoryMonitor numbers them), title (an index into the device's
// titles), save (an index into the title's saves, -1 the whole title),
// copyProgress (0..1 while a copy runs).  Functions:
//   IsDeviceReady(dev)   the device is mounted
//   GetDeviceName(dev)   "Hard Drive", or the memory unit's name
//   GetDeviceSpace(dev)  "12,345 blocks free of 123,456"
//   GetDeviceFill(dev)   the used fraction, for a gauge
//   Refresh()            scan the device's titles; returns their count
//   GetTitleCount(), GetTitleName(i), GetTitleID(i), GetTitleInfo(i)
//   GetTitleImage(i)     the title's TitleImage.xbx, or ""
//   GetSaveCount(i)
//   GetSaveName(s), GetSaveInfo(s), GetSaveImage(s), GetSaveKind(s)
//                        of the current title (kind: 0 save, 1 download,
//                        2 installed game)
//   CanCopy(s)           the save may be copied to another device
//   SaveExists(dev)      the current save already exists on device dev
//   StartCopy(dev)       copy the current save to device dev
//   StartDelete()        delete the current save (or the whole title)
//   IsBusy()             a copy or delete is running
// Not part of Microsoft's dashboard; added for Frolick (tools/frolickdash).

#include "std.h"
#include "xapp.h"
#include "FileUtil.h"
#include "node.h"
#include "runner.h"
#include "Locale.h"
#include "SettingsFile.h"
#include "TitleCollection.h"
#include "CopyGames.h"

#ifndef MAX_MUNAME
#define MAX_MUNAME 32
#endif

extern CGameCopier theGameCopier;               // SavedGameGrid.cpp
extern void FormatDeviceName(int devUnit, TCHAR* szBuf);    // Memory.cpp

class CStorage : public CNode
{
    DECLARE_NODE(CStorage, CNode)
public:
    CStorage();
    ~CStorage();

    void Advance(float nSeconds);

    int IsDeviceReady(int nDev);
    CStrObject* GetDeviceName(int nDev);
    CStrObject* GetDeviceSpace(int nDev);
    float GetDeviceFill(int nDev);

    int Refresh();
    int GetTitleCount();
    CStrObject* GetTitleName(int nTitle);
    CStrObject* GetTitleID(int nTitle);
    CStrObject* GetTitleInfo(int nTitle);
    CStrObject* GetTitleImage(int nTitle);
    int GetSaveCount(int nTitle);

    CStrObject* GetSaveName(int nSave);
    CStrObject* GetSaveInfo(int nSave);
    CStrObject* GetSaveImage(int nSave);
    int GetSaveKind(int nSave);
    int CanCopy(int nSave);
    int SaveExists(int nDev);
    void StartCopy(int nDev);
    void StartDelete();
    int IsBusy();

    int m_device;
    int m_title;
    int m_save;
    float m_copyProgress;

protected:
    bool DevOK(int nDev) { return nDev >= 0 && nDev <= 8 && g_titles[nDev].IsValid() && !g_titles[nDev].IsDirty(); }
    bool TitleOK(int nTitle) { return DevOK(m_device) && nTitle >= 0 && nTitle < g_titles[m_device].GetTitleCount(); }
    bool SaveOK(int nSave) { return TitleOK(m_title) && nSave >= 0 && nSave < g_titles[m_device].GetSavedGameCount(m_title); }
    bool GetSavePath(TCHAR* szPath, int nSave);
    bool GetSpace(int nDev, int* pnTotal, int* pnFree);

    static void WINAPI DeleteThread(CStorage* p);

    bool m_bCopying;
    bool m_bDeleting;
    HANDLE m_hDeleteThread;
    XTIME m_timeCopyStart;

    DECLARE_NODE_PROPS()
    DECLARE_NODE_FUNCTIONS()
};

IMPLEMENT_NODE("Storage", CStorage, CNode)

START_NODE_PROPS(CStorage, CNode)
    NODE_PROP(pt_integer, CStorage, device)
    NODE_PROP(pt_integer, CStorage, title)
    NODE_PROP(pt_integer, CStorage, save)
    NODE_PROP(pt_number, CStorage, copyProgress)
END_NODE_PROPS()

START_NODE_FUN(CStorage, CNode)
    NODE_FUN_II(IsDeviceReady)
    NODE_FUN_SI(GetDeviceName)
    NODE_FUN_SI(GetDeviceSpace)
    NODE_FUN_NI(GetDeviceFill)
    NODE_FUN_IV(Refresh)
    NODE_FUN_IV(GetTitleCount)
    NODE_FUN_SI(GetTitleName)
    NODE_FUN_SI(GetTitleID)
    NODE_FUN_SI(GetTitleInfo)
    NODE_FUN_SI(GetTitleImage)
    NODE_FUN_II(GetSaveCount)
    NODE_FUN_SI(GetSaveName)
    NODE_FUN_SI(GetSaveInfo)
    NODE_FUN_SI(GetSaveImage)
    NODE_FUN_II(GetSaveKind)
    NODE_FUN_II(CanCopy)
    NODE_FUN_II(SaveExists)
    NODE_FUN_VI(StartCopy)
    NODE_FUN_VV(StartDelete)
    NODE_FUN_IV(IsBusy)
END_NODE_FUN()

CStorage::CStorage() :
    m_device(Dev0), m_title(-1), m_save(-1), m_copyProgress(0.0f),
    m_bCopying(false), m_bDeleting(false), m_hDeleteThread(NULL), m_timeCopyStart(0.0f)
{
}

CStorage::~CStorage()
{
    if (m_hDeleteThread != NULL)
    {
        WaitForSingleObject(m_hDeleteThread, INFINITE);
        CloseHandle(m_hDeleteThread);
    }
}

// The copy and delete end on the frame after they finish, as the Memory
// screen's grid reports them.
void CStorage::Advance(float nSeconds)
{
    CNode::Advance(nSeconds);

    if (m_bCopying)
    {
        if (m_copyProgress != theGameCopier.m_progress)
        {
            m_copyProgress = theGameCopier.m_progress;
            CallFunction(this, _T("OnCopyProgress"));
        }
        if (theGameCopier.m_error)
        {
            m_bCopying = false;
            theGameCopier.Finish();
            CallFunction(this, _T("OnCopyError"));
        }
        else if (theGameCopier.m_done && XAppGetNow() >= m_timeCopyStart + 0.5f)
        {
            m_bCopying = false;
            theGameCopier.Finish();
            m_copyProgress = 1.0f;
            CallFunction(this, _T("OnCopyComplete"));
        }
        return;
    }

    if (m_bDeleting && WaitForSingleObject(m_hDeleteThread, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(m_hDeleteThread);
        m_hDeleteThread = NULL;
        m_bDeleting = false;
        CallFunction(this, _T("OnDeleteEnd"));
    }
}

////////////////////////////////////////////////////////////////////////////
// Devices

int CStorage::IsDeviceReady(int nDev)
{
    return nDev >= 0 && nDev <= 8 && g_titles[nDev].IsValid();
}

CStrObject* CStorage::GetDeviceName(int nDev)
{
    if (nDev == Dev0)
        return new CStrObject(_T("Hard Drive"));
    TCHAR sz [MAX_MUNAME + 16];
    sz[0] = 0;
    if (nDev >= 0 && nDev < Dev0)
        FormatDeviceName(nDev, sz);
    if (sz[0] == 0)
        _stprintf(sz, _T("Memory Unit %d%c"), nDev / 2 + 1, nDev % 2 == 0 ? 'A' : 'B');
    return new CStrObject(sz);
}

// The device's root (C: holds UDATA and TDATA on the hard disk; a memory
// unit is its mounted drive).
bool CStorage::GetSpace(int nDev, int* pnTotal, int* pnFree)
{
    *pnTotal = *pnFree = 0;
    if (!IsDeviceReady(nDev))
        return false;
    CHAR szRoot [4];    // (the Xbox API takes ANSI paths)
    szRoot[0] = nDev == Dev0 ? 'C' : (CHAR)g_titles[nDev].GetUData()[0];
    szRoot[1] = ':'; szRoot[2] = '\\'; szRoot[3] = 0;
    if (szRoot[0] == 0)
        return false;
    ULARGE_INTEGER qwAvail, qwTotal, qwFree;
    if (!GetDiskFreeSpaceExA(szRoot, &qwAvail, &qwTotal, &qwFree))
        return false;
    *pnTotal = (int)((qwTotal.QuadPart + BLOCK_SIZE - 1) / BLOCK_SIZE) - 1;
    *pnFree = (int)((qwFree.QuadPart + BLOCK_SIZE - 1) / BLOCK_SIZE);
    return true;
}

CStrObject* CStorage::GetDeviceSpace(int nDev)
{
    int nTotal, nFree;
    if (!GetSpace(nDev, &nTotal, &nFree))
        return new CStrObject;
    TCHAR szFree [16], szTotal [16], sz [64];
    FormatBlocks(szFree, nFree);
    FormatBlocks(szTotal, nTotal);
    _stprintf(sz, _T("%s blocks free of %s"), szFree, szTotal);
    return new CStrObject(sz);
}

float CStorage::GetDeviceFill(int nDev)
{
    int nTotal, nFree;
    if (!GetSpace(nDev, &nTotal, &nFree) || nTotal <= 0)
        return 0.0f;
    float f = 1.0f - (float)nFree / (float)nTotal;
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

////////////////////////////////////////////////////////////////////////////
// Titles

int CStorage::Refresh()
{
    if (m_device < 0 || m_device > 8 || !g_titles[m_device].IsValid())
        return 0;
    if (g_titles[m_device].IsDirty())
        g_titles[m_device].Update();
    return g_titles[m_device].GetTitleCount();
}

int CStorage::GetTitleCount()
{
    return DevOK(m_device) ? g_titles[m_device].GetTitleCount() : 0;
}

CStrObject* CStorage::GetTitleName(int nTitle)
{
    if (!TitleOK(nTitle))
        return new CStrObject;
    const TCHAR* sz = g_titles[m_device].GetTitleName(nTitle);
    return new CStrObject(sz != NULL && sz[0] ? sz : g_titles[m_device].GetTitleID(nTitle));
}

CStrObject* CStorage::GetTitleID(int nTitle)
{
    return new CStrObject(TitleOK(nTitle) ? g_titles[m_device].GetTitleID(nTitle) : _T(""));
}

int CStorage::GetSaveCount(int nTitle)
{
    return TitleOK(nTitle) ? g_titles[m_device].GetSavedGameCount(nTitle) : 0;
}

// "3 saved games, 1,234 blocks" (downloads and installed games counted
// apart, as the Memory screen's "N games, N saves")
CStrObject* CStorage::GetTitleInfo(int nTitle)
{
    if (!TitleOK(nTitle))
        return new CStrObject;
    int nItems = g_titles[m_device].GetSavedGameCount(nTitle), nSaves = 0, nGames = 0, i;
    for (i = 0; i < nItems; i++)
    {
        if (g_titles[m_device].GetSavedGameKind(nTitle, i) != 0)
            nGames += 1;
        else
            nSaves += 1;
    }
    TCHAR szBlocks [16], sz [128], szPart [48];
    FormatBlocks(szBlocks, g_titles[m_device].GetTitleTotalBlocks(nTitle, NULL));
    sz[0] = 0;
    if (nGames > 0)
    {
        _stprintf(szPart, _T("%d game%s"), nGames, nGames == 1 ? _T("") : _T("s"));
        _tcscat(sz, szPart);
    }
    if (nSaves > 0 || nGames == 0)
    {
        if (sz[0]) _tcscat(sz, _T(", "));
        _stprintf(szPart, _T("%d saved game%s"), nSaves, nSaves == 1 ? _T("") : _T("s"));
        _tcscat(sz, szPart);
    }
    _stprintf(szPart, _T(", %s blocks"), szBlocks);
    _tcscat(sz, szPart);
    return new CStrObject(sz);
}

// Pages show their own placeholder for an empty path, so the dashboard's
// xboxlogo fallbacks (not in Frolick's XIP) come back empty.
static bool IsXboxLogo(const TCHAR* sz)
{
    return _tcsnicmp(sz, _T("xboxlogo"), 8) == 0;
}

CStrObject* CStorage::GetTitleImage(int nTitle)
{
    if (!TitleOK(nTitle))
        return new CStrObject;
    TCHAR szPath [MAX_PATH];
    MakePath(szPath, g_titles[m_device].GetUData(), g_titles[m_device].GetTitleID(nTitle));
    MakePath(szPath, szPath, szTitleImageXBX);
    if (DoesFileExist(szPath))
        return new CStrObject(szPath);
    // a title with only downloads or installed games: the first one's image
    int nItems = g_titles[m_device].GetSavedGameCount(nTitle);
    for (int i = 0; i < nItems; i++)
    {
        if (g_titles[m_device].GetSavedGameKind(nTitle, i) != 0)
        {
            g_titles[m_device].GetSavedGameImageName(nTitle, i, szPath);
            if (szPath[0] && !IsXboxLogo(szPath))
                return new CStrObject(szPath);
        }
    }
    return new CStrObject;
}

////////////////////////////////////////////////////////////////////////////
// Saves of the current title

bool CStorage::GetSavePath(TCHAR* szPath, int nSave)
{
    return SaveOK(nSave) && g_titles[m_device].GetSavedGamePath(m_title, nSave, szPath);
}

int CStorage::GetSaveKind(int nSave)
{
    if (!SaveOK(nSave))
        return 0;
    DWORD dwKind = g_titles[m_device].GetSavedGameKind(m_title, nSave);
    return (dwKind & SAVEFLAG_GAME) ? 2 : (dwKind & SAVEFLAG_CONTENT) ? 1 : 0;
}

// The save's name from its SaveMeta.xbx (the Memory screen's
// FormatSavedGameName), a download's or installed game's own name.
CStrObject* CStorage::GetSaveName(int nSave)
{
    if (!SaveOK(nSave))
        return new CStrObject;
    TCHAR szName [64];
    if (g_titles[m_device].GetSavedGameKind(m_title, nSave) != 0)
    {
        if (!g_titles[m_device].GetSavedGameName(m_title, nSave, szName, countof(szName)))
            szName[0] = 0;
        return new CStrObject(szName);
    }
    TCHAR szPath [MAX_PATH];
    if (!GetSavePath(szPath, nSave))
        return new CStrObject;
    MakePath(szPath, szPath, szSaveDataXBX);
    TCHAR szLangCode [MAX_LANGUAGE_CODE_LEN];
    CSettingsFile settings;
    if (!settings.Open(szPath) || !settings.GetValue(GetLanguageCode(szLangCode), _T("Name"), szName, countof(szName)))
        return new CStrObject(_T("Broken Save"));
    return new CStrObject(szName);
}

// "10/20/2026 8:05 AM, 12 blocks"
CStrObject* CStorage::GetSaveInfo(int nSave)
{
    TCHAR szPath [MAX_PATH];
    if (!GetSavePath(szPath, nSave))
        return new CStrObject;
    TCHAR szTime [48], szBlocks [16], sz [96];
    szTime[0] = 0;
    FILETIME ft = g_titles[m_device].GetSavedGameTime(m_title, nSave), lft;
    SYSTEMTIME st;
    if ((ft.dwLowDateTime != 0 || ft.dwHighDateTime != 0) && FileTimeToLocalFileTime(&ft, &lft) && FileTimeToSystemTime(&lft, &st))
        FormatTime(szTime, countof(szTime), &st);
    FormatBlocks(szBlocks, GetDirectoryBlocks(szPath, BLOCK_SIZE, true, NULL));
    if (szTime[0])
        _stprintf(sz, _T("%s, %s blocks"), szTime, szBlocks);
    else
        _stprintf(sz, _T("%s blocks"), szBlocks);
    return new CStrObject(sz);
}

CStrObject* CStorage::GetSaveImage(int nSave)
{
    if (!SaveOK(nSave))
        return new CStrObject;
    TCHAR szPath [MAX_PATH];
    g_titles[m_device].GetSavedGameImageName(m_title, nSave, szPath);
    if (szPath[0] == 0 || IsXboxLogo(szPath))
        return new CStrObject;
    return new CStrObject(szPath);
}

// As the Memory screen's CanCopy: a plain save with a name, not marked
// NoCopy, in a title that isn't broken.
int CStorage::CanCopy(int nSave)
{
    if (!SaveOK(nSave) || g_titles[m_device].GetSavedGameKind(m_title, nSave) != 0 || g_titles[m_device].IsBroken(m_title))
        return 0;
    TCHAR szPath [MAX_PATH];
    if (!GetSavePath(szPath, nSave))
        return 0;
    MakePath(szPath, szPath, szSaveDataXBX);
    CSettingsFile settings;
    TCHAR szLangCode [MAX_LANGUAGE_CODE_LEN], szValue [64];
    if (!settings.Open(szPath) || !settings.GetValue(GetLanguageCode(szLangCode), _T("Name"), szValue, countof(szValue)))
        return 0;
    if (!settings.GetValue(GetLanguageCode(szLangCode), _T("NoCopy"), szValue, countof(szValue)))
        return 1;
    return _ttoi(szValue) == 0;
}

int CStorage::SaveExists(int nDev)
{
    if (!SaveOK(m_save) || !IsDeviceReady(nDev))
        return 0;
    TCHAR szPath [MAX_PATH];
    MakePath(szPath, g_titles[nDev].GetUData(), g_titles[m_device].GetTitleID(m_title));
    MakePath(szPath, szPath, g_titles[m_device].GetSavedGameID(m_title, m_save));
    return DoesFileExist(szPath);
}

// The Memory screen's StartCopy: the save's blocks plus the title's
// TitleMeta, TitleImage and SaveImage when the destination lacks them.
void CStorage::StartCopy(int nDev)
{
    if (m_bCopying || m_bDeleting || !SaveOK(m_save) || !IsDeviceReady(nDev) || nDev == m_device)
    {
        CallFunction(this, _T("OnCopyError"));
        return;
    }
    TCHAR szBuf [MAX_PATH];
    const TCHAR* szTitleID = g_titles[m_device].GetTitleID(m_title);
    VERIFY(GetSavePath(szBuf, m_save));
    int nBlocks = GetDirectoryBlocks(szBuf, BLOCK_SIZE, true, NULL);
    static const TCHAR* const rgszShared[] = { szTitleDataXBX, szTitleImageXBX, szSaveImageXBX };
    for (int i = 0; i < 3; i++)
    {
        MakePath(szBuf, g_titles[nDev].GetUData(), szTitleID);
        MakePath(szBuf, szBuf, rgszShared[i]);
        if (!DoesFileExist(szBuf))
        {
            MakePath(szBuf, g_titles[m_device].GetUData(), szTitleID);
            MakePath(szBuf, szBuf, rgszShared[i]);
            nBlocks += GetFileBlocks(szBuf, BLOCK_SIZE);
        }
    }

    m_bCopying = true;
    m_timeCopyStart = XAppGetNow();
    m_copyProgress = 0.0f;
    theGameCopier.SetSource(m_device);
    theGameCopier.SetDestination(nDev);
    theGameCopier.AddGame(szTitleID, g_titles[m_device].GetSavedGameID(m_title, m_save),
                          g_titles[m_device].GetSavedGameTime(m_title, m_save), nBlocks);
    theGameCopier.Start();
}

static DWORD ParseTitleID(const TCHAR* szID)
{
    DWORD dw = 0;
    for (const TCHAR* pch = szID; *pch; pch++)
    {
        DWORD d;
        if (*pch >= '0' && *pch <= '9') d = *pch - '0';
        else if (*pch >= 'a' && *pch <= 'f') d = 10 + *pch - 'a';
        else if (*pch >= 'A' && *pch <= 'F') d = 10 + *pch - 'A';
        else return 0xffffffff;
        dw = (dw << 4) + d;
    }
    return dw;
}

// The Memory screen's DeleteThread: the whole title (its installed games,
// its UDATA and TDATA, its publisher's shared data when no other title
// uses it, its cache partition) or one save.
void WINAPI CStorage::DeleteThread(CStorage* p)
{
    CTitleArray& titles = g_titles[p->m_device];
    TCHAR szBuf [MAX_PATH];
    if (p->m_save < 0)
    {
        TCHAR szTitleID [16], szPublisherID [16];
        lstrcpyn(szTitleID, titles.GetTitleID(p->m_title), countof(szTitleID));
        lstrcpyn(szPublisherID, szTitleID, countof(szPublisherID));
        szPublisherID[4] = 0;

        int nItems = titles.GetSavedGameCount(p->m_title);
        for (int i = 0; i < nItems; i++)
        {
            if ((titles.GetSavedGameKind(p->m_title, i) & SAVEFLAG_GAME) != 0 && titles.GetSavedGamePath(p->m_title, i, szBuf))
                theGameCopier.DeleteDirectory(szBuf);
        }
        titles.RemoveTitle(p->m_title);
        if (!titles.IsPublisherExists(szPublisherID))
        {
            lstrcat(szPublisherID, _T("ffff"));
            MakePath(szBuf, titles.GetTData(), szPublisherID);
            theGameCopier.DeleteDirectory(szBuf);
        }
        MakePath(szBuf, titles.GetUData(), szTitleID);
        theGameCopier.DeleteDirectory(szBuf);
        if (p->m_device == Dev0)
        {
            MakePath(szBuf, titles.GetTData(), szTitleID);
            theGameCopier.DeleteDirectory(szBuf);
            XapiDeleteCachePartition(ParseTitleID(szTitleID));
        }
        p->m_title = -1;
    }
    else
    {
        if (titles.GetSavedGamePath(p->m_title, p->m_save, szBuf))
        {
            theGameCopier.m_error = false;
            theGameCopier.DeleteDirectory(szBuf);
        }
        titles.RemoveSavedGame(p->m_title, p->m_save);
        p->m_save = -1;
    }
}

void CStorage::StartDelete()
{
    if (m_bCopying || m_bDeleting || !TitleOK(m_title) || (m_save >= 0 && !SaveOK(m_save)))
    {
        CallFunction(this, _T("OnDeleteEnd"));
        return;
    }
    m_hDeleteThread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)DeleteThread, this, 0, 0);
    if (m_hDeleteThread == NULL)
    {
        DeleteThread(this);
        CallFunction(this, _T("OnDeleteEnd"));
        return;
    }
    m_bDeleting = true;
}

int CStorage::IsBusy()
{
    return m_bCopying || m_bDeleting;
}
