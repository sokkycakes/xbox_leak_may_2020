// Shared by Store.cpp (the catalog) and Downloads.cpp (the queue): the
// storefront's files on E:\FROLICK\STORE, written by frolick-store on the
// Sion (tools/frolickdash/store).

#define STORE_MAX 256

struct STOREITEM
{
    int nTab;
    const CHAR* szId;
    int nMB;
    __int64 qwBytes;
    const CHAR* szKind;
    const CHAR* szTile;
    const CHAR* szTitle;
    const CHAR* szDesc;
    bool bStream;           // plays as it downloads (flag "stream")
};

const STOREITEM* StoreItem(int t, int i);
CStrObject* StoreStr(const CHAR* s);
int StoreUtf16(WCHAR* out, int n, const CHAR* s);
CHAR* StoreReadFile(const CHAR* szPath, DWORD* pcb);
int StoreSplit(CHAR* s, CHAR** rg, int n);
bool StoreStat(const CHAR* szPath, FILETIME* pft, DWORD* pcb);
void StoreFormatMB(CHAR* sz, int nMB);

// What the page says when there is no catalog: the helper isn't running,
// is still connecting, or can't reach the Internet Archive (Downloads.cpp,
// from status.txt).
const CHAR* StoreServiceState();
