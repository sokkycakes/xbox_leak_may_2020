/* jit_sse.c - MMX/SSE translation (filled in below) */
#include "jit.h"
void *xbt_sse_helper;
int jit_sse_classify(jinsn *ji) { ji->clobber = 1; return 0; }
int jit_sse(jit_t *j, jinsn *ji) { (void)j; (void)ji; return 0; }
