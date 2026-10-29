// SysSettings: the console's own settings behind Frolick's Console
// Settings pages (sokky, 2026-10-25), the ones the Xbox's EEPROM doesn't
// hold: the TV's mode, the screen saver, which xbcompat runs games, a
// clock sync.  The Xbox's own settings (language, time zone, audio, the
// video flags games read) go through the dashboard's Config node.
//
// The Sion side is frolick-store (tools/frolickdash/store/settings.go),
// through two files (UTF-8, tab-separated, ending in a line "end"):
//
//   E:\FROLICK\SETTINGS\settings.txt   written here on every change:
//                  <key> <value>       display, keep, gpu, sync, screensaver
//   E:\FROLICK\STORE\system.txt        read here every second:
//                  gpu <0|1> <1 when xbcompat-gpu is installed>
//                  display <mode> <the modes the TV has: 480i,480p,...>
//                  sync <n> <none|running|ok|failed> <detail>
//                  info <label> <value>    (System Info)
//
//   DEF theSysSettings SysSettings { }
//
// Functions:
//   GetValue(key)      a setting ("" when not set)
//   SetValue(key, v)   set it (settings.txt is rewritten)
//   GetSys(key.n)      field n of the helper's line key ("display.2": the
//                      TV's modes), "" until the helper has said
//   HasSys()           1 when the helper is writing system.txt
//   GetInfoCount()     System Info's lines
//   GetInfoLabel(i), GetInfoValue(i)
//   Relaunch()         start this program again (a new TV mode is set
//                      between titles)
// Not part of Microsoft's dashboard; added for Frolick (tools/frolickdash).

#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"
#include "store.h"

#define SET_MAX 16
#define SET_KEY 24
#define SET_VAL 96
#define SYS_MAX 48
#define SYS_LINE 192

static const CHAR c_szSettings[] = "E:\\FROLICK\\SETTINGS\\settings.txt";
static const CHAR c_szSystem[] = "E:\\FROLICK\\STORE\\system.txt";

class CSysSettings : public CNode
{
    DECLARE_NODE(CSysSettings, CNode)
public:
    CSysSettings();

    void Advance(float nSeconds);

    CStrObject* GetValue(const TCHAR* szKey);
    void SetValue(const TCHAR* szKey, const TCHAR* szValue);
    CStrObject* GetSys(const TCHAR* szKey);
    int HasSys();
    int GetInfoCount();
    CStrObject* GetInfoLabel(int i);
    CStrObject* GetInfoValue(int i);
    void Relaunch();

protected:
    void Load();
    void Save();
    void ReadSystem();
    int Find(const CHAR* szKey);
    const CHAR* InfoLine(int i);

    CHAR m_rgKey[SET_MAX][SET_KEY];
    CHAR m_rgVal[SET_MAX][SET_VAL];
    int m_nSet;
    CHAR m_rgSys[SYS_MAX][SYS_LINE];    // system.txt's lines, as read
    int m_nSys;
    float m_nWait;
    float m_nStale;
    FILETIME m_ftSystem;
    DWORD m_cbSystem;

    DECLARE_NODE_PROPS()
    DECLARE_NODE_FUNCTIONS()
};

IMPLEMENT_NODE("SysSettings", CSysSettings, CNode)

START_NODE_PROPS(CSysSettings, CNode)
END_NODE_PROPS()

START_NODE_FUN(CSysSettings, CNode)
    NODE_FUN_SS(GetValue)
    NODE_FUN_VSS(SetValue)
    NODE_FUN_SS(GetSys)
    NODE_FUN_IV(HasSys)
    NODE_FUN_IV(GetInfoCount)
    NODE_FUN_SI(GetInfoLabel)
    NODE_FUN_SI(GetInfoValue)
    NODE_FUN_VV(Relaunch)
END_NODE_FUN()

