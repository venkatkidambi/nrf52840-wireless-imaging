#ifndef IMAGING_NRFX_GLUE_H
#define IMAGING_NRFX_GLUE_H
#include "system.h"
#define NRFX_ASSERT(x) do { if (!(x)) platform_fault(4); } while (0)
#define NRFX_STATIC_ASSERT(x) _Static_assert((x), #x)
#endif
