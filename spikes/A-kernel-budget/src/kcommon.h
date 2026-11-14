// Shared preamble for kernel TUs (see k_*.cu). Variant flags come from the command line.
#pragma once
#include "gpu_scene.h"
#include <cuda_runtime.h>
#define KL_CAT2(a, b) a##b
#define KL_CAT(a, b) KL_CAT2(a, b)
#if defined(KL_LB) && KL_LB
#define KL_LB_ATTR __launch_bounds__(256, KL_LB_MIN)
#else
#define KL_LB_ATTR
#endif
using namespace kl;
static const int NW = KL_WIDTH;
