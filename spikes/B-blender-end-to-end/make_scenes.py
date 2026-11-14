"""Procedural S1/S2 scenes for the sync + Cycles baselines. Run: blender -b --factory-startup --python make_scenes.py -- S1|S2
S1: one mesh, ~1.0M triangles (displaced grid with a UV map), 1 material.
S2: 2,000 unique meshes of ~2,520 triangles (5.0M unique triangles), 300 materials, 10,000 mesh instances:
    2,000 objects with their own mesh, 5,000 linked duplicates, 2,000 collection instances (empties),
    1,000 geometry-nodes instances (Instance on Points). Effective instanced triangles ~25M."""
import os, sys, time
import bpy, bmesh
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
which = sys.argv[sys.argv.index("--") + 1]
rng = np.random.default_rng(12345)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def add_camera_light(loc, target, lens=35, sun=True):
    cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
    bpy.context.scene.collection.objects.link(cam)
    cam.location = loc
    d = np.array(target) - np.array(loc)
    # look down -Z with +Y up: aim with a track-to constraint-free rotation
    from mathutils import Vector
    cam.rotation_euler = Vector(d.tolist()).to_track_quat('-Z', 'Y').to_euler()
    cam.data.lens = lens
    cam.data.clip_end = 5000
    bpy.context.scene.camera = cam
    if sun:
        l = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", 'SUN'))
        l.data.energy = 3.0
        l.rotation_euler = (0.6, 0.2, 0.4)
        bpy.context.scene.collection.objects.link(l)
    w = bpy.data.worlds.new("World")
    bpy.context.scene.world = w
    w.use_nodes = True
    w.node_tree.nodes["Background"].inputs[0].default_value = (0.3, 0.35, 0.4, 1)


def make_material(name, rgb):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.5
    return m


def bm_to_mesh(bm, name):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    return me


def add_uv(me):
    """Planar UV layer built with foreach_set (bmesh calc_uvs creates no layer without a prior layers.uv.new())."""
    co = np.empty(len(me.vertices) * 3, np.float32); me.vertices.foreach_get("co", co); co = co.reshape(-1, 3)
    vi = np.empty(len(me.loops), np.int32); me.loops.foreach_get("vertex_index", vi)
    uvl = me.uv_layers.new(name="UVMap")
    uvl.data.foreach_set("uv", (co[vi, :2] * 0.1 + 0.5).astype(np.float32).ravel())


def set_scene_res():
    s = bpy.context.scene
    s.render.resolution_x, s.render.resolution_y, s.render.resolution_percentage = 1920, 1080, 100


def build_s1():
    n = 707  # n*n quads -> 2*n*n = 999,698 triangles
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=n, y_segments=n, size=10.0, calc_uvs=True)
    me = bm_to_mesh(bm, "S1_mesh")
    bm.free()
    co = np.empty(len(me.vertices) * 3, np.float32)
    me.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    co[:, 2] = 0.4 * np.sin(co[:, 0] * 1.7) * np.cos(co[:, 1] * 1.3) + 0.05 * np.sin(co[:, 0] * 9) * np.sin(co[:, 1] * 11)
    me.vertices.foreach_set("co", co.ravel())
    me.update()
    me.polygons.foreach_set("use_smooth", np.ones(len(me.polygons), bool))
    add_uv(me)
    me.materials.append(make_material("S1_mat", (0.8, 0.4, 0.2)))
    ob = bpy.data.objects.new("S1_obj", me)
    bpy.context.scene.collection.objects.link(ob)
    add_camera_light((0, -16, 12), (0, 0, 0))


def sphere_template():
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=36, v_segments=36, radius=1.0, calc_uvs=True)
    co = np.array([v.co[:] for v in bm.verts], np.float32)
    faces = [[v.index for v in f.verts] for f in bm.faces]
    uvl = bm.loops.layers.uv.active
    bm.free()
    return co, faces


