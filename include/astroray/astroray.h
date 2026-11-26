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
#define AR_REPORT_CAMERA_UNSUPPORTED 43
#define AR_REPORT_PASS_UNSUPPORTED 44
#define AR_REPORT_TEST_HOOK_ACTIVE 45
#define AR_REPORT_DEVICE_INIT_FAILED 46
#define AR_REPORT_MEDIUM_PRIORITY_TIE 47

/* Session mode, state and display mode (0007 decisions 5, 28, 40). */
#define AR_SESSION_MODE_VIEWPORT 0
#define AR_SESSION_MODE_FINAL 1
#define AR_SESSION_MODE_PREVIEW 2
#define AR_STATE_INITIALIZING 0
#define AR_STATE_RENDERING 1
#define AR_STATE_PAUSED 2
#define AR_STATE_CONVERGED 3
#define AR_STATE_TRIMMED 4
#define AR_STATE_CLOSED 5
#define AR_STATE_FAILED 6
#define AR_DISPLAY_BEAUTY 0
#define AR_DISPLAY_SAMPLE_COUNT 1
#define AR_DISPLAY_BOUNCE_COUNT 2
#define AR_DISPLAY_RELATIVE_ERROR 3
#define AR_DISPLAY_INVALID_SAMPLES 4
#define AR_DISPLAY_TERMINATION_REASON 5
#define AR_DISPLAY_TIME_PER_PIXEL 6
#define AR_DISPLAY_RAYS_PER_SAMPLE 7
#define AR_DISPLAY_ALBEDO 8
#define AR_DISPLAY_NORMAL 9
#define AR_DISPLAY_DEPTH 10
#define AR_DISPLAY_FORMAT_RGBA16F 0
#define AR_DISPLAY_FORMAT_DIAG_RGBA8 1
#define AR_DENOISER_NONE 0
#define AR_DENOISER_OIDN 1
#define AR_DENOISER_DLSS_RR 2

/* Graph targets, edit kinds, lights, images, spectra, geometry. */
#define AR_TARGET_SURFACE 0
#define AR_TARGET_VOLUME 1
#define AR_TARGET_DISPLACEMENT 2
#define AR_TARGET_LIGHT 3
#define AR_TARGET_WORLD 4
#define AR_KIND_GEOMETRY 0
#define AR_KIND_IMAGE 1
#define AR_KIND_MATERIAL 2
#define AR_KIND_GRAPH 3
#define AR_KIND_LIGHT 4
#define AR_KIND_ASTRO 5
#define AR_KIND_MEDIUM 6
#define AR_LIGHT_POINT 0
#define AR_LIGHT_SPOT 1
#define AR_LIGHT_SUN 2
#define AR_LIGHT_AREA_RECT 3
#define AR_LIGHT_AREA_ELLIPSE 4
#define AR_EMISSION_SAMPLING_NONE 0
#define AR_EMISSION_SAMPLING_AUTO 1
#define AR_EMISSION_SAMPLING_FRONT 2
#define AR_EMISSION_SAMPLING_BACK 3
#define AR_EMISSION_SAMPLING_FRONT_BACK 4
#define AR_SPECTRUM_RGB 0
#define AR_SPECTRUM_BLACKBODY 1
#define AR_SPECTRUM_TABLE 2
#define AR_IMAGE_SOURCE_FILE 0
#define AR_IMAGE_SOURCE_MEMORY 1
#define AR_IMAGE_SOURCE_PIXELS 2
#define AR_ALPHA_STRAIGHT 0
#define AR_ALPHA_PREMULTIPLIED 1
#define AR_ALPHA_CHANNEL_PACKED 2
#define AR_ALPHA_IGNORE 3
/* ar_image_desc.color_interop, in the order of 0007 d21; strings are Blender 5.3 interop ids. */
#define AR_COLOR_INTEROP_SRGB_REC709 0 /* srgb_rec709_display (5.3 sRGB), also srgb_rec709_scene */
#define AR_COLOR_INTEROP_LIN_REC709 1 /* lin_rec709_scene */
#define AR_COLOR_INTEROP_LIN_REC2020 2 /* lin_rec2020_scene */
#define AR_COLOR_INTEROP_LIN_AP1 3 /* lin_ap1_scene (ACEScg) */
#define AR_COLOR_INTEROP_LIN_AP0 4 /* lin_ap0_scene (ACES2065-1) */
#define AR_COLOR_INTEROP_SRGB_P3D65 5 /* srgb_p3d65_display (Display P3) */
#define AR_COLOR_INTEROP_DATA 6 /* data (Non-Color) */
#define AR_DOMAIN_POINT 0
#define AR_DOMAIN_CORNER 1
#define AR_DOMAIN_FACE 2
#define AR_DOMAIN_CURVE 3
#define AR_MESH_HAS_MATERIAL_INDEX 0x1
#define AR_MESH_HAS_ATTRIBUTES 0x2
#define AR_CURVE_LINEAR 0
#define AR_CURVE_CATMULL_ROM 1
#define AR_CURVE_BSPLINE 2
#define AR_VISIBILITY_CAMERA 0x1
#define AR_VISIBILITY_DIFFUSE 0x2
#define AR_VISIBILITY_GLOSSY 0x4
#define AR_VISIBILITY_TRANSMISSION 0x8
#define AR_VISIBILITY_SHADOW 0x10
#define AR_VISIBILITY_VOLUME 0x20
#define AR_INSTANCE_HOLDOUT 0x1
#define AR_INSTANCE_SHADOW_CATCHER 0x2
#define AR_INSTANCE_CASTER_OPT_OUT 0x4

