/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ASTRORAY_H
#define ASTRORAY_H
#include <stdint.h>
#ifdef _WIN32
#ifdef AR_BUILDING_CORE
#define AR_API __declspec(dllexport)
#else
#define AR_API __declspec(dllimport)
#endif
#else
#define AR_API
#endif
#define AR_QUERY
#define AR_IN
#define AR_OUT
#define AR_IN_ARRAY(n, T)
#define AR_OUT_ARRAY(n, T)
#define AR_STAGED(n, T)
typedef uint32_t ar_status;
typedef uint64_t ar_key;
typedef struct ar_context_s* ar_context;
typedef struct ar_x_desc {
    uint32_t f0;
    uint32_t f1;
    uint32_t f2;
    uint32_t f3;
    uint32_t f4;
    uint32_t f5;
    uint32_t f6;
    uint32_t f7;
    uint32_t f8;
    uint32_t f9;
    uint32_t f10;
    uint32_t f11;
    uint32_t f12;
    uint32_t f13;
    uint32_t f14;
    uint32_t f15;
    uint32_t f16;
    uint32_t f17;
    uint32_t f18;
    uint32_t f19;
    uint32_t f20;
    uint32_t f21;
    uint32_t f22;
    uint32_t f23;
    uint32_t f24;
    uint32_t f25;
    uint32_t f26;
    uint32_t f27;
    uint32_t f28;
    uint32_t f29;
    uint32_t f30;
    uint32_t f31;
    uint32_t f32;
    uint32_t f33;
    uint32_t f34;
    uint32_t f35;
    uint32_t f36;
    uint32_t f37;
    uint32_t f38;
    uint32_t f39;
    uint32_t f40;
    uint32_t f41;
    uint32_t f42;
    uint32_t f43;
    uint32_t f44;
    uint32_t f45;
    uint32_t f46;
    uint32_t f47;
    uint32_t f48;
    uint32_t f49;
    uint32_t f50;
    uint32_t f51;
    uint32_t f52;
    uint32_t f53;
    uint32_t f54;
    uint32_t f55;
    uint32_t f56;
    uint32_t f57;
    uint32_t f58;
    uint32_t f59;
    uint32_t f60;
    uint32_t f61;
    uint32_t f62;
    uint32_t f63;
    uint32_t f64;
    uint32_t f65;
    uint32_t f66;
    uint32_t f67;
    uint32_t f68;
    uint32_t f69;
    uint32_t f70;
    uint32_t f71;
    uint32_t f72;
    uint32_t f73;
    uint32_t f74;
    uint32_t f75;
    uint32_t f76;
    uint32_t f77;
    uint32_t f78;
    uint32_t f79;
    uint32_t f80;
    uint32_t f81;
    uint32_t f82;
    uint32_t f83;
    uint32_t f84;
    uint32_t f85;
    uint32_t f86;
    uint32_t f87;
    uint32_t f88;
    uint32_t f89;
    uint32_t f90;
    uint32_t f91;
    uint32_t f92;
    uint32_t f93;
    uint32_t f94;
    uint32_t f95;
    uint32_t f96;
    uint32_t f97;
    uint32_t f98;
    uint32_t f99;
    uint32_t f100;
    uint32_t f101;
    uint32_t f102;
    uint32_t f103;
    uint32_t f104;
    uint32_t f105;
    uint32_t f106;
    uint32_t f107;
    uint32_t f108;
    uint32_t f109;
    uint32_t f110;
    uint32_t f111;
    uint32_t f112;
    uint32_t f113;
    uint32_t f114;
    uint32_t f115;
    uint32_t f116;
    uint32_t f117;
    uint32_t f118;
    uint32_t f119;
    uint32_t f120;
    uint32_t f121;
    uint32_t f122;
    uint32_t f123;
    uint32_t f124;
    uint32_t f125;
    uint32_t f126;
    uint32_t f127;
    uint32_t f128;
} ar_x_desc;
#endif
