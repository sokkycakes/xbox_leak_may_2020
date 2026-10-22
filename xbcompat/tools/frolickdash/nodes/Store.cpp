// Store: Frolick's storefront catalog, from the Internet Archive.
//
// The catalog is built on the Sion by frolick-store (tools/frolickdash/
// store), which does the archive.org work Frolick can't (no TLS on this
// side), and lands here as E:\FROLICK\STORE\catalog.txt (UTF-8, tab-
// separated, ending in a line "end"):
//
//   tab   <t> <name>
//   item  <t> <identifier> <bytes> <kind> <tile> <title> <description>
//
// Tiles are PNGs in E:\FROLICK\STORE\img.  The file is read again when it
// changes (checked every two seconds), and GetVersion() moves on so the
// page can refill itself.  Spotlight's tab row is laid out at build time
// (genpage.py SPOTLIGHT), so the helper's tabs are matched by number.
//
//   DEF theStore Store { }
//
//   GetCount(t)       items in tab t
//   GetName(t, i)     the item's title
//   GetKind(t, i)     its row's second line ("Demo", "Shareware", ...)
//   GetDesc(t, i)     its description, one paragraph
//   GetSize(t, i)     "412 MB"
//   GetImage(t, i)    its tile's path, or ""
//   GetId(t, i)       its archive.org identifier
//   GetVersion()      changes whenever the catalog is read again
//   GetState()        "" with a catalog, else what the page says instead
// Not part of Microsoft's dashboard; added for Frolick (tools/frolickdash).

#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"
#include "store.h"

static const CHAR c_szCatalog[] = "E:\\FROLICK\\STORE\\catalog.txt";

static CHAR* c_pCatalog;            // the file, split in place
static STOREITEM c_rgItems[STORE_MAX];
static int c_nItems;
static FILETIME c_ftCatalog;
static DWORD c_cbCatalog;

int StoreUtf16(WCHAR* out, int n, const CHAR* s)
{
    int i = 0;
    const BYTE* p = (const BYTE*)s;
    while (*p && i < n - 1)
    {
        unsigned c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 0xF) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
        if (c >= 0x10000) c = '?';
        out[i++] = (WCHAR)c;
    }
    out[i] = 0;
    return i;
}

CStrObject* StoreStr(const CHAR* s)
{
    static WCHAR wsz[1024];
    StoreUtf16(wsz, 1024, s ? s : "");
    return new CStrObject(wsz);
}

// A whole file, NUL-terminated (free it), or NULL.
CHAR* StoreReadFile(const CHAR* szPath, DWORD* pcb)
{
    HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return NULL;
    DWORD cb = GetFileSize(hFile, NULL), cbRead = 0;
    CHAR* p = NULL;
    if (cb != 0xFFFFFFFF && cb < 4 * 1024 * 1024)
    {
        p = new CHAR[cb + 1];
        if (!ReadFile(hFile, p, cb, &cbRead, NULL) || cbRead != cb)
        {
            delete [] p;
            p = NULL;
        }
        else
            p[cb] = 0;
    }
    CloseHandle(hFile);
    if (pcb)
        *pcb = cb;
    return p;
}

// Splits s at tabs into at most n fields; returns the count.
int StoreSplit(CHAR* s, CHAR** rg, int n)
{
    int k = 0;
    rg[k++] = s;
    for (; *s && k < n; s++)
    {
        if (*s == '\t')
        {
            *s = 0;
            rg[k++] = s + 1;
        }
    }
    return k;
}

