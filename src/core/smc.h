// smc: AppleSMC reads for temperatures, fans and system power.
// Each read is a synchronous firmware round trip, so temperature keys are refreshed
// round-robin a few per tick and averages use the latest value of every key.
#pragma once
#include "sampler.h"

int smc_open_once(void);
void smc_sensors(snapshot_t *s);
