#pragma once
#include "stages.h"
struct OptixParams { kl::PathIO<4> io; const float* table; kl::LightTable<4> lt; int n; };