static void Copy(CHAR* dst, const CHAR* src, int n)
{
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

// The script's strings are ASCII here (keys, modes, numbers).
static void Narrow(CHAR* dst, const TCHAR* src, int n)
{
    int i = 0;
    for (; src && src[i] && i < n - 1; i++)
        dst[i] = (CHAR)src[i];
    dst[i] = 0;
}

CSysSettings::CSysSettings() : m_nSet(0), m_nSys(0), m_nWait(0.0f), m_nStale(0.0f), m_cbSystem(0)
{
    ZeroMemory(&m_ftSystem, sizeof m_ftSystem);
    CreateDirectoryA("E:\\FROLICK", NULL);
    CreateDirectoryA("E:\\FROLICK\\SETTINGS", NULL);
    Load();
    ReadSystem();
}

int CSysSettings::Find(const CHAR* szKey)
{
    for (int i = 0; i < m_nSet; i++)
        if (strcmp(m_rgKey[i], szKey) == 0)
            return i;
    return -1;
}

void CSysSettings::Load()
{
    CHAR* p = StoreReadFile(c_szSettings, NULL);
    if (p == NULL)
        return;
    CHAR* line = p;
    while (line && *line)
    {
        CHAR* next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        int n = strlen(line);
        if (n > 0 && line[n - 1] == '\r')
            line[n - 1] = 0;
        CHAR* f[2];
        if (StoreSplit(line, f, 2) == 2 && m_nSet < SET_MAX && Find(f[0]) < 0)
        {
            Copy(m_rgKey[m_nSet], f[0], SET_KEY);
            Copy(m_rgVal[m_nSet], f[1], SET_VAL);
            m_nSet++;
        }
        line = next;
    }
    delete [] p;
}

static void Put(HANDLE h, const CHAR* sz)
{
    DWORD cb;
    WriteFile(h, sz, strlen(sz), &cb, NULL);
}

void CSysSettings::Save()
{
    HANDLE h = CreateFileA(c_szSettings, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    CHAR sz[SET_KEY + SET_VAL + 4];
    Put(h, "frolick-settings 1\n");
    for (int i = 0; i < m_nSet; i++)
    {
        _snprintf(sz, sizeof sz - 1, "%s\t%s\n", m_rgKey[i], m_rgVal[i]);
        sz[sizeof sz - 1] = 0;
        Put(h, sz);
    }
    Put(h, "end\n");
    CloseHandle(h);
}

CStrObject* CSysSettings::GetValue(const TCHAR* szKey)
{
    CHAR sz[SET_KEY];
    Narrow(sz, szKey, SET_KEY);
    int i = Find(sz);
    return StoreStr(i < 0 ? "" : m_rgVal[i]);
}

void CSysSettings::SetValue(const TCHAR* szKey, const TCHAR* szValue)
{
    CHAR szK[SET_KEY], szV[SET_VAL];
    Narrow(szK, szKey, SET_KEY);
    Narrow(szV, szValue, SET_VAL);
    int i = Find(szK);
    if (i < 0)
    {
        if (m_nSet >= SET_MAX)
            return;
        i = m_nSet++;
        Copy(m_rgKey[i], szK, SET_KEY);
    }
    else if (strcmp(m_rgVal[i], szV) == 0)
        return;
    Copy(m_rgVal[i], szV, SET_VAL);
    Save();
}

// ---- the helper's side ----------------------------------------------------

void CSysSettings::Advance(float nSeconds)
{
    CNode::Advance(nSeconds);
    m_nWait -= nSeconds;
    if (m_nWait > 0.0f)
        return;
    m_nWait = 1.0f;
    FILETIME ft;
    DWORD cb;
    if (!StoreStat(c_szSystem, &ft, &cb))
    {
        m_nSys = 0;
        return;
    }
    if (CompareFileTime(&ft, &m_ftSystem) == 0 && cb == m_cbSystem)
    {
        // the helper rewrites it when anything changes, and the uptime
        // every half minute: a minute and a half without, it is gone
        m_nStale += 1.0f;
        if (m_nStale > 90.0f)
            m_nSys = 0;
        return;
    }
    m_nStale = 0.0f;
    m_ftSystem = ft;
    m_cbSystem = cb;
    ReadSystem();
}

void CSysSettings::ReadSystem()
{
    CHAR* p = StoreReadFile(c_szSystem, NULL);
    if (p == NULL)
        return;
    if (strstr(p, "\nend") == NULL)
    {
        delete [] p;        // half written: the last one stands
        return;
    }
    m_nSys = 0;
    CHAR* line = p;
    while (line && *line && m_nSys < SYS_MAX)
    {
        CHAR* next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        int n = strlen(line);
        if (n > 0 && line[n - 1] == '\r')
            line[n - 1] = 0;
        if (strcmp(line, "end") == 0)
            break;
        if (strchr(line, '\t'))
            Copy(m_rgSys[m_nSys++], line, SYS_LINE);
        line = next;
    }
    delete [] p;
}

int CSysSettings::HasSys()
{
    return m_nSys > 0 ? 1 : 0;
}

// "display.2": field 2 of the line "display ...", the key being field 0.
CStrObject* CSysSettings::GetSys(const TCHAR* szKey)
{
    CHAR szK[SET_KEY], sz[SYS_LINE];
    Narrow(szK, szKey, SET_KEY);
    int nField = 1;
    CHAR* pDot = strchr(szK, '.');
    if (pDot)
    {
        *pDot = 0;
        nField = atoi(pDot + 1);
    }
    for (int i = 0; i < m_nSys; i++)
    {
        Copy(sz, m_rgSys[i], SYS_LINE);
        CHAR* f[8];
        int k = StoreSplit(sz, f, 8);
        if (strcmp(f[0], szK) == 0)
            return StoreStr(nField < k ? f[nField] : "");
    }
    return StoreStr("");
}

const CHAR* CSysSettings::InfoLine(int i)
{
    for (int k = 0; k < m_nSys; k++)
    {
        if (strncmp(m_rgSys[k], "info\t", 5) != 0)
            continue;
        if (i-- == 0)
            return m_rgSys[k];
    }
    return NULL;
}

int CSysSettings::GetInfoCount()
{
    int n = 0;
    while (InfoLine(n))
        n++;
    return n;
}

CStrObject* CSysSettings::GetInfoLabel(int i)
{
    CHAR sz[SYS_LINE];
    const CHAR* p = InfoLine(i);
    if (p == NULL)
        return StoreStr("");
    Copy(sz, p, SYS_LINE);
    CHAR* f[3];
    return StoreStr(StoreSplit(sz, f, 3) == 3 ? f[1] : "");
}

CStrObject* CSysSettings::GetInfoValue(int i)
{
    CHAR sz[SYS_LINE];
    const CHAR* p = InfoLine(i);
    if (p == NULL)
        return StoreStr("");
    Copy(sz, p, SYS_LINE);
    CHAR* f[3];
    return StoreStr(StoreSplit(sz, f, 3) == 3 ? f[2] : "");
}

// This program again: the dashboard build is the dashboard (back to it),
// the game build is D:\default.xbe beside its files.
void CSysSettings::Relaunch()
{
    DWORD dwCert = *(DWORD*)0x10118;
    if (*(DWORD*)(dwCert + 8) == 0xFFFE0000)
        XLaunchNewImage(NULL, NULL);
    else
        XLaunchNewImage("d:\\default.xbe", NULL);
}
