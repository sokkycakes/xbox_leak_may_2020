// The game's sound made longer without changing its pitch (WSOLA), for when the game runs
// slower than the DS (a scene too heavy for the Sion): instead of playing it slower and so
// lower, 16 ms pieces of it overlap a little more than they did, each one placed where it
// lines up best with the sound before it (within 4 ms), so what repeats is a whole number
// of the sound's own waves. At a ratio of 1 the pieces follow on as they were and the
// sound comes out as it went in, 16 ms later.
#ifndef MIRROR_STRETCH_H
#define MIRROR_STRETCH_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class Stretcher
{
public:
    static constexpr int N = 768, H = N / 2, Delta = 192;   // window, hop, search (frames at 48 kHz)

    Stretcher()
    {
        // periodic Hann: two windows half a window apart add up to 1
        for (int i = 0; i < N; i++) win[i] = 0.5f - 0.5f * cosf(6.2831853f * i / N);
    }

    // stereo frames in, stereo frames out; ratio: how much longer (1 to 1.25)
    void Process(const int16_t* in, uint32_t frames, double ratio, std::vector<int16_t>& out)
    {
        ratio = ratio < 1 ? 1 : ratio > 1.25 ? 1.25 : ratio;
        buf.insert(buf.end(), in, in + frames * 2);
        end += frames;
        for (;;)
        {
            int64_t natural = prev + H;   // where the last piece would have gone on
            int64_t c;
            if (prev < 0)
            {
                c = (int64_t)pos;
                if (c + N > end) break;
            }
            else if (ratio == 1 && natural >= start)
            {
                // nothing to stretch: carry on where the sound did, and stop owing any
                c = natural;
                if (c + N > end) break;
                pos = (double)c;
            }
            else
            {
                int64_t p = (int64_t)llround(pos);
                if (p + Delta + N > end) break;
                c = Best(p, natural);
            }
            // overlap-add: the last piece's second half and this one's first half
            const float* s = &buf[(c - start) * 2];
            for (int i = 0; i < H; i++)
                for (int ch = 0; ch < 2; ch++)
                {
                    float v = tail[i * 2 + ch] + win[i] * s[i * 2 + ch];
                    out.push_back((int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v)));
                    tail[i * 2 + ch] = win[H + i] * s[(H + i) * 2 + ch];
                }
            prev = c;
            pos += H / ratio;
            // what no later piece can start at or reach back to goes
            int64_t keep = std::min<int64_t>((int64_t)pos - Delta, prev + H) - 1;
            if (keep > start + 4096)
            {
                buf.erase(buf.begin(), buf.begin() + (keep - start) * 2);
                start = keep;
            }
        }
    }

private:
    std::vector<float> buf;   // stereo input from frame `start` on
    int64_t start = 0, end = 0;
    double pos = 0;           // where the next piece would start if the sound were only slowed
    int64_t prev = -1;        // where the last piece started
    float tail[H * 2] = {};
    float win[N];

    float Mono(int64_t f) const { return buf[(f - start) * 2] + buf[(f - start) * 2 + 1]; }

    // the start within Delta of p whose first half matches the last piece's continuation
    // best (normalized correlation); the continuation itself wins a tie
    int64_t Best(int64_t p, int64_t natural)
    {
        int64_t lo = std::max<int64_t>(p - Delta, start), hi = p + Delta;
        auto score = [&](int64_t c, int step) {
            double dot = 0, e = 0;
            for (int i = 0; i < H; i += step)
            {
                float a = Mono(natural + i), b = Mono(c + i);
                dot += a * b;
                e += b * b;
            }
            return e > 0 ? dot / sqrt(e) : -1e30;
        };
        int64_t best = -1;
        double bestScore = -1e300;
        if (natural >= lo && natural <= hi)
        {
            best = natural;
            bestScore = score(natural, 2);
        }
        // every other start at half the samples, then the neighbours of the best one fully
        for (int64_t c = lo + ((lo ^ p) & 1); c <= hi; c += 2)
        {
            double sc = score(c, 2);
            if (sc > bestScore) { bestScore = sc; best = c; }
        }
        if (best < 0) return p;
        int64_t fine = best;
        double fineScore = score(best, 1);
        for (int64_t c = best - 1; c <= best + 1; c += 2)
        {
            if (c < lo || c > hi) continue;
            double sc = score(c, 1);
            if (sc > fineScore) { fineScore = sc; fine = c; }
        }
        return fine;
    }
};

#endif