/* Camera, astrophysics, settings, readback. */
#define AR_CAMERA_PERSP 0
#define AR_CAMERA_ORTHO 1
#define AR_CAMERA_PANO 2
#define AR_ASTRO_BLACK_HOLE 0
#define AR_FILTER_BOX 0
#define AR_FILTER_GAUSSIAN 1
#define AR_FILTER_BLACKMAN_HARRIS 2
#define AR_SAMPLER_SOBOL 0
#define AR_SAMPLER_INDEPENDENT 1
#define AR_STATISTICS_ON 0
#define AR_STATISTICS_OFF 1
#define AR_CRYPTO_OBJECT 0x1
#define AR_CRYPTO_MATERIAL 0x2
#define AR_CRYPTO_ASSET 0x4
#define AR_UNIT_ENERGY 0
#define AR_UNIT_PHOTON 1
#define AR_UNIT_NONE 2
#define AR_PASS_DENOISED 0x1
#define AR_QUANTITY_MEAN 0
#define AR_QUANTITY_VARIANCE 1
#define AR_QUANTITY_COUNT 2
#define AR_FITS_SCIENCE 0
#define AR_FITS_DUMP 1
#define AR_LEDGER_CATEGORY_COUNT 16

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

/* Scene, session, progress, settings. Field lists: docs/decisions/0007-appendix-abi.md. */
typedef struct ar_xform {
    float m[12];
} ar_xform;

typedef struct ar_rgba {
    float v[4];
} ar_rgba;

/* A UTF-8 name: a pass or shader AOV name (0008 decision 10). */
typedef struct ar_name {
    uint8_t text[64];
} ar_name;

typedef struct ar_scene_desc {
    AR_IN const char* cache_dir;
    uint32_t worker_threads;
    uint32_t pad0;
} ar_scene_desc;

typedef struct ar_session_desc {
    uint32_t mode;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t denoiser;
} ar_session_desc;

/* ledger_bytes is indexed in the order of the host ledger's categories (0007 decision 14), the session's
   ledger and its scene's device ledger summed; device_free_bytes and device_total_bytes are cuMemGetInfo's
   (0 on the CPU device). */
typedef struct ar_progress {
    uint32_t state;
    uint32_t samples_per_pixel;
    uint64_t active_generation;
    uint64_t pending_generation;
    uint64_t failed_generation;
    uint64_t epoch;
    float last_pass_ms;
    uint32_t pad0;
    uint64_t device_free_bytes;
    uint64_t device_total_bytes;
    uint64_t ledger_bytes[17];
} ar_progress;

