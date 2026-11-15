# SPDX-License-Identifier: GPL-3.0-or-later
# Node lowering table: bpy ShaderNode type name -> how the material compiler
# handles it. tools/opsgen.py joins it with ops.def and the 5.3 node list into
# the coverage matrix; a node type missing here is `unsupported` (magenta).
# Real entries arrive with the lowerings (task 083).
# Ref: docs/decisions/0005-material-model.md item 40.
#
# Schema, read without executing this file:
#   "ShaderNodeFoo": {
#       "disposition": "implemented" | "approximated" | "ignored",
#       "ops": ["MUL_F32", ...],   # names from ops.def the lowering emits
#       "note": "free text without commas",
#   }
LOWERING = {}
