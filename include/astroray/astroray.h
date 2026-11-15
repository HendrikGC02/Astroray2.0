/* SPDX-License-Identifier: Apache-2.0 */
/* Astroray C ABI. Restricted grammar of 0001 D2.10, checked by tools/abigen.py.
   Ref: docs/decisions/0001-product-shape.md D2.1-D2.11. */
#ifndef ASTRORAY_H
#define ASTRORAY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

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

/* Status codes. */
typedef uint32_t ar_status;
#define AR_OK 0
#define AR_ERR_INVALID_ARGUMENT 1
#define AR_ERR_UNSUPPORTED 2
#define AR_ERR_DEVICE 3
#define AR_ERR_OUT_OF_MEMORY 4
#define AR_ERR_ABI_MISMATCH 5
#define AR_ERR_INTERNAL 6

/* Host-assigned datablock key. */
typedef uint64_t ar_key;

/* Opaque handles. */
typedef struct ar_context_s* ar_context;
typedef struct ar_scene_s* ar_scene;
typedef struct ar_session_s* ar_session;
typedef struct ar_edit_s* ar_edit;

/* Device kinds. */
#define AR_DEVICE_CPU 0
#define AR_DEVICE_CUDA 1

/* Report levels. */
#define AR_LEVEL_INFO 0
#define AR_LEVEL_WARNING 1
#define AR_LEVEL_ERROR 2

/* Report codes, numbered in the table order of 0001 D2.11. */
#define AR_REPORT_SESSION_FAILED 1
#define AR_REPORT_POOL_CAPACITY 2
#define AR_REPORT_KERNEL_MODULE_FALLBACK 3
#define AR_REPORT_UNTESTED_DEVICE 4
#define AR_REPORT_GENERATION_REJECTED 5
#define AR_REPORT_IMAGE_UNREADABLE 6
#define AR_REPORT_COLORSPACE_UNSUPPORTED 7
#define AR_REPORT_MEMORY_SPILLED 8
#define AR_REPORT_SESSION_TRIMMED 9
#define AR_REPORT_NODE_UNSUPPORTED 10
#define AR_REPORT_NODE_APPROXIMATED 11
#define AR_REPORT_WORKING_SPACE_UNSUPPORTED 12
#define AR_REPORT_LARGE_WORLD_PRECISION 13
#define AR_REPORT_INPUT_IGNORED 14
#define AR_REPORT_NOT_ENERGY_CONSERVING 15
#define AR_REPORT_PROGRAM_REJECTED 16
#define AR_REPORT_GRATING_TABLE_LIMIT 17
#define AR_REPORT_SPECTRUM_OUT_OF_RANGE 18
#define AR_REPORT_RGB_ONLY_OUTSIDE_VISIBLE 19
#define AR_REPORT_WAVELENGTH_BINS_WIDENED 20
#define AR_REPORT_NONPHYSICAL_UNITS 21
#define AR_REPORT_BIASED_CONTROL 22
#define AR_REPORT_SCIENCE_REFUSED 23
#define AR_REPORT_SKY_DISC_NOT_SEPARABLE 24
#define AR_REPORT_CASTER_RAY_TYPE_DEPENDENT 25
#define AR_REPORT_DELTA_LIGHT_PATH_GAP 26
#define AR_REPORT_SHADOW_CATCHER_NO_LIGHT_PATHS 27
#define AR_REPORT_INVALID_SAMPLE 28
#define AR_REPORT_ZERO_COL_PIXELS 29
#define AR_REPORT_CRYPTOMATTE_OVERFLOW 30
#define AR_REPORT_DENOISER_UNAVAILABLE 31
#define AR_REPORT_DLSS_DOF_UNGUIDED 32
#define AR_REPORT_MAJORANT_VIOLATION 33
#define AR_REPORT_VOLUME_UNBOUNDED 34
#define AR_REPORT_PHASE_LOBES_EXCEEDED 35
#define AR_REPORT_MEDIUM_PATH_LIMIT 36
#define AR_REPORT_MEDIUM_MESH_OPEN 37
#define AR_REPORT_REGION_SCATTERING_AS_EXTINCTION 38
#define AR_REPORT_ERGOSPHERE_EXCLUDED 39
#define AR_REPORT_LINE_OMITTED 40
#define AR_REPORT_REGION_DELTA_BLOCKED 41
#define AR_REPORT_LIGHT_PATH_IN_REGION 42
typedef struct ar_context_desc {
    uint32_t device_kind;
    uint32_t device_index;
    AR_IN const char* log_path;
    AR_IN const char* record_path;
} ar_context_desc;

typedef struct ar_device_info {
    uint32_t kind;
    uint32_t index;
    uint32_t status;
    uint32_t compute_capability;
    uint64_t memory_bytes;
    uint8_t name[64];
    uint8_t reason[128];
} ar_device_info;

typedef struct ar_build_record {
    uint8_t version[32];
    uint8_t git_sha[48];
    uint32_t dirty;
    uint32_t pad0;
    uint8_t cuda[16];
    uint8_t optix[16];
    uint8_t embree[16];
    uint8_t oidn[16];
    uint8_t build_type[16];
    uint8_t kernel_source[264];
} ar_build_record;

typedef struct ar_struct_layout {
    uint8_t name[64];
    uint32_t size;
    uint32_t align;
    uint32_t field_count;
    uint32_t field_offset[128];
} ar_struct_layout;

typedef struct ar_report_entry {
    uint32_t code;
    uint32_t level;
    uint64_t subject;
    uint64_t first_generation;
    uint64_t last_generation;
    uint64_t count;
    double value[2];
    uint8_t detail[128];
} ar_report_entry;

typedef struct ar_report_info {
    uint64_t revision;
    uint32_t entry_count;
    uint32_t highest_level;
    uint32_t dropped;
    uint32_t pad0;
} ar_report_info;

/* Identity. */
AR_API AR_QUERY ar_status ar_abi_hash(AR_OUT uint64_t* hash);
AR_API AR_QUERY ar_status ar_abi_layout_count(AR_OUT uint32_t* count);
AR_API AR_QUERY ar_status ar_abi_layout(uint32_t index, AR_OUT ar_struct_layout* layout);
AR_API AR_QUERY ar_status ar_build_info(AR_OUT ar_build_record* info);

/* Devices and context. */
AR_API AR_QUERY ar_status ar_device_count(AR_OUT uint32_t* count);
AR_API AR_QUERY ar_status ar_device_get_info(uint32_t index, AR_OUT ar_device_info* info);
AR_API ar_status ar_context_create(AR_IN const ar_context_desc* desc, AR_OUT ar_context* out);
AR_API ar_status ar_context_destroy(ar_context ctx);
AR_API AR_QUERY ar_status ar_context_last_error(ar_context ctx, uint32_t capacity, AR_OUT_ARRAY(capacity, uint8_t) uint8_t* text);

#ifdef __cplusplus
}
#endif
#endif
