// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "core/real.h"
AR_NS_BEGIN
struct A { virtual void f(); };
void a() { throw 1; }
void b() { try { } catch (...) { } }
void c() { int* p = new int; delete p; }
void d() { void* p = malloc(4); free(p); }
__noinline__ void e();
long double f;
thread_local int g;
void h() { asm("nop"); }
AR_NS_END
