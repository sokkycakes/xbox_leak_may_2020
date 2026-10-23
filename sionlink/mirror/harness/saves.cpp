#include "saves.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

SaveKeeper* gSaveKeeper = nullptr;

std::vector<uint8_t> SaveKeeper::Load(const std::string& p)
{
    path = p;
    std::vector<uint8_t> d;
    if (FILE* f = fopen(p.c_str(), "rb"))
    {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0)
        {
            d.resize(n);
            if (fread(d.data(), 1, n, f) != (size_t)n) d.clear();
        }
        fclose(f);
        fprintf(stderr, "save: %s, %zu KB\n", p.c_str(), d.size() >> 10);
    }
    else
        fprintf(stderr, "save: %s (none yet)\n", p.c_str());
    t = std::thread([this] { Run(); });
    return d;
}

void SaveKeeper::Note(const uint8_t* data, uint32_t len, uint32_t off, uint32_t n)
{
    std::lock_guard<std::mutex> g(m);
    if (mem.size() != len)
    {
        // the first write (or the cart's size changed): take all of it
        mem.assign(data, data + len);
    }
    else if (off + n <= len)
        memcpy(mem.data() + off, data + off, n);
    else
    {
        // wraps around the end
        memcpy(mem.data() + off, data + off, len - off);
        memcpy(mem.data(), data, n - (len - off));
    }
    dirty = true;
    lastNote = std::chrono::steady_clock::now();
    wake.notify_one();
}

void SaveKeeper::Run()
{
    pthread_setname_np(pthread_self(), "mirror-save");
    std::unique_lock<std::mutex> g(m);
    for (;;)
    {
        wake.wait(g, [this] { return stop || dirty; });
        if (!stop)
        {
            // until the game has stopped writing for 2 s: a save is many small writes
            while (!stop && std::chrono::steady_clock::now() - lastNote < std::chrono::seconds(2))
                wake.wait_until(g, lastNote + std::chrono::seconds(2));
        }
        if (dirty)
        {
            std::vector<uint8_t> d = mem;
            dirty = false;
            g.unlock();
            WriteFile(d, path);
            g.lock();
        }
        if (stop) return;
    }
}

bool SaveKeeper::Backup(const std::string& to)
{
    std::vector<uint8_t> d;
    {
        std::lock_guard<std::mutex> g(m);
        d = mem;
    }
    if (d.empty())
    {
        // nothing written this time: the file as it was
        if (FILE* f = fopen(path.c_str(), "rb"))
        {
            uint8_t buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
            fclose(f);
        }
    }
    if (d.empty()) return true;   // no save yet
    return WriteFile(d, to);
}

bool SaveKeeper::WriteFile(const std::vector<uint8_t>& d, const std::string& file)
{
    std::string tmp = file + ".new";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0 && write(fd, d.data(), d.size()) == (ssize_t)d.size() && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    ok = ok && rename(tmp.c_str(), file.c_str()) == 0;
    if (ok)
    {
        // and the rename itself
        std::string dir = file.substr(0, file.rfind('/') + 1);
        int dfd = open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) { fsync(dfd); close(dfd); }
        if (file == path) writes++;
        fprintf(stderr, "save: %s written (%zu KB)\n", file.c_str(), d.size() >> 10);
    }
    else
        perror("save");
    return ok;
}

void SaveKeeper::Close()
{
    if (!t.joinable()) return;
    {
        std::lock_guard<std::mutex> g(m);
        stop = true;
    }
    wake.notify_one();
    t.join();
}
