// The game's save memory in a file (--save FILE): read when the game starts, written back
// two seconds after the game last wrote to it (and when mirrorrun stops), on a thread of
// its own so the game never waits on the disk. The file is replaced whole each time
// (written beside it, then renamed), so a power cut leaves the old save or the new one.
#ifndef MIRROR_SAVES_H
#define MIRROR_SAVES_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SaveKeeper
{
public:
    ~SaveKeeper() { Close(); }
    // what the file holds now (empty: no save yet)
    std::vector<uint8_t> Load(const std::string& path);
    // from melonDS (Platform::WriteNDSSave), on the game's thread
    void Note(const uint8_t* data, uint32_t len, uint32_t off, uint32_t n);
    // the save as it stands (the game's last writes too, if they aren't in the file yet)
    // copied to `to`, before a quick save is loaded over it
    bool Backup(const std::string& to);
    // writes what's left and stops the thread
    void Close();
    uint32_t Writes() const { return writes; }

private:
    void Run();
    bool WriteFile(const std::vector<uint8_t>& d, const std::string& file);

    std::string path;
    std::mutex m;
    std::condition_variable wake;
    std::vector<uint8_t> mem;   // the save memory as the game left it
    bool dirty = false, stop = false;
    std::chrono::steady_clock::time_point lastNote;
    std::thread t;
    uint32_t writes = 0;
};

// set by mirrorrun when it keeps saves; melonDS's save writes go to it
extern SaveKeeper* gSaveKeeper;

#endif
