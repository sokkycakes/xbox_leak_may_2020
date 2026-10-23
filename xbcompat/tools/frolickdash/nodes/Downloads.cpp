// Downloads: the queue behind Frolick's Downloads page (catalog pad, Y),
// laid out as the NXE dashboard's Active Downloads: one active download
// on top, the pending queue under it (sokky, 2026-10-20).
//
// The downloading itself is done on the Sion by frolick-store
// (tools/frolickdash/store), from the Internet Archive.  This node owns
// the queue and tells the helper about it through files in
// E:\FROLICK\STORE (UTF-8, tab-separated, each ending in a line "end"):
//
//   queue.txt    written here on every change, the active download first:
//                  q <identifier> <go|pause> <bytes> <kind> <title>
//   status.txt   read here every second, written by the helper:
//                  online <0|1> <what the store says>
//                  active <identifier> <state> <done> <total> <detail>
//                  have <identifier> <bytes on disk>
//                  done <identifier>
//                  gone <identifier>   (its game was deleted: out of the
//                                       history, so it can be had again)
//                  stream <identifier> <starting|ready|error> <in> <bytes> <detail>
//                                      (the disc image being streamed)
//   history.txt  written here: what finished (Download History)
//                  h <identifier> <MB> <kind> <title>
//   play.txt     written here: the Streaming Download item to play now
//                  p <identifier>      (none when the dashboard starts)
//
// A finished item leaves the queue for the history and the next pending
// item starts, as a queue does.  The helper rewrites status.txt every
// second; when it stops changing, the service is taken to be down.
//
//   DEF theDownloads Downloads { }
//
// Items are listed active first (index 0 when HasActive() is 1), then
// the pending queue in order.  Functions:
//   GetCount()        items, the active one included
//   HasActive()       1 when item 0 is the active download (downloading
//                     or paused), 0 when the queue only pends
//   GetName(i)        the item's title
//   GetKind(i)        what it is ("Demo", "Shareware", ...)
//   GetStatus(i)      "Downloading", "Installing" (becoming a game card),
//                     "Paused", "Pending", or why it waits
//   GetDetail(i)      "288 MB of 412 MB" (active) or "412 MB" (pending)
//   GetProgress(i)    0..1
//   GetIcon(i)        the Blades' 64x icon for the kind
//   IsPaused()        the active download is paused
//   StartNow(i)       a pending item becomes the active download; the one
//                     that was active goes back to the top of the queue
//                     (resumes where it was when started again)
//   TogglePause()     pause or resume the active download
//   Remove(i)         drop an item, active or pending (the helper deletes
//                     what it had); removing the active one starts the next
//   Queue(t, i)       queue Store item i of tab t: 1 queued, 0 already
//                     queued or downloaded, -1 the queue is full
//   Has(id)           0, 1 queued, 2 downloaded
//   Play(t, i)        play Store item i of tab t as it downloads: the helper
//                     streams it and puts it in the DVD tray
//   GetPlayState()    "" (nothing asked), "starting", "ready" (in the tray:
//                     start it from there) or "error"
//   GetPlayDetail()   why, for "error"
// Not part of Microsoft's dashboard; added for Frolick (tools/frolickdash).

#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"
#include "store.h"

#define DL_MAX 16
#define DL_ID 96
#define DL_NAME 128
#define DL_KIND 32
#define HIST_MAX 64

enum { dlPending = 0, dlDownloading = 1, dlPaused = 2 };

struct DLITEM
{
    CHAR szId[DL_ID];
    CHAR szName[DL_NAME];
    CHAR szKind[DL_KIND];
    int nSizeMB;
    __int64 qwBytes;      // the size exactly, for queue.txt
    float nDoneMB;
    int nState;
};

static const CHAR c_szQueue[] = "E:\\FROLICK\\STORE\\queue.txt";
static const CHAR c_szStatus[] = "E:\\FROLICK\\STORE\\status.txt";
static const CHAR c_szHistory[] = "E:\\FROLICK\\STORE\\history.txt";
static const CHAR c_szPlay[] = "E:\\FROLICK\\STORE\\play.txt";

// From status.txt: the helper's word on the store, and on the active
// download when it isn't simply downloading.
static int c_nOnline = -1;          // -1: no word from the helper
static CHAR c_szOnline[96];
static CHAR c_szActiveState[16];
static CHAR c_szActiveDetail[64];
// the item Play asked for, and the helper's word on it
static CHAR c_szPlayId[DL_ID];
static CHAR c_szStreamId[DL_ID];
static CHAR c_szStreamState[16];
static CHAR c_szStreamDetail[64];

