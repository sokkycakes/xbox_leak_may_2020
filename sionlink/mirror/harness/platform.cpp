// Minimal melonDS Platform layer for the headless mirror runner: plain files,
// std threads, no audio/network/camera.

#include <cstdio>
#include <cstdarg>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <pthread.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Platform.h"
#include "saves.h"

namespace melonDS::Platform
{

void SignalStop(StopReason, void*) {}

std::string GetLocalFilePath(const std::string& filename) { return filename; }

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    std::string m;
    if (mode & FileMode::Read && mode & FileMode::Write)
        m = (mode & FileMode::Preserve) ? "r+" : "w+";
    else if (mode & FileMode::Write)
        m = (mode & FileMode::Append) ? "a" : "w";
    else
        m = "r";
    if (!(mode & FileMode::Text)) m += "b";
    if ((mode & FileMode::Preserve) && (mode & FileMode::Write) && !(mode & FileMode::NoCreate))
    {
        FILE* t = fopen(path.c_str(), "ab");
        if (t) fclose(t);
    }
    return (FileHandle*)fopen(path.c_str(), m.c_str());
}
FileHandle* OpenLocalFile(const std::string& path, FileMode mode) { return OpenFile(path, mode); }
bool FileExists(const std::string& name) { FILE* f = fopen(name.c_str(), "rb"); if (f) fclose(f); return f; }
bool LocalFileExists(const std::string& name) { return FileExists(name); }
bool CheckFileWritable(const std::string&) { return true; }
bool CheckLocalFileWritable(const std::string&) { return true; }
bool CloseFile(FileHandle* f) { return fclose((FILE*)f) == 0; }
bool IsEndOfFile(FileHandle* f) { return feof((FILE*)f); }
bool FileReadLine(char* str, int count, FileHandle* f) { return fgets(str, count, (FILE*)f); }
u64 FilePosition(FileHandle* f) { return ftell((FILE*)f); }
bool FileSeek(FileHandle* f, s64 off, FileSeekOrigin o)
{
    int w = o == FileSeekOrigin::Start ? SEEK_SET : o == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
    return fseek((FILE*)f, off, w) == 0;
}
void FileRewind(FileHandle* f) { rewind((FILE*)f); }
u64 FileRead(void* data, u64 size, u64 count, FileHandle* f) { return fread(data, size, count, (FILE*)f); }
bool FileFlush(FileHandle* f) { return fflush((FILE*)f) == 0; }
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* f) { return fwrite(data, size, count, (FILE*)f); }
u64 FileWriteFormatted(FileHandle* f, const char* fmt, ...)
{
    va_list a; va_start(a, fmt); int r = vfprintf((FILE*)f, fmt, a); va_end(a); return r;
}
u64 FileLength(FileHandle* f)
{
    long p = ftell((FILE*)f); fseek((FILE*)f, 0, SEEK_END); long l = ftell((FILE*)f); fseek((FILE*)f, p, SEEK_SET); return l;
}

void Log(LogLevel level, const char* fmt, ...)
{
    if (level < LogLevel::Warn && !getenv("MIRROR_VERBOSE")) return;
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
}

struct Thread { std::thread t; };
// melonDS's threads (the 3D renderer's): named for mirrorrun's CPU line
Thread* Thread_Create(std::function<void()> func)
{
    return new Thread{std::thread([func] { pthread_setname_np(pthread_self(), "mirror-3d"); func(); })};
}
void Thread_Free(Thread* t) { if (t->t.joinable()) t->t.detach(); delete t; }
void Thread_Wait(Thread* t) { if (t->t.joinable()) t->t.join(); }

struct Semaphore { std::mutex m; std::condition_variable cv; int count = 0; };
Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* s) { delete s; }
void Semaphore_Reset(Semaphore* s) { std::lock_guard<std::mutex> l(s->m); s->count = 0; }
void Semaphore_Wait(Semaphore* s)
{
    std::unique_lock<std::mutex> l(s->m);
    s->cv.wait(l, [s] { return s->count > 0; });
    s->count--;
}
bool Semaphore_TryWait(Semaphore* s, int timeout_ms)
{
    std::unique_lock<std::mutex> l(s->m);
    if (!s->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [s] { return s->count > 0; })) return false;
    s->count--;
    return true;
}
void Semaphore_Post(Semaphore* s, int count)
{
    { std::lock_guard<std::mutex> l(s->m); s->count += count; }
    s->cv.notify_all();
}

struct Mutex { std::mutex m; };
Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* m) { delete m; }
void Mutex_Lock(Mutex* m) { m->m.lock(); }
void Mutex_Unlock(Mutex* m) { m->m.unlock(); }
bool Mutex_TryLock(Mutex* m) { return m->m.try_lock(); }

void Sleep(u64 usecs) { std::this_thread::sleep_for(std::chrono::microseconds(usecs)); }
u64 GetMSCount() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
u64 GetUSCount() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void WriteNDSSave(const u8* data, u32 len, u32 off, u32 n, void*)
{
    if (gSaveKeeper) gSaveKeeper->Note(data, len, off, n);
}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

// The DS's radio. Cloud tests of the DS app's Wi-Fi link: MELONDS_AIR=host:port trades the
// emulated radio's frames (melonDS's 12-byte TX/RX header kept) with a UDP peer, a software
// Sion such as sionlinkd --melon (branch rb4kah), as its AirMP bridge does. Otherwise nothing
// is out there.
static int AirSock()
{
    static int sock = -2;
    static sockaddr_in peer;
    if (sock != -2) return sock;
    sock = -1;
    const char* air = getenv("MELONDS_AIR");
    if (!air) return sock;
    std::string s(air);
    size_t colon = s.rfind(':');
    peer.sin_family = AF_INET;
    peer.sin_port = htons(atoi(colon == std::string::npos ? s.c_str() : s.c_str() + colon + 1));
    inet_pton(AF_INET, colon == std::string::npos ? "127.0.0.1" : s.substr(0, colon).c_str(), &peer.sin_addr);
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    connect(sock, (sockaddr*)&peer, sizeof peer);
    send(sock, "", 0, 0);   // the peer learns our port before we send anything
    return sock;
}
static int AirSend(u8* data, int len)
{
    if (AirSock() < 0 || !data || len <= 0) return 0;
    send(AirSock(), data, len, 0);
    return len;
}
static int AirRecv(u8* data, u64* timestamp)
{
    if (AirSock() < 0) return 0;
    *timestamp = 0;   // keeps melonDS out of its instance-synced MP client mode
    int n = recv(AirSock(), data, 2048, 0);
    return n > 0 ? n : 0;
}

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8* d, int n, u64, void*) { return AirSend(d, n); }
int MP_RecvPacket(u8* d, u64* t, void*) { return AirRecv(d, t); }
int MP_SendCmd(u8* d, int n, u64, void*) { return AirSend(d, n); }
int MP_SendReply(u8* d, int n, u64, u16, void*) { return d ? AirSend(d, n) : 0; }
int MP_SendAck(u8* d, int n, u64, void*) { return AirSend(d, n); }
int MP_RecvHostPacket(u8* d, u64* t, void*) { return AirRecv(d, t); }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16*, int, void*) { return 0; }

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0; }

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

}
