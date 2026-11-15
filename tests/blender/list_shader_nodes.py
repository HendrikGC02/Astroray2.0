# SPDX-License-Identifier: Apache-2.0
# Writes every bpy.types.ShaderNode* type name of this Blender to
# tests/data/shader_node_types_5_3.txt (sorted, one per line).
# Run with Invoke-ArBlender (0009 d17a).
import os

import bpy

# Every ShaderNode* name, base class and tree included: 0009 d17a counts 108.
names = sorted(n for n in dir(bpy.types) if n.startswith("ShaderNode"))
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
path = os.path.join(root, "tests", "data", "shader_node_types_5_3.txt")
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(names) + "\n")
print(f"{len(names)} ShaderNode types, Blender {'.'.join(str(v) for v in bpy.app.version)}")
