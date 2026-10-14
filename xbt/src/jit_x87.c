/* jit_x87.c - x87 translation (filled in below) */
#include "jit.h"
void *xbt_x87_helper, *xbt_x87_mem_helper;
void jit_x87_classify(jinsn *ji) { ji->clobber = 1; }
int jit_x87(jit_t *j, jinsn *ji) { (void)j; (void)ji; return 0; }
