// Verilator testbench: feeds the TMDS word stream, dumps every output sample.
#include "Vav_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdint>
int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    if (argc < 4) { fprintf(stderr, "tb in.tmds out.bin ypbpr\n"); return 1; }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    Vav_top *t = new Vav_top;
    t->ypbpr = argv[3][0] == '1';
    t->rst = 1;
    for (int i = 0; i < 8; i++) { t->clk = 0; t->eval(); t->clk = 1; t->eval(); }
    t->rst = 0;
    uint32_t w; long n = 0, lockat = -1;
    while (fread(&w, 4, 1, in) == 1) {
        t->raw0 = w & 0x3ff; t->raw1 = (w >> 10) & 0x3ff; t->raw2 = (w >> 20) & 0x3ff;
        t->clk = 0; t->eval(); t->clk = 1; t->eval();
        if (t->locked && lockat < 0) lockat = n;
        // comp, luma, chroma, r, g, b, flags
        uint8_t rec[8] = { t->comp, t->luma, t->chroma, t->ro, t->go, t->bo,
            (uint8_t)(t->csync_n | (t->de_o << 1) | (t->hs_o << 2) | (t->vs_o << 3) | (t->field2 << 4) | (t->locked << 5)), 0 };
        fwrite(rec, 8, 1, out);
        n++;
    }
    fprintf(stderr, "%ld clocks, all lanes locked at clock %ld\n", n, lockat);
    fclose(out); delete t; return 0;
}
