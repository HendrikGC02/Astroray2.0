# SPDX-License-Identifier: GPL-3.0-or-later
# Node lowering table: bpy ShaderNode type name -> how the material compiler
# handles it. tools/opsgen.py joins it with ops.def and the 5.3 node list into
# the coverage matrix; a node type missing here is `unsupported` (magenta).
# translate.py lowers exactly the types listed in LOWERING (a test pins the two sets
# equal), so a lowering is one row here plus one function there. PLANNED lists the
# nodes that have no lowering yet but are not `unsupported` by design (0005 item 41);
# they render magenta until lowered, and the matrix says what they will be. A node in
# neither dict is `unsupported`; opsgen refuses a node in both.
# Ref: docs/decisions/0005-material-model.md items 40, 41, 42.
#
# Schema, read without executing this file:
#   "ShaderNodeFoo": {
#       "disposition": "implemented" | "approximated" | "ignored",
#       "ops": ["MUL_F32", ...],   # names from ops.def the lowering emits
#       "note": "free text without commas",
#   }
LOWERING = {
    "ShaderNodeBsdfPrincipled": {
        "disposition": "implemented",
        "ops": ["CONST_F32", "CONST_F32X3", "RGB_LIFT", "STORE_SLOT", "EMIT_LE", "COLOR_TO_F32", "MUL_F32", "BLACKBODY"],
        "note": "Base Color; Metallic as base_metalness; Roughness; Specular IOR Level as specular_weight 2L; Alpha as geometry_opacity; Emission; a colour linked to Metallic or Roughness or Specular IOR Level or Alpha or Emission Strength becomes its luminance (the level then times 2); Cycles' metal ignores Specular IOR Level and the engine's is scaled by 2L so Metallic above 0 with a level other than 0.5 is magenta (Specular IOR Level named); every other input at a non-default value or linked is magenta until M1b",
    },
    "ShaderNodeEmission": {
        "disposition": "implemented",
        "ops": ["CONST_F32", "CONST_F32X3", "STORE_SLOT", "EMIT_LE", "COLOR_TO_F32", "BLACKBODY"],
        "note": "Color times Strength into EMIT_LE with base_weight and specular_weight 0 so the material only emits",
    },
    "ShaderNodeMixShader": {
        "disposition": "implemented",
        "ops": ["BRANCH_MIX", "CONST_F32", "COLOR_TO_F32", "STORE_SLOT", "EMIT_LE"],
        "note": "two lowered shaders (Principled or Emission or Transparent or another Mix) selected by the factor with probability f for the second (never lerped); the factor is a constant or a float link; an unlinked shader input is magenta; as a Mix child an Emission is only its EMIT_LE",
    },
    "ShaderNodeBsdfTransparent": {
        "disposition": "implemented",
        "ops": ["CONST_F32", "STORE_SLOT"],
        "note": "geometry_opacity 1 minus the mean colour so white is opacity 0; a non-white colour is folded by its mean and the compiler reports it NODE_APPROXIMATED under a Mix; a linked colour is magenta",
    },
    "ShaderNodeBlackbody": {
        "disposition": "implemented",
        "ops": ["BLACKBODY"],
        "note": "unreachable in M1b: a constant Temperature is folded by Blender into RGB and a linked one is reported magenta; the lowering is kept for an emission colour only (Emission or Principled) and any other consumer is magenta",
    },
    "ShaderNodeTexImage": {
        "disposition": "implemented",
        "ops": ["TEX_IMAGE", "LOAD_CTX_UV"],
        "note": "Linear or Closest; Repeat or Extend; flat projection; Color output only",
    },
    "ShaderNodeTexEnvironment": {
        "disposition": "implemented",
        "ops": ["TEX_ENVIRONMENT", "LOAD_CTX_P"],
        "note": "equirectangular projection; Linear or Closest; unlinked Vector; Color output only; Mirror Ball and a linked Vector are magenta",
    },
    "ShaderNodeTexCoord": {
        "disposition": "implemented",
        "ops": ["LOAD_CTX_UV"],
        "note": "UV output only (UV set 0)",
    },
    "ShaderNodeUVMap": {
        "disposition": "implemented",
        "ops": ["LOAD_CTX_UV"],
        "note": "UV output of the default UV set only (UV set 0)",
    },
    "ShaderNodeOutputMaterial": {
        "disposition": "implemented",
        "ops": [],
        "note": "active output; Surface only (Volume and Displacement links are magenta)",
    },
    "ShaderNodeOutputWorld": {
        "disposition": "implemented",
        "ops": [],
        "note": "active output; Surface only (Volume links are magenta)",
    },
    "ShaderNodeBackground": {
        "disposition": "implemented",
        "ops": ["CONST_F32", "CONST_F32X3", "EMIT_LE"],
        "note": "Color (constant or an Environment Texture) times a constant Strength",
    },
}

# Not lowered yet; disposition "approximated" or "ignored" (same row schema, no ops yet).
PLANNED = {
    "ShaderNodeAmbientOcclusion": {
        "disposition": "approximated",
        "ops": [],
        "note": "no-op: Color passed through and AO = 1 (0005 item 41; owner decision 26)",
    },
    "ShaderNodeBevel": {
        "disposition": "approximated",
        "ops": [],
        "note": "no-op: the input normal passes through (0005 item 41; owner decision 26)",
    },
}