/* A response channel: a tabulated curve (nm, value), piecewise linear (0008 decision 7). */
typedef struct ar_response_channel {
    uint8_t name[32];
    uint32_t unit;
    uint32_t curve_count;
    uint32_t cube_bins;
    uint32_t pad0;
    AR_IN_ARRAY(curve_count, float) const float* wavelength_nm;
    AR_IN_ARRAY(curve_count, float) const float* value;
} ar_response_channel;

typedef struct ar_settings {
    AR_IN_ARRAY(pass_count, ar_name) const ar_name* passes;
    AR_IN_ARRAY(aov_count, ar_name) const ar_name* aov_names;
    AR_IN_ARRAY(channel_count, ar_response_channel) const ar_response_channel* channels;
    uint32_t pass_count;
    uint32_t aov_count;
    uint32_t channel_count;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t resolution_percent;
    uint32_t border_enabled;
    float border_min_x;
    float border_min_y;
    float border_max_x;
    float border_max_y;
    uint32_t samples;
    uint32_t viewport_samples;
    float adaptive_threshold;
    uint32_t adaptive_min_samples;
    uint32_t max_bounces;
    uint32_t diffuse_bounces;
    uint32_t glossy_bounces;
    uint32_t transparent_bounces;
    uint32_t transmission_bounces;
    uint32_t volume_bounces;
    uint32_t min_bounces;
    uint32_t seed;
    uint32_t animated_seed;
    float clamp_direct;
    float clamp_indirect;
    float filter_glossy;
    uint32_t caustics_reflective;
    uint32_t caustics_refractive;
    uint32_t pixel_filter;
    float pixel_filter_width;
    float film_exposure;
    uint32_t film_transparent;
    uint32_t working_space;
    uint32_t light_group_count;
    uint32_t crypto_levels;
    uint32_t crypto_types;
    float exposure_time;
    float collecting_area;
    uint32_t denoise_start_sample;
    uint32_t allow_spill;
    uint32_t sampler_mode;
    uint32_t statistics;
} ar_settings;

/* Geometry, images, volumes. */
typedef struct ar_attribute_desc {
    uint8_t name[32];
    uint32_t domain;
    uint32_t width;
} ar_attribute_desc;

typedef struct ar_mesh_desc {
    ar_key key;
    uint32_t vertex_count;
    uint32_t corner_count;
    uint32_t face_count;
    uint32_t uv_layer_count;
    uint32_t motion_steps;
    uint32_t flags;
    uint32_t attribute_count;
    uint32_t pad0;
    uint8_t name[64];
    AR_IN_ARRAY(attribute_count, ar_attribute_desc) const ar_attribute_desc* attributes;
} ar_mesh_desc;

/* Each staged field points at an engine-owned slot that holds the array pointer; the
   engine also writes the element counts. Valid until the key's item commit or abort. */
typedef struct ar_mesh_buffers {
    uint64_t positions_count;
    uint64_t corner_vert_count;
    uint64_t face_start_count;
    uint64_t corner_normals_count;
    uint64_t uv_count;
    uint64_t material_index_count;
    uint64_t attributes_count;
    AR_STAGED(positions_count, float) float** positions;
    AR_STAGED(corner_vert_count, int32_t) int32_t** corner_vert;
    AR_STAGED(face_start_count, int32_t) int32_t** face_start;
    AR_STAGED(corner_normals_count, float) float** corner_normals;
    AR_STAGED(uv_count, float) float** uv;
    AR_STAGED(material_index_count, int32_t) int32_t** material_index;
    AR_STAGED(attributes_count, float) float** attributes;
} ar_mesh_buffers;

typedef struct ar_positions_buffers {
    uint64_t positions_count;
    uint64_t corner_normals_count;
    AR_STAGED(positions_count, float) float** positions;
    AR_STAGED(corner_normals_count, float) float** corner_normals;
} ar_positions_buffers;