const CHAR* StoreServiceState()
{
    if (c_nOnline < 0)
        return "The store service isn't running on this console.";
    if (c_nOnline == 0 && c_szOnline[0])
        return c_szOnline;
    return "Getting the catalog from the Internet Archive...";
}

static void Copy(CHAR* dst, const CHAR* src, int n)
{
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

class CDownloads : public CNode
{
    DECLARE_NODE(CDownloads, CNode)
public:
    CDownloads();

    void Advance(float nSeconds);

    int GetCount();
    int HasActive();
    CStrObject* GetName(int i);
    CStrObject* GetKind(int i);
    CStrObject* GetStatus(int i);
    CStrObject* GetDetail(int i);
    float GetProgress(int i);
    CStrObject* GetIcon(int i);
    int IsPaused();
    void StartNow(int i);
    void TogglePause();
    void Remove(int i);
    int Queue(int t, int i);
    int Has(const TCHAR* szId);
    int Play(int t, int i);
    CStrObject* GetPlayState();
    CStrObject* GetPlayDetail();

protected:
    void SavePlay();
    bool OK(int i) { return i >= 0 && i < m_nCount; }
    int Find(const CHAR* szId);
    void StartNext();
    void Finish(int i);
    void ReadStatus();
    void LoadQueue();
    void SaveQueue();
    void LoadHistory();
    void SaveHistory();

    DLITEM m_rg[DL_MAX];
    int m_nCount;
    DLITEM m_rgHist[HIST_MAX];
    int m_nHist;
    float m_nWait;          // seconds to the next look at status.txt
    float m_nStale;         // seconds status.txt has not changed
    FILETIME m_ftStatus;
    DWORD m_cbStatus;

    DECLARE_NODE_PROPS()
    DECLARE_NODE_FUNCTIONS()
};

IMPLEMENT_NODE("Downloads", CDownloads, CNode)

START_NODE_PROPS(CDownloads, CNode)
END_NODE_PROPS()

START_NODE_FUN(CDownloads, CNode)
    NODE_FUN_IV(GetCount)
    NODE_FUN_IV(HasActive)
    NODE_FUN_SI(GetName)
    NODE_FUN_SI(GetKind)
    NODE_FUN_SI(GetStatus)
    NODE_FUN_SI(GetDetail)
    NODE_FUN_NI(GetProgress)
    NODE_FUN_SI(GetIcon)
    NODE_FUN_IV(IsPaused)
    NODE_FUN_VI(StartNow)
    NODE_FUN_VV(TogglePause)
    NODE_FUN_VI(Remove)
    NODE_FUN_III(Queue)
    NODE_FUN_IS(Has)
    NODE_FUN_III(Play)
    NODE_FUN_SV(GetPlayState)
    NODE_FUN_SV(GetPlayDetail)
END_NODE_FUN()

CDownloads::CDownloads() : m_nCount(0), m_nHist(0), m_nWait(0.0f), m_nStale(0.0f), m_cbStatus(0)
{
    ZeroMemory(&m_ftStatus, sizeof m_ftStatus);
    CreateDirectoryA("E:\\FROLICK", NULL);
    CreateDirectoryA("E:\\FROLICK\\STORE", NULL);
    LoadQueue();
    LoadHistory();
    SavePlay();     // a title that was playing has ended: its disc comes out
    ReadStatus();
}

// ---- files ---------------------------------------------------------------

void CDownloads::LoadQueue()
{
    CHAR* p = StoreReadFile(c_szQueue, NULL);
    if (p == NULL)
        return;
    if (strstr(p, "\nend") != NULL)
    {
        CHAR* line = p;
        while (line && *line)
        {
            CHAR* next = strchr(line, '\n');
            if (next)
                *next++ = 0;
            int n = strlen(line);
            if (n > 0 && line[n - 1] == '\r')
                line[n - 1] = 0;
            CHAR* f[6];
            if (strncmp(line, "q\t", 2) == 0 && m_nCount < DL_MAX && StoreSplit(line, f, 6) == 6)
            {
                DLITEM* d = &m_rg[m_nCount];
                ZeroMemory(d, sizeof *d);
                Copy(d->szId, f[1], DL_ID);
                Copy(d->szKind, f[4], DL_KIND);
                Copy(d->szName, f[5], DL_NAME);
                d->qwBytes = _atoi64(f[3]);
                d->nSizeMB = (int)(d->qwBytes / (1024 * 1024));
                d->nState = m_nCount == 0 ? (strcmp(f[2], "pause") == 0 ? dlPaused : dlDownloading) : dlPending;
                m_nCount++;
            }
            line = next;
        }
    }
    delete [] p;
}

static void Put(HANDLE h, const CHAR* sz)
{
    DWORD cb;
    WriteFile(h, sz, strlen(sz), &cb, NULL);
}

void CDownloads::SaveQueue()
{
    HANDLE h = CreateFileA(c_szQueue, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    CHAR sz[DL_ID + DL_NAME + DL_KIND + 64];
    Put(h, "frolick-queue 1\n");
    for (int i = 0; i < m_nCount; i++)
    {
        const DLITEM* d = &m_rg[i];
        _snprintf(sz, sizeof sz - 1, "q\t%s\t%s\t%I64d\t%s\t%s\n", d->szId,
            d->nState == dlPaused ? "pause" : "go", d->qwBytes, d->szKind, d->szName);
        sz[sizeof sz - 1] = 0;
        Put(h, sz);
    }
    Put(h, "end\n");
    CloseHandle(h);
}

void CDownloads::LoadHistory()
{
    CHAR* p = StoreReadFile(c_szHistory, NULL);
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
        CHAR* f[5];
        if (strncmp(line, "h\t", 2) == 0 && m_nHist < HIST_MAX && StoreSplit(line, f, 5) == 5)
        {
            DLITEM* d = &m_rgHist[m_nHist++];
            ZeroMemory(d, sizeof *d);
            Copy(d->szId, f[1], DL_ID);
            d->nSizeMB = atoi(f[2]);
            Copy(d->szKind, f[3], DL_KIND);
            Copy(d->szName, f[4], DL_NAME);
        }
        line = next;
    }
    delete [] p;
}

void CDownloads::SaveHistory()
{
    HANDLE h = CreateFileA(c_szHistory, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    CHAR sz[DL_ID + DL_NAME + DL_KIND + 64];
    Put(h, "frolick-history 1\n");
    for (int i = 0; i < m_nHist; i++)
    {
        const DLITEM* d = &m_rgHist[i];
        _snprintf(sz, sizeof sz - 1, "h\t%s\t%d\t%s\t%s\n", d->szId, d->nSizeMB, d->szKind, d->szName);
        sz[sizeof sz - 1] = 0;
        Put(h, sz);
    }
    Put(h, "end\n");
    CloseHandle(h);
}

// The helper's status.txt: progress for the active download and what the
// others have, finished items out of the queue.
void CDownloads::ReadStatus()
{
    FILETIME ft;
    DWORD cb;
    if (!StoreStat(c_szStatus, &ft, &cb))
    {
        c_nOnline = -1;
        return;
    }
    if (CompareFileTime(&ft, &m_ftStatus) == 0 && cb == m_cbStatus)
    {
        if (m_nStale > 10.0f)
            c_nOnline = -1;     // the helper stopped writing it
        return;
    }
    CHAR* p = StoreReadFile(c_szStatus, NULL);
    if (p == NULL)
        return;
    if (strstr(p, "\nend") == NULL)
    {
        delete [] p;            // half-written: the next look gets it
        return;
    }
    m_ftStatus = ft;
    m_cbStatus = cb;
    m_nStale = 0.0f;
    c_szActiveState[0] = 0;
    c_szActiveDetail[0] = 0;
    c_szStreamId[0] = 0;
    bool bChanged = false;
    CHAR* line = p;
    while (line && *line)
    {
        CHAR* next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        int n = strlen(line);
        if (n > 0 && line[n - 1] == '\r')
            line[n - 1] = 0;
        CHAR* f[6];
        int k = StoreSplit(line, f, 6);
        if (strcmp(f[0], "online") == 0 && k >= 3)
        {
            c_nOnline = atoi(f[1]);
            Copy(c_szOnline, f[2], sizeof c_szOnline);
        }
        else if (strcmp(f[0], "active") == 0 && k >= 5)
        {
            int i = Find(f[1]);
            if (i >= 0)
            {
                __int64 total = _atoi64(f[4]);
                if (total > 0)
                {
                    m_rg[i].qwBytes = total;
                    m_rg[i].nSizeMB = (int)(total / (1024 * 1024));
                }
                m_rg[i].nDoneMB = (float)(_atoi64(f[3]) / 1024) / 1024.0f;
                if (i == 0)
                {
                    Copy(c_szActiveState, f[2], sizeof c_szActiveState);
                    if (k >= 6)
                        Copy(c_szActiveDetail, f[5], sizeof c_szActiveDetail);
                }
            }
        }
        else if (strcmp(f[0], "stream") == 0 && k >= 3)
        {
            Copy(c_szStreamId, f[1], sizeof c_szStreamId);
            Copy(c_szStreamState, f[2], sizeof c_szStreamState);
            c_szStreamDetail[0] = 0;
            if (k >= 6)
                Copy(c_szStreamDetail, f[5], sizeof c_szStreamDetail);
        }
        else if (strcmp(f[0], "have") == 0 && k >= 3)
        {
            int i = Find(f[1]);
            if (i >= 0)
                m_rg[i].nDoneMB = (float)(_atoi64(f[2]) / 1024) / 1024.0f;
        }
        else if (strcmp(f[0], "gone") == 0 && k >= 2)
        {
            for (int h = 0; h < m_nHist; h++)
            {
                if (strcmp(m_rgHist[h].szId, f[1]) != 0)
                    continue;
                memmove(&m_rgHist[h], &m_rgHist[h + 1], (m_nHist - h - 1) * sizeof(DLITEM));
                m_nHist--;
                SaveHistory();
                break;
            }
        }
        else if (strcmp(f[0], "done") == 0 && k >= 2)
        {
            int i = Find(f[1]);
            if (i >= 0)
            {
                Finish(i);
                bChanged = true;
            }
        }
        line = next;
    }
    delete [] p;
    if (bChanged)
        SaveQueue();
}

void CDownloads::Advance(float nSeconds)
{
    CNode::Advance(nSeconds);
    m_nStale += nSeconds;
    m_nWait -= nSeconds;
    if (m_nWait > 0.0f)
        return;
    m_nWait = 1.0f;
    ReadStatus();
}

// ---- the queue -----------------------------------------------------------

int CDownloads::Find(const CHAR* szId)
{
    for (int i = 0; i < m_nCount; i++)
        if (strcmp(m_rg[i].szId, szId) == 0)
            return i;
    return -1;
}

// item i finished: into the history (newest first), out of the queue
void CDownloads::Finish(int i)
{
    if (m_nHist == HIST_MAX)
        m_nHist--;
    memmove(&m_rgHist[1], &m_rgHist[0], m_nHist * sizeof(DLITEM));
    m_rgHist[0] = m_rg[i];
    m_rgHist[0].nDoneMB = (float)m_rgHist[0].nSizeMB;
    m_nHist++;
    SaveHistory();
    memmove(&m_rg[i], &m_rg[i + 1], (m_nCount - i - 1) * sizeof(DLITEM));
    m_nCount--;
    if (i == 0)
        StartNext();
}

int CDownloads::Queue(int t, int i)
{
    const STOREITEM* it = StoreItem(t, i);
    if (it == NULL)
        return 0;
    if (Find(it->szId) >= 0)
        return 0;
    for (int h = 0; h < m_nHist; h++)
        if (strcmp(m_rgHist[h].szId, it->szId) == 0)
            return 0;
    if (m_nCount >= DL_MAX)
        return -1;
    DLITEM* d = &m_rg[m_nCount];
    ZeroMemory(d, sizeof *d);
    Copy(d->szId, it->szId, DL_ID);
    Copy(d->szName, it->szTitle, DL_NAME);
    Copy(d->szKind, it->szKind, DL_KIND);
    d->nSizeMB = it->nMB;
    d->qwBytes = it->qwBytes;
    d->nState = m_nCount == 0 ? dlDownloading : dlPending;
    m_nCount++;
    SaveQueue();
    return 1;
}

int CDownloads::Has(const TCHAR* szId)
{
    CHAR sz[DL_ID];
    int n = 0;
    for (; szId[n] && n < DL_ID - 1; n++)
        sz[n] = (CHAR)szId[n];      // identifiers are ASCII
    sz[n] = 0;
    if (Find(sz) >= 0)
        return 1;
    for (int h = 0; h < m_nHist; h++)
        if (strcmp(m_rgHist[h].szId, sz) == 0)
            return 2;
    return 0;
}

// ---- play as you download ----------------------------------------------

void CDownloads::SavePlay()
{
    HANDLE h = CreateFileA(c_szPlay, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    CHAR sz[DL_ID + 8];
    Put(h, "frolick-play 1\n");
    if (c_szPlayId[0])
    {
        _snprintf(sz, sizeof sz - 1, "p\t%s\n", c_szPlayId);
        sz[sizeof sz - 1] = 0;
        Put(h, sz);
    }
    Put(h, "end\n");
    CloseHandle(h);
}

int CDownloads::Play(int t, int i)
{
    const STOREITEM* it = StoreItem(t, i);
    if (it == NULL || !it->bStream)
        return 0;
    Copy(c_szPlayId, it->szId, sizeof c_szPlayId);
    c_szStreamId[0] = 0;    // its word from the next status.txt on
    SavePlay();
    return 1;
}

CStrObject* CDownloads::GetPlayState()
{
    if (c_szPlayId[0] == 0)
        return StoreStr("");
    if (strcmp(c_szStreamId, c_szPlayId) != 0)
        return StoreStr("starting");
    return StoreStr(c_szStreamState);
}

CStrObject* CDownloads::GetPlayDetail()
{
    if (c_szPlayId[0] == 0 || strcmp(c_szStreamId, c_szPlayId) != 0)
        return StoreStr("");
    return StoreStr(c_szStreamDetail);
}

void CDownloads::StartNext()
{
    if (m_nCount > 0 && m_rg[0].nState == dlPending)
        m_rg[0].nState = dlDownloading;
}

int CDownloads::GetCount()
{
    return m_nCount;
}

int CDownloads::HasActive()
{
    return m_nCount > 0 && m_rg[0].nState != dlPending ? 1 : 0;
}

CStrObject* CDownloads::GetName(int i)
{
    return StoreStr(OK(i) ? m_rg[i].szName : "");
}

CStrObject* CDownloads::GetKind(int i)
{
    return StoreStr(OK(i) ? m_rg[i].szKind : "");
}

CStrObject* CDownloads::GetStatus(int i)
{
    if (!OK(i))
        return new CStrObject;
    if (m_rg[i].nState == dlPaused)
        return new CStrObject(_T("Paused"));
    if (m_rg[i].nState == dlPending)
        return new CStrObject(_T("Pending"));
    // the active one: what the helper says about it
    if (c_nOnline < 0)
        return new CStrObject(_T("Waiting"));
    if (strcmp(c_szActiveState, "nospace") == 0)
        return new CStrObject(_T("Not enough space"));
    if (strcmp(c_szActiveState, "error") == 0)
        return new CStrObject(_T("Retrying"));
    if (strcmp(c_szActiveState, "installing") == 0)
        return new CStrObject(_T("Installing"));
    return new CStrObject(_T("Downloading"));
}

CStrObject* CDownloads::GetDetail(int i)
{
    CHAR sz[64], szDone[24], szSize[24];
    if (!OK(i))
        return new CStrObject;
    StoreFormatMB(szSize, m_rg[i].nSizeMB);
    if (i == 0 && c_szActiveDetail[0] && m_rg[0].nState == dlDownloading)
        return StoreStr(c_szActiveDetail);
    if (m_rg[i].nState == dlPending && m_rg[i].nDoneMB <= 0.0f)
        return StoreStr(szSize);
    StoreFormatMB(szDone, (int)m_rg[i].nDoneMB);
    sprintf(sz, "%s of %s", szDone, szSize);
    return StoreStr(sz);
}

float CDownloads::GetProgress(int i)
{
    if (!OK(i) || m_rg[i].nSizeMB <= 0)
        return 0.0f;
    float f = m_rg[i].nDoneMB / (float)m_rg[i].nSizeMB;
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

CStrObject* CDownloads::GetIcon(int i)
{
    if (OK(i) && strcmp(m_rg[i].szKind, "Theme") == 0)
        return new CStrObject(_T("ico_64x_Themes.png"));
    return new CStrObject(_T("ico_64x_GameDownloads.png"));
}

int CDownloads::IsPaused()
{
    return m_nCount > 0 && m_rg[0].nState == dlPaused ? 1 : 0;
}

// A pending item jumps the queue: it becomes the active download and
// the one that was active waits again at the top (the helper keeps what
// it had and resumes from there).
void CDownloads::StartNow(int i)
{
    if (!OK(i))
        return;
    if (i == 0)
    {
        if (m_rg[0].nState == dlPending)
            m_rg[0].nState = dlDownloading;
        SaveQueue();
        return;
    }
    DLITEM item = m_rg[i];
    memmove(&m_rg[1], &m_rg[0], i * sizeof(DLITEM));
    m_rg[0] = item;
    m_rg[0].nState = dlDownloading;
    m_rg[1].nState = dlPending;
    SaveQueue();
}

void CDownloads::TogglePause()
{
    if (m_nCount == 0)
        return;
    if (m_rg[0].nState == dlDownloading)
        m_rg[0].nState = dlPaused;
    else
        m_rg[0].nState = dlDownloading;
    SaveQueue();
}

void CDownloads::Remove(int i)
{
    if (!OK(i))
        return;
    memmove(&m_rg[i], &m_rg[i + 1], (m_nCount - i - 1) * sizeof(DLITEM));
    m_nCount--;
    if (i == 0)
        StartNext();
    SaveQueue();
}
