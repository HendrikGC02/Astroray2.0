// SPDX-License-Identifier: Apache-2.0
#pragma once
AR_NS_BEGIN
void a() { printf("x"); }
#ifdef AR_DEBUG
void b() { printf("y"); }
#else
void c() { printf("z"); }
#endif
void d() { fprintf(stderr, "w"); }
AR_NS_END