/* Arrays of N entries use instance_count; xf holds motion_steps entries per instance. */
typedef struct ar_instances_desc {
    uint32_t instance_count;
    uint32_t motion_steps;
    uint32_t transform_count;
    uint32_t material_key_count;
    AR_IN_ARRAY(instance_count, ar_key) const ar_key* geom_keys;
    AR_IN_ARRAY(transform_count, ar_xform) const ar_xform* xf;
    AR_IN_ARRAY(instance_count, ar_key) const ar_key* object_keys;
    AR_IN_ARRAY(instance_count, uint32_t) const uint32_t* random_ids;
    AR_IN_ARRAY(instance_count, uint32_t) const uint32_t* pass_indices;
    AR_IN_ARRAY(instance_count, uint32_t) const uint32_t* crypto_objects;
    AR_IN_ARRAY(instance_count, uint32_t) const uint32_t* crypto_assets;
    AR_IN_ARRAY(instance_count, ar_rgba) const ar_rgba* colors;
    AR_IN_ARRAY(instance_count, uint32_t) const uint32_t* visibility;
    AR_IN_ARRAY(instance_count, uint32_t) const uint32_t* flags;
    AR_IN_ARRAY(instance_count, uint8_t) const uint8_t* light_groups;
    AR_IN_ARRAY(instance_count, uint32_t) const uint32_t* material_offsets;
    AR_IN_ARRAY(instance_count, uint32_t) const uint32_t* material_counts;
    AR_IN_ARRAY(material_key_count, ar_key) const ar_key* material_keys;
    /* A medium key per instance (0 = none), or NULL for no medium on any instance. The key must name a medium
       of this edit or the scene (ar_edit_set_medium) when the edit commits, else the commit fails. A mesh with a
       medium should be closed; the instance mask then carries the medium boundary bit whatever the visibility. */
    AR_IN_ARRAY(instance_count, ar_key) const ar_key* medium_keys;
} ar_instances_desc;

typedef struct ar_curves_desc {
    ar_key key;
    uint32_t point_count;
    uint32_t curve_count;
    uint32_t type;
    uint32_t attribute_count;
    AR_IN_ARRAY(attribute_count, ar_attribute_desc) const ar_attribute_desc* attributes;
} ar_curves_desc;

typedef struct ar_curves_buffers {
    uint64_t positions_count;
    uint64_t radius_count;
    uint64_t curve_start_count;
    uint64_t attributes_count;
    AR_STAGED(positions_count, float) float** positions;
    AR_STAGED(radius_count, float) float** radius;
    AR_STAGED(curve_start_count, int32_t) int32_t** curve_start;
    AR_STAGED(attributes_count, float) float** attributes;
} ar_curves_buffers;

typedef struct ar_points_desc {
    ar_key key;
    uint32_t point_count;
    uint32_t attribute_count;
    AR_IN_ARRAY(attribute_count, ar_attribute_desc) const ar_attribute_desc* attributes;
} ar_points_desc;

typedef struct ar_points_buffers {
    uint64_t positions_count;
    uint64_t radius_count;
    uint64_t attributes_count;
    AR_STAGED(positions_count, float) float** positions;
    AR_STAGED(radius_count, float) float** radius;
    AR_STAGED(attributes_count, float) float** attributes;
} ar_points_buffers;

typedef struct ar_grid_desc {
    ar_key key;
    uint8_t name[64];
    uint32_t channels;
    uint32_t leaf_count;
    float background[3];
    uint32_t pad0;
    ar_xform index_to_object;
    ar_xform object_to_world;
} ar_grid_desc;

typedef struct ar_grid_leaf_buffers {
    uint64_t leaf_origins_count;
    uint64_t voxels_count;
    AR_STAGED(leaf_origins_count, int32_t) int32_t** leaf_origins;
    AR_STAGED(voxels_count, float) float** voxels;
} ar_grid_leaf_buffers;

/* path is read for FILE; bytes for MEMORY; PIXELS go through ar_edit_image_begin, which stages
   width * height * 4 floats (RGBA, rows bottom-up) in ar_image_buffers; fill them, then ar_edit_item_commit. */
typedef struct ar_image_desc {
    ar_key key;
    uint32_t source;
    uint32_t color_interop;
    uint32_t alpha_mode;
    uint32_t udim_tile;
    uint32_t reload_count;
    uint32_t width;
    uint32_t height;
    uint64_t byte_count;
    AR_IN const char* path;
    AR_IN_ARRAY(byte_count, uint8_t) const uint8_t* bytes;
} ar_image_desc;