// The file's write time and size, to tell when it changed.
bool StoreStat(const CHAR* szPath, FILETIME* pft, DWORD* pcb)
{
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(szPath, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FindClose(h);
    *pft = fd.ftLastWriteTime;
    *pcb = fd.nFileSizeLow;
    return true;
}

static bool LoadCatalog()
{
    DWORD cb;
    CHAR* p = StoreReadFile(c_szCatalog, &cb);
    if (p == NULL)
        return false;
    // a catalog the helper has finished writing ends in "end"
    if (strncmp(p, "frolick-catalog", 15) != 0 || strstr(p, "\nend") == NULL)
    {
        delete [] p;
        return false;
    }
    delete [] c_pCatalog;
    c_pCatalog = p;
    c_nItems = 0;
    CHAR* line = p;
    while (line && *line)
    {
        CHAR* next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        int n = strlen(line);
        if (n > 0 && line[n - 1] == '\r')
            line[n - 1] = 0;
        CHAR* f[8];
        if (strncmp(line, "item\t", 5) == 0 && c_nItems < STORE_MAX && StoreSplit(line, f, 8) == 8)
        {
            STOREITEM* it = &c_rgItems[c_nItems++];
            it->nTab = atoi(f[1]);
            it->szId = f[2];
            it->qwBytes = _atoi64(f[3]);
            it->nMB = (int)(it->qwBytes / (1024 * 1024));
            if (it->nMB == 0 && f[3][0] != '0')
                it->nMB = 1;
            it->szKind = f[4];
            it->szTile = f[5];
            it->szTitle = f[6];
            it->szDesc = f[7];
        }
        line = next;
    }
    return true;
}

const STOREITEM* StoreItem(int t, int i)
{
    int k = 0;
    for (int j = 0; j < c_nItems; j++)
    {
        if (c_rgItems[j].nTab != t)
            continue;
        if (k == i)
            return &c_rgItems[j];
        k++;
    }
    return NULL;
}

class CStore : public CNode
{
    DECLARE_NODE(CStore, CNode)
public:
    CStore();

    void Advance(float nSeconds);

    int GetCount(int t);
    CStrObject* GetName(int t, int i);
    CStrObject* GetKind(int t, int i);
    CStrObject* GetDesc(int t, int i);
    CStrObject* GetSize(int t, int i);
    CStrObject* GetImage(int t, int i);
    CStrObject* GetId(int t, int i);
    int GetVersion();
    CStrObject* GetState();

protected:
    void Check();

    float m_nWait;
    int m_nVersion;

    DECLARE_NODE_PROPS()
    DECLARE_NODE_FUNCTIONS()
};

IMPLEMENT_NODE("Store", CStore, CNode)

START_NODE_PROPS(CStore, CNode)
END_NODE_PROPS()

START_NODE_FUN(CStore, CNode)
    NODE_FUN_II(GetCount)
    NODE_FUN_SII(GetName)
    NODE_FUN_SII(GetKind)
    NODE_FUN_SII(GetDesc)
    NODE_FUN_SII(GetSize)
    NODE_FUN_SII(GetImage)
    NODE_FUN_SII(GetId)
    NODE_FUN_IV(GetVersion)
    NODE_FUN_SV(GetState)
END_NODE_FUN()

CStore::CStore() : m_nWait(0.0f), m_nVersion(0)
{
    Check();
}

void CStore::Check()
{
    FILETIME ft;
    DWORD cb;
    if (!StoreStat(c_szCatalog, &ft, &cb))
        return;
    if (c_pCatalog && CompareFileTime(&ft, &c_ftCatalog) == 0 && cb == c_cbCatalog)
        return;
    if (LoadCatalog())
    {
        c_ftCatalog = ft;
        c_cbCatalog = cb;
        m_nVersion++;
    }
}

void CStore::Advance(float nSeconds)
{
    CNode::Advance(nSeconds);
    m_nWait += nSeconds;
    if (m_nWait < 2.0f)
        return;
    m_nWait = 0.0f;
    Check();
}

int CStore::GetCount(int t)
{
    int n = 0;
    for (int j = 0; j < c_nItems; j++)
        if (c_rgItems[j].nTab == t)
            n++;
    return n;
}

CStrObject* CStore::GetName(int t, int i)
{
    const STOREITEM* it = StoreItem(t, i);
    return StoreStr(it ? it->szTitle : "");
}

CStrObject* CStore::GetKind(int t, int i)
{
    const STOREITEM* it = StoreItem(t, i);
    return StoreStr(it ? it->szKind : "");
}

CStrObject* CStore::GetDesc(int t, int i)
{
    const STOREITEM* it = StoreItem(t, i);
    return StoreStr(it ? it->szDesc : "");
}

CStrObject* CStore::GetSize(int t, int i)
{
    const STOREITEM* it = StoreItem(t, i);
    CHAR sz[32] = "";
    if (it)
        StoreFormatMB(sz, it->nMB);
    return StoreStr(sz);
}

CStrObject* CStore::GetImage(int t, int i)
{
    const STOREITEM* it = StoreItem(t, i);
    CHAR sz[64] = "";
    if (it && it->szTile[0])
        _snprintf(sz, sizeof sz - 1, "E:\\FROLICK\\STORE\\img\\%s", it->szTile);
    return StoreStr(sz);
}

CStrObject* CStore::GetId(int t, int i)
{
    const STOREITEM* it = StoreItem(t, i);
    return StoreStr(it ? it->szId : "");
}

int CStore::GetVersion()
{
    return m_nVersion;
}

CStrObject* CStore::GetState()
{
    if (c_pCatalog)
        return new CStrObject;
    return StoreStr(StoreServiceState());
}

// "412 MB", "1.4 GB"
void StoreFormatMB(CHAR* sz, int nMB)
{
    if (nMB >= 1024)
        sprintf(sz, "%d.%d GB", nMB / 1024, (nMB % 1024) * 10 / 1024);
    else
        sprintf(sz, "%d MB", nMB);
}
