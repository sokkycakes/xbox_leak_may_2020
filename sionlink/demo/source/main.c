// Sion Hello: a tiny program to send with Download Play. It shows that both
// CPUs came up: the keys come from the ARM7, the frame count from the ARM9.

#include <stdio.h>

#include <nds.h>

int main(int argc, char *argv[])
{
    PrintConsole top, bottom;

    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&top, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottom, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    consoleSelect(&top);
    printf("\x1b[1;7H== Sion Hello ==\n\n");
    printf(" This program came over\n");
    printf(" the air from the Sion and\n");
    printf(" is running from RAM.\n");

    consoleSelect(&bottom);
    consoleSetColor(&bottom, CONSOLE_LIGHT_GREEN);
    printf("\n Press buttons to test input.\n");
    printf(" Power off to leave.\n");

    unsigned frames = 0;
    for (;;)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 keys = keysHeld();
        consoleSelect(&bottom);
        printf("\x1b[6;1Hkeys %04lx   frame %u   ", (unsigned long)keys, ++frames);
        printf("\x1b[8;1H%s %s %s %s %s %s   ",
               keys & KEY_A ? "A" : "-", keys & KEY_B ? "B" : "-",
               keys & KEY_X ? "X" : "-", keys & KEY_Y ? "Y" : "-",
               keys & KEY_L ? "L" : "-", keys & KEY_R ? "R" : "-");
    }
}