typedef struct ar_image_buffers {
    uint64_t pixels_count;
    AR_STAGED(pixels_count, float) float** pixels;
} ar_image_buffers;

/* Shading, lights, world, camera, astrophysics. */
typedef struct ar_graph_node {
    uint32_t op;
    uint32_t f32_first;
    uint32_t f32_count;
    uint32_t i32_first;
    uint32_t i32_count;
    uint32_t string_first;
    uint32_t string_count;
    uint32_t pad0;
} ar_graph_node;

typedef struct ar_graph_link {
    uint32_t from_node;
    uint32_t from_socket;
    uint32_t to_node;
    uint32_t to_socket;
} ar_graph_link;

/* string_bytes holds NUL-terminated UTF-8 strings back to back; string_first counts strings. */
typedef struct ar_graph_desc {
    uint32_t node_count;
    uint32_t link_count;
    uint32_t f32_count;
    uint32_t i32_count;
    uint32_t string_bytes_count;
    uint32_t pad0;
    AR_IN_ARRAY(node_count, ar_graph_node) const ar_graph_node* nodes;
    AR_IN_ARRAY(link_count, ar_graph_link) const ar_graph_link* links;
    AR_IN_ARRAY(f32_count, float) const float* params_f32;
    AR_IN_ARRAY(i32_count, int32_t) const int32_t* params_i32;
    AR_IN_ARRAY(string_bytes_count, uint8_t) const uint8_t* string_bytes;
} ar_graph_desc;

typedef struct ar_material_desc {
    ar_key surface_graph;
    ar_key volume_graph;
    ar_key displacement_graph;
    uint32_t ext_kind;
    uint32_t medium_priority;
    uint32_t pass_index;
    uint32_t crypto_material;
    uint32_t emission_sampling;
    uint32_t bump_correction;
    uint32_t displacement_method;
    uint32_t pad0;
    uint8_t ext_constants[64];
} ar_material_desc;

typedef struct ar_spectrum_ref {
    uint32_t kind;
    uint32_t table_count;
    float rgb[3];
    float temperature;
    float scale;
    uint32_t pad0;
    AR_IN_ARRAY(table_count, float) const float* table_wavelength_nm;
    AR_IN_ARRAY(table_count, float) const float* table_value;
} ar_spectrum_ref;

/* Sizes in metres. POINT and SPOT: size_x is the radius. SUN: none (sun_angle is the disc).
   AREA_RECT: size_x by size_y full extents. AREA_ELLIPSE: size_x and size_y are full extents
   (diameters), like Blender's lamp size; equal sizes are a disk, and the engine halves them
   into the ellipse record's radii. spread is in radians (pi = a full hemisphere), like Blender's RNA. */
typedef struct ar_light_desc {
    ar_spectrum_ref color;
    ar_key graph;
    ar_xform xf;
    uint32_t type;
    uint32_t visibility;
    float size_x;
    float size_y;
    float spot_size;
    float spot_blend;
    float sun_angle;
    float spread;
    float power;
    uint32_t normalize;
    uint32_t cast_shadow;
    uint32_t light_group;
} ar_light_desc;

/* A test medium of constant coefficients: sigma_a and sigma_s are RGB triples in 1/m (each finite and
   >= 0), g the Henyey-Greenstein anisotropy of the scattering (finite; beyond the engine's limit it is
   clamped and reported). flags is reserved and must be 0; any other value fails with AR_ERR_UNSUPPORTED. */
typedef struct ar_medium_desc {
    float sigma_a[3];
    float sigma_s[3];
    float g;
    uint32_t flags;
} ar_medium_desc;

typedef struct ar_world_desc {
    ar_key surface_graph;
    ar_key volume_graph;
    uint32_t light_group;
    float mist_start;
    float mist_depth;
    float mist_falloff;
} ar_world_desc;

