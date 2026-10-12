/* The DirectSound mixer's per-sample work, natively (XBR_DS_MIX). */
#include "../hle/dsmix.h"

void renderer_ds_mix(struct dsmix_job *job);

void renderer_ds_mix(struct dsmix_job *job)
{
    dsmix_run(job);
}