def build_s2():
    co0, faces = sphere_template()
    # triangle count after triangulation: quads*2 + tris
    ntri = sum(len(f) - 2 for f in faces)
    print("template tris", ntri, "verts", len(co0))
    mats = [make_material(f"M{i:03d}", rng.random(3).tolist()) for i in range(300)]
    col = bpy.context.scene.collection
    meshes, objs = [], []
    n_unique = 2000
    cols = 50  # layout: 50 x 40 grid for the unique objects
    t0 = time.time()
    for i in range(n_unique):
        scale = rng.uniform(0.6, 1.0, 3).astype(np.float32)
        noise = (rng.random(co0.shape).astype(np.float32) - 0.5) * 0.03
        co = co0 * scale + noise
        me = bpy.data.meshes.new(f"U{i:04d}")
        me.from_pydata(co.tolist(), [], faces)
        add_uv(me)
        me.materials.append(mats[i % 300])
        meshes.append(me)
        ob = bpy.data.objects.new(f"U{i:04d}", me)
        ob.location = ((i % cols) * 3.0, (i // cols) * 3.0, 0.0)
        col.objects.link(ob)
        objs.append(ob)
    print("unique meshes built in", round(time.time() - t0, 1), "s")
    # 5000 linked duplicates (share mesh data)
    for j in range(5000):
        src = int(rng.integers(0, n_unique))
        ob = bpy.data.objects.new(f"L{j:04d}", meshes[src])
        ob.location = (float(rng.uniform(0, cols * 3.0)), float(rng.uniform(0, 40 * 3.0)), float(rng.uniform(2.5, 8.0)))
        ob.rotation_euler = tuple(rng.uniform(0, 6.28, 3).tolist())
        col.objects.link(ob)
    # 2000 collection instances: 40 source collections of one linked-duplicate-like object each, excluded from view layer
    src_cols = []
    for k in range(40):
        c = bpy.data.collections.new(f"ISRC{k:02d}")
        bpy.context.scene.collection.children.link(c)
        o = bpy.data.objects.new(f"ISRCOBJ{k:02d}", meshes[(k * 37) % n_unique])
        c.objects.link(o)
        src_cols.append(c)
    for j in range(2000):
        e = bpy.data.objects.new(f"E{j:04d}", None)
        e.instance_type = 'COLLECTION'
        e.instance_collection = src_cols[j % 40]
        e.location = (float(rng.uniform(0, cols * 3.0)), float(rng.uniform(0, 40 * 3.0)), float(rng.uniform(9.0, 14.0)))
        col.objects.link(e)
    for c in src_cols:
        bpy.context.view_layer.layer_collection.children[c.name].exclude = True
    # 1000 GN instances: Instance on Points over a grid, instancing a small template object
    tmpl = bpy.data.objects.new("GNTEMPLATE", meshes[7])
    gcol = bpy.data.collections.new("GNSRC")
    bpy.context.scene.collection.children.link(gcol)
    gcol.objects.link(tmpl)
    bpy.context.view_layer.layer_collection.children["GNSRC"].exclude = True
    ng = bpy.data.node_groups.new("GNI", "GeometryNodeTree")
    ng.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    nout = ng.nodes.new("NodeGroupOutput")
    grid = ng.nodes.new("GeometryNodeMeshGrid")
    grid.inputs["Size X"].default_value = 90.0
    grid.inputs["Size Y"].default_value = 60.0
    grid.inputs["Vertices X"].default_value = 40
    grid.inputs["Vertices Y"].default_value = 25
    iop = ng.nodes.new("GeometryNodeInstanceOnPoints")
    oi = ng.nodes.new("GeometryNodeObjectInfo")
    oi.inputs["Object"].default_value = tmpl
    oi.transform_space = 'RELATIVE'
    oi.inputs["As Instance"].default_value = True
    ng.links.new(grid.outputs["Mesh"], iop.inputs["Points"])
    ng.links.new(oi.outputs["Geometry"], iop.inputs["Instance"])
    ng.links.new(iop.outputs["Instances"], nout.inputs[0])
    gob = bpy.data.objects.new("GNHOST", bpy.data.meshes.new("GNHOST"))
    md = gob.modifiers.new("GN", 'NODES')
    md.node_group = ng
    gob.location = (45.0, 60.0, 16.0)
    col.objects.link(gob)
    add_camera_light((75, -90, 70), (75, 60, 4), lens=30)


reset()
t0 = time.time()
if which == "S1":
    build_s1()
else:
    build_s2()
set_scene_res()
bpy.context.view_layer.update()
os.makedirs(os.path.join(HERE, "scenes"), exist_ok=True)
path = os.path.join(HERE, "scenes", f"{which}.blend")
bpy.ops.wm.save_as_mainfile(filepath=path, compress=False)
dg = bpy.context.evaluated_depsgraph_get()
n_inst = 0
tris = 0
mesh_inst = 0
for i in dg.object_instances:
    n_inst += 1
    o = i.object
    if o.type == 'MESH':
        mesh_inst += 1
        m = o.data
        m.calc_loop_triangles()
        tris += len(m.loop_triangles)
inst = range(n_inst)
uniq = {}
for me in bpy.data.meshes:
    if me.users:
        me.calc_loop_triangles()
        uniq[me.name] = len(me.loop_triangles)
print("SCENE", which, "built+saved in", round(time.time() - t0, 1), "s", path, os.path.getsize(path) // 2**20, "MB",
      "| object_instances", len(inst), "mesh instances", mesh_inst, "effective tris", tris,
      "| mesh datablocks", len(uniq), "unique tris", sum(uniq.values()), "materials", len(bpy.data.materials), "objects", len(bpy.data.objects))