/* motion_xf holds motion_steps transforms when motion_steps > 1; cam_to_world is then ignored. */
typedef struct ar_camera_desc {
    ar_xform cam_to_world;
    float projection_inverse[16];
    uint32_t type;
    float near_clip;
    float far_clip;
    uint32_t dof_enabled;
    float fstop;
    float focus_distance;
    uint32_t blades;
    float aperture_rotation;
    float aperture_ratio;
    uint32_t panorama_type;
    float fisheye_fov;
    float fisheye_lens;
    float latitude_min;
    float latitude_max;
    float longitude_min;
    float longitude_max;
    float shutter_open;
    float shutter_close;
    uint32_t motion_steps;
    uint32_t pad0;
    AR_IN_ARRAY(motion_steps, ar_xform) const ar_xform* motion_xf;
} ar_camera_desc;

typedef struct ar_view_desc {
    ar_camera_desc camera;
    uint32_t region_width;
    uint32_t region_height;
    float render_scale;
    uint32_t border_enabled;
    float border_min_x;
    float border_min_y;
    float border_max_x;
    float border_max_y;
} ar_view_desc;

typedef struct ar_astro_desc {
    uint32_t astro_kind;
    uint32_t pad0;
    ar_xform xf;
    double mass;
    double spin;
    double meters_per_rg;
    double theta_tol;
    double eps_nee;
} ar_astro_desc;

/* Display and readback. data is engine-owned and read-only for the host until release. */
typedef struct ar_display_frame {
    uint64_t frame_id;
    uint64_t generation;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t row_bytes;
    AR_OUT uint8_t* data;
} ar_display_frame;

typedef struct ar_pass_info {
    uint8_t name[64];
    uint32_t component_count;
    uint32_t unit;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t pad0;
} ar_pass_info;

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

/* Scene: host scene, its device copy, worker pool and generations (0007 decision 5). */
AR_API ar_status ar_scene_create(ar_context ctx, AR_IN const ar_scene_desc* desc, AR_OUT ar_scene* out);
AR_API ar_status ar_scene_trim(ar_scene scene);
/* Returns at once; the context frees the scene on its own thread, and ar_context_destroy waits for
   that (0007 decision 23). Later calls on the scene handle fail with AR_ERR_INVALID_ARGUMENT. An
   open ar_edit of the scene is aborted and its handle dies with the scene: never use it again. */
AR_API ar_status ar_scene_destroy(ar_scene scene);
AR_API AR_QUERY ar_status ar_scene_last_error(ar_scene scene, uint32_t capacity, AR_OUT_ARRAY(capacity, uint8_t) uint8_t* text);

/* Session: one render of one scene. */
AR_API ar_status ar_session_create(ar_scene scene, AR_IN const ar_session_desc* desc, AR_OUT ar_session* out);
AR_API ar_status ar_session_close(ar_session session);
AR_API ar_status ar_session_destroy(ar_session session);
AR_API ar_status ar_session_start(ar_session session);
AR_API ar_status ar_session_pause(ar_session session);
AR_API ar_status ar_session_resume(ar_session session);
AR_API ar_status ar_session_cancel(ar_session session);
AR_API ar_status ar_session_set_view(ar_session session, AR_IN const ar_view_desc* view);
AR_API ar_status ar_session_set_display_mode(ar_session session, uint32_t mode);
AR_API AR_QUERY ar_status ar_session_get_progress(ar_session session, AR_OUT ar_progress* progress);
AR_API ar_status ar_session_wait(ar_session session, uint32_t timeout_ms);
AR_API AR_QUERY ar_status ar_session_last_error(ar_session session, uint32_t capacity, AR_OUT_ARRAY(capacity, uint8_t) uint8_t* text);
AR_API AR_QUERY ar_status ar_session_report_info(ar_session session, AR_OUT ar_report_info* info);
AR_API AR_QUERY ar_status ar_session_report_entry(ar_session session, uint32_t index, AR_OUT ar_report_entry* entry);

