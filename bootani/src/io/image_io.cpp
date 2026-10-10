//
//  image_io.cpp
//
#include "image_io.h"
#include <string.h>
#include <vector>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static uint32_t Crc32(const unsigned char* p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static void Put32(std::vector<unsigned char>& v, uint32_t x)
{
    v.push_back((unsigned char)(x >> 24)); v.push_back((unsigned char)(x >> 16));
    v.push_back((unsigned char)(x >> 8));  v.push_back((unsigned char)x);
}

static void Chunk(FILE* f, const char* type, const std::vector<unsigned char>& data)
{
    std::vector<unsigned char> buf;
    Put32(buf, (uint32_t)data.size());
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    uint32_t crc = Crc32(&buf[4], buf.size() - 4);
    Put32(buf, crc);
    fwrite(&buf[0], 1, buf.size(), f);
}

bool WritePNG(const char* path, const unsigned char* rgba, int width, int height)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);

    std::vector<unsigned char> ihdr;
    Put32(ihdr, width);
    Put32(ihdr, height);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // colour type: RGB (alpha dropped, the frame is opaque)
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    Chunk(f, "IHDR", ihdr);

    // Raw scanlines with filter byte 0.
    std::vector<unsigned char> raw;
    raw.reserve((size_t)(width * 3 + 1) * height);
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        const unsigned char* row = rgba + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            raw.push_back(row[x * 4 + 0]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 2]);
        }
    }

    // zlib stream of stored (uncompressed) deflate blocks.
    std::vector<unsigned char> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size(); i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    do {
        size_t n = raw.size() - pos;
        if (n > 65535) n = 65535;
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back((unsigned char)(n & 0xff)); z.push_back((unsigned char)(n >> 8));
        z.push_back((unsigned char)(~n & 0xff)); z.push_back((unsigned char)((~n >> 8) & 0xff));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    Put32(z, (b << 16) | a);
    Chunk(f, "IDAT", z);
    Chunk(f, "IEND", std::vector<unsigned char>());
    return fclose(f) == 0;
}

bool MakeDirectory(const char* path)
{
#ifdef _WIN32
    int r = _mkdir(path);
#else
    int r = mkdir(path, 0755);
#endif
    return r == 0 || errno == EEXIST;
}

static void Le32(FILE* f, uint32_t x) { unsigned char b[4] = { (unsigned char)x, (unsigned char)(x >> 8), (unsigned char)(x >> 16), (unsigned char)(x >> 24) }; fwrite(b, 1, 4, f); }
static void Le16(FILE* f, uint16_t x) { unsigned char b[2] = { (unsigned char)x, (unsigned char)(x >> 8) }; fwrite(b, 1, 2, f); }

bool WavWriter::Open(const char* path, int rate, int channels)
{
    m_file = fopen(path, "wb");
    if (!m_file) return false;
    m_channels = channels;
    m_frames = 0;
    fwrite("RIFF", 1, 4, m_file); Le32(m_file, 0);
    fwrite("WAVEfmt ", 1, 8, m_file); Le32(m_file, 16);
    Le16(m_file, 1); Le16(m_file, (uint16_t)channels);
    Le32(m_file, rate); Le32(m_file, rate * channels * 2);
    Le16(m_file, (uint16_t)(channels * 2)); Le16(m_file, 16);
    fwrite("data", 1, 4, m_file); Le32(m_file, 0);
    return true;
}

void WavWriter::Write(const int16_t* samples, int frames)
{
    if (!m_file) return;
    for (int i = 0; i < frames * m_channels; i++) Le16(m_file, (uint16_t)samples[i]);
    m_frames += frames;
}

void WavWriter::Close()
{
    if (!m_file) return;
    uint32_t data = m_frames * m_channels * 2;
    fseek(m_file, 4, SEEK_SET); Le32(m_file, 36 + data);
    fseek(m_file, 40, SEEK_SET); Le32(m_file, data);
    fclose(m_file);
    m_file = NULL;
}
