//
//  image_io.h
//
//  Dependency-free writers for the capture options: PNG (uncompressed
//  deflate) and 16-bit PCM WAV.
//
#ifndef BOOTANI_IMAGE_IO_H
#define BOOTANI_IMAGE_IO_H

#include <stdint.h>
#include <stdio.h>

// rgba: width*height*4 bytes, first row = top of the image.
bool WritePNG(const char* path, const unsigned char* rgba, int width, int height);

bool MakeDirectory(const char* path);

class WavWriter
{
public:
    WavWriter() : m_file(NULL), m_frames(0), m_channels(2) {}
    ~WavWriter() { Close(); }
    bool Open(const char* path, int rate, int channels);
    void Write(const int16_t* samples, int frames);
    void Close();
private:
    FILE*    m_file;
    uint32_t m_frames;
    int      m_channels;
};

#endif