/* Scene edit transaction: one open edit per scene. */
AR_API ar_status ar_scene_edit_begin(ar_scene scene, AR_OUT ar_edit* out);
AR_API ar_status ar_edit_reset(ar_edit edit);
AR_API ar_status ar_edit_mesh_begin(ar_edit edit, AR_IN const ar_mesh_desc* desc, AR_OUT ar_mesh_buffers* buffers);
AR_API ar_status ar_edit_positions_begin(ar_edit edit, ar_key geom, AR_OUT ar_positions_buffers* buffers);
AR_API ar_status ar_edit_curves_begin(ar_edit edit, AR_IN const ar_curves_desc* desc, AR_OUT ar_curves_buffers* buffers);
AR_API ar_status ar_edit_points_begin(ar_edit edit, AR_IN const ar_points_desc* desc, AR_OUT ar_points_buffers* buffers);
AR_API ar_status ar_edit_grid_leaves_begin(ar_edit edit, AR_IN const ar_grid_desc* desc, AR_OUT ar_grid_leaf_buffers* buffers);
AR_API ar_status ar_edit_image_begin(ar_edit edit, AR_IN const ar_image_desc* desc, AR_OUT ar_image_buffers* buffers);
AR_API ar_status ar_edit_set_image(ar_edit edit, AR_IN const ar_image_desc* desc);
AR_API ar_status ar_edit_item_commit(ar_edit edit, ar_key key);
AR_API ar_status ar_edit_remove(ar_edit edit, uint32_t kind, ar_key key);
AR_API ar_status ar_edit_set_instances(ar_edit edit, AR_IN const ar_instances_desc* desc);
AR_API ar_status ar_edit_update_transforms(ar_edit edit, uint32_t n, AR_IN_ARRAY(n, uint32_t) const uint32_t* idx, AR_IN_ARRAY(n, ar_xform) const ar_xform* xf);
/* set_graph and set_material commit their own item; a later ar_edit_item_commit on that key fails. */
AR_API ar_status ar_edit_set_graph(ar_edit edit, ar_key key, uint32_t target, AR_IN const ar_graph_desc* desc);
AR_API ar_status ar_edit_set_material(ar_edit edit, ar_key key, AR_IN const ar_material_desc* desc);
AR_API ar_status ar_edit_set_light(ar_edit edit, ar_key key, AR_IN const ar_light_desc* desc);
/* Creates the medium under key, or replaces it; the old one stays in effect until the edit commits.
   Remove it with ar_edit_remove(AR_KIND_MEDIUM). Like set_light it stages and commits its own item. */
AR_API ar_status ar_edit_set_medium(ar_edit edit, ar_key key, AR_IN const ar_medium_desc* desc);
AR_API ar_status ar_edit_set_world(ar_edit edit, AR_IN const ar_world_desc* desc);
AR_API ar_status ar_edit_set_camera(ar_edit edit, AR_IN const ar_camera_desc* desc);
AR_API ar_status ar_edit_set_settings(ar_edit edit, AR_IN const ar_settings* settings);
AR_API ar_status ar_edit_set_astro(ar_edit edit, ar_key key, AR_IN const ar_astro_desc* desc);
AR_API ar_status ar_edit_commit(ar_edit edit, AR_OUT uint64_t* generation);
AR_API ar_status ar_edit_abort(ar_edit edit);
AR_API AR_QUERY ar_status ar_edit_last_error(ar_edit edit, uint32_t capacity, AR_OUT_ARRAY(capacity, uint8_t) uint8_t* text);

/* Display and readback: all AR_QUERY, never recorded (0008 decision 13a). */
AR_API AR_QUERY ar_status ar_display_acquire(ar_session session, uint64_t after_frame, AR_OUT ar_display_frame* out);
AR_API AR_QUERY ar_status ar_display_release(ar_session session, uint64_t frame_id);
AR_API AR_QUERY ar_status ar_session_pass_count(ar_session session, AR_OUT uint32_t* count);
AR_API AR_QUERY ar_status ar_session_pass_info(ar_session session, uint32_t index, AR_OUT ar_pass_info* info);
AR_API AR_QUERY ar_status ar_session_read_pass(ar_session session, uint32_t index, uint32_t quantity, uint32_t n, AR_OUT_ARRAY(n, float) float* dst);
AR_API AR_QUERY ar_status ar_session_read_counters(ar_session session, uint32_t n, AR_OUT_ARRAY(n, uint64_t) uint64_t* dst);
AR_API AR_QUERY ar_status ar_session_write_fits(ar_session session, AR_IN const char* path, uint32_t what);

#ifdef __cplusplus
}
#endif
#endif
