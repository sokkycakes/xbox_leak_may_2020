// Downloads: the queue behind Frolick's Downloads page (catalog pad, Y),
// laid out as the NXE dashboard's Active Downloads: one active download
// on top, the pending queue under it (sokky, 2026-10-20).
//
// PLACEHOLDER: Frolick has no download system yet (no store, no server),
// so this node holds a sample queue and moves the active download along
// by itself (c_mbPerSecond) so the page can be driven and looked at.
// Its script interface is the one a real downloader will get, so only
// this file changes when one exists.
//
//   DEF theDownloads Downloads { }
//
// Items are listed active first (index 0 when HasActive() is 1), then
// the pending queue in order.  Functions:
//   GetCount()        items, the active one included
//   HasActive()       1 when item 0 is the active download (downloading
//                     or paused), 0 when the queue only pends
//   GetName(i)        the item's title
//   GetKind(i)        what it is ("Game card image", "Theme", ...)
//   GetStatus(i)      "Downloading", "Paused" or "Pending"
//   GetDetail(i)      "288 MB of 412 MB" (active) or "412 MB" (pending)
//   GetProgress(i)    0..1
//   GetIcon(i)        the Blades' 64x icon for the kind
//   IsPaused()        the active download is paused
//   StartNow(i)       a pending item becomes the active download; the one
//                     that was active goes back to the top of the queue,
//                     paused (resumes where it was when started again)
//   TogglePause()     pause or resume the active download
//   Remove(i)         drop an item, active or pending; removing the active
//                     one starts the next pending item
// A download that completes leaves the list and the next pending item
// starts, as a queue does.
// Not part of Microsoft's dashboard; added for Frolick (tools/frolickdash).

#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"

#define DL_MAX 16
#define DL_NAME 64
#define DL_KIND 32

enum { dlPending = 0, dlDownloading = 1, dlPaused = 2 };

struct DLITEM
{
    CHAR szName[DL_NAME];
    CHAR szKind[DL_KIND];
    int nSizeMB;
    float nDoneMB;
    int nState;
};

static const float c_mbPerSecond = 6.0f;     // the sample download's speed

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

protected:
    bool OK(int i) { return i >= 0 && i < m_nCount; }
    void Add(const CHAR* szName, const CHAR* szKind, int nSizeMB);
    void StartNext();

    DLITEM m_rg[DL_MAX];
    int m_nCount;

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
END_NODE_FUN()

CDownloads::CDownloads() : m_nCount(0)
{
    // The sample queue (PLACEHOLDER until Frolick can download): one
    // download under way, three waiting.
    Add("Stiletto Swift", "Game card image", 412);
    m_rg[0].nDoneMB = 288.0f;
    m_rg[0].nState = dlDownloading;
    Add("Frolick Premium Theme Pack", "Theme", 12);
    Add("Marble Blast Ultra", "Game card image", 96);
    Add("Orbz", "Game card image", 54);
}

void CDownloads::Add(const CHAR* szName, const CHAR* szKind, int nSizeMB)
{
    if (m_nCount >= DL_MAX)
        return;
    DLITEM* p = &m_rg[m_nCount++];
    strncpy(p->szName, szName, DL_NAME - 1);
    p->szName[DL_NAME - 1] = 0;
    strncpy(p->szKind, szKind, DL_KIND - 1);
    p->szKind[DL_KIND - 1] = 0;
    p->nSizeMB = nSizeMB;
    p->nDoneMB = 0.0f;
    p->nState = dlPending;
}

// The active download (the sample one moves by itself); a finished one
// leaves the list and the next pending item takes its place.
void CDownloads::Advance(float nSeconds)
{
    CNode::Advance(nSeconds);
    if (m_nCount == 0 || m_rg[0].nState != dlDownloading)
        return;
    m_rg[0].nDoneMB += c_mbPerSecond * nSeconds;
    if (m_rg[0].nDoneMB >= (float)m_rg[0].nSizeMB)
    {
        Remove(0);
    }
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

static CStrObject* Str(const CHAR* sz)
{
    TCHAR wsz[DL_NAME];
    _stprintf(wsz, _T("%hs"), sz);
    return new CStrObject(wsz);
}

CStrObject* CDownloads::GetName(int i)
{
    return Str(OK(i) ? m_rg[i].szName : "");
}

CStrObject* CDownloads::GetKind(int i)
{
    return Str(OK(i) ? m_rg[i].szKind : "");
}

CStrObject* CDownloads::GetStatus(int i)
{
    if (!OK(i))
        return new CStrObject;
    if (m_rg[i].nState == dlDownloading)
        return new CStrObject(_T("Downloading"));
    if (m_rg[i].nState == dlPaused)
        return new CStrObject(_T("Paused"));
    return new CStrObject(_T("Pending"));
}

CStrObject* CDownloads::GetDetail(int i)
{
    TCHAR sz[64];
    if (!OK(i))
        return new CStrObject;
    if (m_rg[i].nState == dlPending && m_rg[i].nDoneMB <= 0.0f)
        _stprintf(sz, _T("%d MB"), m_rg[i].nSizeMB);
    else
        _stprintf(sz, _T("%d MB of %d MB"), (int)m_rg[i].nDoneMB, m_rg[i].nSizeMB);
    return new CStrObject(sz);
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
// the one that was active waits again at the top, paused where it was.
void CDownloads::StartNow(int i)
{
    if (!OK(i))
        return;
    if (i == 0)
    {
        if (m_rg[0].nState == dlPending)
            m_rg[0].nState = dlDownloading;
        return;
    }
    DLITEM item = m_rg[i];
    memmove(&m_rg[1], &m_rg[0], i * sizeof(DLITEM));
    m_rg[0] = item;
    m_rg[0].nState = dlDownloading;
    if (m_rg[1].nState != dlPending)
        m_rg[1].nState = dlPending;   // keeps its nDoneMB; resumes from there
}

void CDownloads::TogglePause()
{
    if (m_nCount == 0)
        return;
    if (m_rg[0].nState == dlDownloading)
        m_rg[0].nState = dlPaused;
    else
        m_rg[0].nState = dlDownloading;
}

void CDownloads::Remove(int i)
{
    if (!OK(i))
        return;
    memmove(&m_rg[i], &m_rg[i + 1], (m_nCount - i - 1) * sizeof(DLITEM));
    m_nCount--;
    if (i == 0)
        StartNext();
}
