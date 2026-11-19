"""Builds a Blender scene from a Scene Recorder recording.

Layout of the result:
  * camera "SceneRec Camera" animated per frame (position, rotation, focal length), following
    first/third person exactly as recorded;
  * retained geometry (terrain sections, Sable sub-level sections, ...) as regular mesh objects.
    Static meshes with identical material and lifetime are merged; moving ones get transform
    keyframes. Visibility is keyed over each mesh's lifetime;
  * per-frame geometry (entities, particles, block entities, Create contraptions, the held item)
    is written to "<recording>/frames.usdc" as time-sampled meshes and imported through Blender's
    USD importer, which streams it from disk with a Mesh Sequence Cache modifier.

Recorded frame k is placed on Blender frame k + 1.
"""
import math
import os
import time

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector

from . import geom, materials, screc


def _log(msg):
    print("[SceneRec] " + msg)


# ---------------------------------------------------------------------------------------------
# pass 1: metadata, cameras, retained meshes


class Recording:
    def __init__(self, path):
        self.path = path
        self.dir = os.path.dirname(path)
        self.tex_dir = os.path.join(self.dir, "textures")
        self.formats = {}
        self.states = {}
        self.textures = {}
        self.sprites = {}
        self.sprite_emission = {}
        self.frames = []          # camera dicts in frame order
        self.retained = {}        # mesh id -> dict
        self.fps = 200

    def read_meta(self):
        import json
        meta = os.path.join(self.dir, "meta.json")
        if os.path.exists(meta):
            with open(meta, "r", encoding="utf-8") as f:
                m = json.load(f)
            self.fps = int(m.get("fps", 200))

    def pass1(self):
        frame = -1
        retained = self.retained
        for t, p in screc.iter_records(self.path):
            if t == screc.FORMAT:
                self.formats[p["id"]] = geom.Format(p)
            elif t == screc.STATE:
                self.states[p["id"]] = p
            elif t == screc.TEXTURE:
                self.textures[p["id"]] = p
            elif t == screc.SPRITES:
                self.sprites[p["texture"]] = p["sprites"]
            elif t == screc.SPRITE_EMISSION:
                for name, em, amb in p["entries"]:
                    if not amb:
                        self.sprite_emission[name] = em / 15.0
            elif t == screc.FRAME_BEGIN:
                frame = len(self.frames)
                self.frames.append(p)
            elif t == screc.MESH:
                if not p["transient"]:
                    retained[p["id"]] = {"format": p["format"], "mode": p["mode"], "vertex_count": p["vertex_count"],
                                         "data": p["data"], "draws": [], "intervals": [], "last": None}
            elif t == screc.DRAW:
                r = retained.get(p["mesh"])
                if r is not None:
                    key = (p["state"], p["pass"], p["translation"], p["matrix"])
                    if r["last"] != key:
                        r["draws"].append((frame, p["state"], p["pass"], p["translation"], p["matrix"]))
                        r["last"] = key
                    self._mark(r, frame)
            elif t == screc.DRAW_SAME:
                r = retained.get(p["mesh"])
                if r is not None:
                    self._mark(r, frame)
        # drop retained meshes that were never drawn
        for mid in [m for m, r in retained.items() if not r["draws"]]:
            del retained[mid]

    @staticmethod
    def _mark(r, frame):
        iv = r["intervals"]
        if iv and iv[-1][1] >= frame - 1:
            iv[-1][1] = frame
        else:
            iv.append([frame, frame])

    def sprite_lookup(self, tex_id):
        tex = self.textures.get(tex_id)
        if tex is None or not tex["atlas"] or tex_id not in self.sprites:
            return None
        if not hasattr(self, "_lookups"):
            self._lookups = {}
        if tex_id not in self._lookups:
            self._lookups[tex_id] = geom.SpriteLookup(tex["width"], tex["height"], self.sprites[tex_id])
        return self._lookups[tex_id]


# ---------------------------------------------------------------------------------------------
# helpers


def _fcurve(id_block, id_type, data_path, index, frames, values, interpolation):
    """Creates an F-Curve through the slotted action API (Blender 4.4+/5.x)."""
    from bpy_extras import anim_utils
    ad = id_block.animation_data or id_block.animation_data_create()
    if ad.action is None:
        action = bpy.data.actions.new(id_block.name + "Action")
        slot = action.slots.new(id_type=id_type, name=id_block.name)
        ad.action = action
        ad.action_slot = slot
    action = ad.action
    slot = ad.action_slot
    cb = anim_utils.action_ensure_channelbag_for_slot(action, slot)
    fc = cb.fcurves.new(data_path, index=index)
    n = len(frames)
    fc.keyframe_points.add(n)
    co = np.empty(2 * n, dtype=np.float32)
    co[0::2] = frames
    co[1::2] = values
    fc.keyframe_points.foreach_set("co", co)
    fc.keyframe_points.foreach_set("interpolation", np.full(n, interpolation, dtype=np.int32))
    fc.update()
    return fc


IPO_CONSTANT = 0
IPO_LINEAR = 1


def _visibility_keys(obj, intervals, total_frames):
    """Keys hide_render/hide_viewport so the object only shows on its lifetime intervals."""
    if len(intervals) == 1 and intervals[0][0] == 0 and intervals[0][1] >= total_frames - 1:
        return
    frames = []
    values = []
    if intervals[0][0] > 0:
        frames.append(1)
        values.append(1.0)
    for a, b in intervals:
        frames.append(a + 1)
        values.append(0.0)
        if b < total_frames - 1:
            frames.append(b + 2)
            values.append(1.0)
    for path in ("hide_render", "hide_viewport"):
        _fcurve(obj, "OBJECT", path, 0, frames, values, IPO_CONSTANT)


def _mesh_from_arrays(name, verts, counts, idx, uv, colors, emission):
    me = bpy.data.meshes.new(name)
    nv = len(verts)
    me.vertices.add(nv)
    me.vertices.foreach_set("co", verts.astype(np.float32).ravel())
    me.loops.add(len(idx))
    me.loops.foreach_set("vertex_index", idx.astype(np.int32))
    nf = len(counts)
    me.polygons.add(nf)
    starts = np.zeros(nf, dtype=np.int32)
    if nf > 1:
        starts[1:] = np.cumsum(counts[:-1])
    me.polygons.foreach_set("loop_start", starts)
    if uv is not None:
        uvl = me.uv_layers.new(name="st")
        luv = uv[idx].copy()
        luv[:, 1] = 1.0 - luv[:, 1]
        uvl.data.foreach_set("uv", luv.astype(np.float32).ravel())
    col = me.color_attributes.new("Col", "FLOAT_COLOR", "POINT")
    col.data.foreach_set("color", colors.astype(np.float32).ravel())
    em = me.attributes.new("mc_emission", "FLOAT", "POINT")
    em.data.foreach_set("value", emission.astype(np.float32))
    me.update()
    return me


def _is_static(draws):
    """True if all recorded transforms of a mesh are equal within rounding noise."""
    if len(draws) == 1:
        return True
    t = np.array([d[3] for d in draws], dtype=np.float64)
    m = np.array([d[4] for d in draws], dtype=np.float64)
    return np.abs(t - t[0]).max() < 1e-3 and np.abs(m - m[0]).max() < 1e-4


def _decode_retained(rec, r, state):
    fmt = rec.formats[r["format"]]
    dec = fmt.decode(r["data"], r["vertex_count"])
    counts, idx = geom.faces_for_mode(r["mode"], dec["n"])
    tex_id = state["texture"]
    em = geom.vertex_emission(dec, r["mode"], state, rec.sprite_lookup(tex_id), rec.sprite_emission)
    return dec, counts, idx, em


# ---------------------------------------------------------------------------------------------
# builder


class Builder:
    def __init__(self, context, rec, options):
        self.context = context
        self.rec = rec
        self.opt = options
        self.scene = context.scene
        self.materials = {}
        f0 = rec.frames[0]["pos"]
        self.origin = np.floor(np.array(f0, dtype=np.float64))
        self.total = len(rec.frames)
        name = os.path.basename(rec.dir) or "SceneRec"
        self.root = bpy.data.collections.new("SceneRec " + name)
        self.scene.collection.children.link(self.root)
        self.col_world = self._sub("World Geometry")
        self.col_moving = self._sub("Moving Geometry")
        self.col_hand = self._sub("Hand")

    def _sub(self, name):
        c = bpy.data.collections.new(name)
        self.root.children.link(c)
        return c

    def material(self, state_id):
        m = self.materials.get(state_id)
        if m is None:
            st = self.rec.states[state_id]
            tex = self.rec.textures.get(st["texture"])
            label = st["render_type"] or st["shader"] or "state"
            if tex is not None and tex["name"]:
                label += " " + tex["name"].split("/")[-1].replace(".png", "")
            m = materials.build_material("MC %d %s" % (state_id, label), st, tex, self.rec.tex_dir)
            self.materials[state_id] = m
        return m

    # ---- scene + camera

    def setup_scene(self):
        sc = self.scene
        sc.render.fps = self.rec.fps
        sc.render.fps_base = 1.0
        sc.frame_start = 1
        sc.frame_end = self.total
        aspect = self.rec.frames[0]["aspect"] or (16 / 9)
        sc.render.resolution_y = 1080
        sc.render.resolution_x = int(round(1080 * aspect / 2.0)) * 2
        sc["scenerec_origin"] = [float(v) for v in self.origin]
        if self.opt.get("set_cycles", True):
            sc.render.engine = "CYCLES"

    def build_camera(self):
        cam_data = bpy.data.cameras.new("SceneRec Camera")
        cam_data.sensor_fit = "VERTICAL"
        cam_data.clip_start = 0.05
        cam_data.clip_end = 2000.0
        cam = bpy.data.objects.new("SceneRec Camera", cam_data)
        self.root.objects.link(cam)
        cam.rotation_mode = "QUATERNION"
        frames = np.arange(1, self.total + 1, dtype=np.float32)
        n = self.total
        loc = np.empty((n, 3))
        quat = np.empty((n, 4))
        lens = np.empty(n)
        ctype = np.empty(n)
        Cm = Matrix(geom.C.tolist())
        prev = None
        for i, f in enumerate(self.rec.frames):
            p = np.array(f["pos"]) - self.origin
            loc[i] = (p[0], -p[2], p[1])
            qx, qy, qz, qw = f["rot"]
            r = Cm @ Quaternion((qw, qx, qy, qz)).to_matrix()
            q = r.to_quaternion()
            if prev is not None and prev.dot(q) < 0:
                q.negate()
            prev = q
            quat[i] = (q.w, q.x, q.y, q.z)
            fov = math.radians(max(1.0, min(170.0, f["fov"])))
            lens[i] = (cam_data.sensor_height * 0.5) / math.tan(fov * 0.5)
            ctype[i] = f["camera_type"]
        for k in range(3):
            _fcurve(cam, "OBJECT", "location", k, frames, loc[:, k], IPO_LINEAR)
        for k in range(4):
            _fcurve(cam, "OBJECT", "rotation_quaternion", k, frames, quat[:, k], IPO_LINEAR)
        _fcurve(cam_data, "CAMERA", "lens", 0, frames, lens, IPO_LINEAR)
        cam["scenerec_camera_type"] = "0=first person, 1=third person back, 2=third person front"
        self.scene.camera = cam
        return cam

    # ---- retained

    def build_retained(self):
        rec = self.rec
        static_groups = {}
        moving = []
        for mid, r in rec.retained.items():
            states = {(d[1], d[2]) for d in r["draws"]}
            if len(states) == 1 and _is_static(r["draws"]):
                static_groups.setdefault(next(iter(states)), []).append(mid)
            else:
                moving.append(mid)
        _log("retained meshes: %d static (%d materials), %d moving" % (sum(len(v) for v in static_groups.values()), len(static_groups), len(moving)))

        # All static meshes of one material become one object. Terrain sections are rebuilt while
        # recording (block changes, light updates, camera crossing section borders), so every face
        # carries the frame range of its mesh version and a geometry-nodes modifier removes faces
        # outside the current frame.
        lifetime = self._lifetime_group()
        for (state_id, ps), mids in static_groups.items():
            st = rec.states[state_id]
            vs, cs, ix, uvs, cols, ems, f_start, f_end = [], [], [], [], [], [], [], []
            base = 0
            has_uv = True
            whole = True
            for mid in mids:
                r = rec.retained[mid]
                dec, counts, idx, em = _decode_retained(rec, r, st)
                if dec["n"] == 0 or len(counts) == 0:
                    continue
                _, _, _, tr, m9 = r["draws"][0]
                verts = geom.separate_coplanar(geom.transform_points(dec["positions"], m9, tr, self.origin), counts, idx)
                for a, b in r["intervals"]:
                    if not (a == 0 and b >= self.total - 1):
                        whole = False
                    vs.append(verts)
                    cs.append(counts)
                    ix.append(idx + base)
                    base += dec["n"]
                    cols.append(dec["colors"])
                    ems.append(em)
                    if dec["uv"] is None:
                        has_uv = False
                    uvs.append(dec["uv"] if dec["uv"] is not None else np.zeros((dec["n"], 2), np.float32))
                    f_start.append(np.full(len(counts), a + 1, dtype=np.float32))
                    f_end.append(np.full(len(counts), b + 1, dtype=np.float32))
            if not vs:
                continue
            me = _mesh_from_arrays("MC %s %d" % (st["render_type"] or st["shader"], state_id), np.concatenate(vs), np.concatenate(cs),
                                   np.concatenate(ix), np.concatenate(uvs) if has_uv else None, np.concatenate(cols), np.concatenate(ems))
            me.materials.append(self.material(state_id))
            obj = bpy.data.objects.new(me.name, me)
            (self.col_hand if ps == screc.PASS_HAND else self.col_world).objects.link(obj)
            if not whole:
                a = me.attributes.new("scenerec_start", "FLOAT", "FACE")
                a.data.foreach_set("value", np.concatenate(f_start))
                a = me.attributes.new("scenerec_end", "FLOAT", "FACE")
                a.data.foreach_set("value", np.concatenate(f_end))
                mod = obj.modifiers.new("SceneRec Lifetime", "NODES")
                mod.node_group = lifetime

        for mid in moving:
            self._build_moving(mid)

    def _lifetime_group(self):
        name = "SceneRec Lifetime"
        g = bpy.data.node_groups.get(name)
        if g is not None:
            return g
        g = bpy.data.node_groups.new(name, "GeometryNodeTree")
        g.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
        g.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
        n = g.nodes
        gin = n.new("NodeGroupInput")
        gout = n.new("NodeGroupOutput")
        t = n.new("GeometryNodeInputSceneTime")
        sa = n.new("GeometryNodeInputNamedAttribute")
        sa.data_type = "FLOAT"
        sa.inputs["Name"].default_value = "scenerec_start"
        ea = n.new("GeometryNodeInputNamedAttribute")
        ea.data_type = "FLOAT"
        ea.inputs["Name"].default_value = "scenerec_end"
        before = n.new("FunctionNodeCompare")
        before.data_type = "FLOAT"
        before.operation = "LESS_THAN"
        after = n.new("FunctionNodeCompare")
        after.data_type = "FLOAT"
        after.operation = "GREATER_THAN"
        orn = n.new("FunctionNodeBooleanMath")
        orn.operation = "OR"
        delete = n.new("GeometryNodeDeleteGeometry")
        delete.domain = "FACE"
        L = g.links
        L.new(t.outputs["Frame"], before.inputs["A"])
        L.new(sa.outputs["Attribute"], before.inputs["B"])
        L.new(t.outputs["Frame"], after.inputs["A"])
        L.new(ea.outputs["Attribute"], after.inputs["B"])
        L.new(before.outputs["Result"], orn.inputs[0])
        L.new(after.outputs["Result"], orn.inputs[1])
        L.new(gin.outputs[0], delete.inputs["Geometry"])
        L.new(orn.outputs[0], delete.inputs["Selection"])
        L.new(delete.outputs["Geometry"], gout.inputs[0])
        for i, node in enumerate((gin, t, sa, ea, before, after, orn, delete, gout)):
            node.location = (i * 180 - 800, 0)
        return g

    def _build_moving(self, mid):
        rec = self.rec
        r = rec.retained[mid]
        state_id = r["draws"][0][1]
        st = rec.states[state_id]
        dec, counts, idx, em = _decode_retained(rec, r, st)
        if dec["n"] == 0 or len(counts) == 0:
            return
        local = geom.separate_coplanar(geom.to_blender(dec["positions"]), counts, idx)
        me = _mesh_from_arrays("MC moving %d" % mid, local, counts, idx, dec["uv"], dec["colors"], em)
        me.materials.append(self.material(state_id))
        obj = bpy.data.objects.new(me.name, me)
        ps = r["draws"][0][2]
        (self.col_hand if ps == screc.PASS_HAND else self.col_moving).objects.link(obj)
        obj.rotation_mode = "QUATERNION"

        # Transform keys: a key at each frame the transform changed, plus a hold key on the frame
        # before when the previous value was held for several frames.
        Cn = geom.C
        keys = []
        prev = None
        for fi, _st, _ps, tr, m9 in r["draws"]:
            m = np.asarray(m9, dtype=np.float64).reshape(3, 3)
            t = np.asarray(tr, dtype=np.float64) - self.origin
            mb = Cn @ m @ Cn.T
            tb = Cn @ t
            M = Matrix(((mb[0, 0], mb[0, 1], mb[0, 2], tb[0]),
                        (mb[1, 0], mb[1, 1], mb[1, 2], tb[1]),
                        (mb[2, 0], mb[2, 1], mb[2, 2], tb[2]),
                        (0, 0, 0, 1)))
            loc, rot, scale = M.decompose()
            if prev is not None:
                if prev[1].dot(rot) < 0:
                    rot.negate()
                if fi - 1 > prev[0]:
                    keys.append((fi - 1, prev[1].copy(), prev[2].copy(), prev[3].copy()))
            cur = (fi, rot, loc, scale)
            keys.append((fi, rot.copy(), loc.copy(), scale.copy()))
            prev = cur
        frames = np.array([k[0] + 1 for k in keys], dtype=np.float32)
        q = np.array([(k[1].w, k[1].x, k[1].y, k[1].z) for k in keys])
        l = np.array([tuple(k[2]) for k in keys])
        s = np.array([tuple(k[3]) for k in keys])
        for i in range(4):
            _fcurve(obj, "OBJECT", "rotation_quaternion", i, frames, q[:, i], IPO_LINEAR)
        for i in range(3):
            _fcurve(obj, "OBJECT", "location", i, frames, l[:, i], IPO_LINEAR)
            _fcurve(obj, "OBJECT", "scale", i, frames, s[:, i], IPO_LINEAR)
        _visibility_keys(obj, r["intervals"], self.total)

    # ---- per-frame geometry via USD

    def build_transient(self):
        from pxr import Sdf, Usd, UsdGeom, Vt

        rec = self.rec
        usd_path = os.path.join(rec.dir, "frames.usdc")
        if os.path.exists(usd_path):
            os.remove(usd_path)
        stage = Usd.Stage.CreateNew(usd_path)
        UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
        UsdGeom.SetStageMetersPerUnit(stage, 1.0)
        stage.SetTimeCodesPerSecond(rec.fps)
        stage.SetFramesPerSecond(rec.fps)
        stage.SetStartTimeCode(1)
        stage.SetEndTimeCode(self.total)
        root = UsdGeom.Xform.Define(stage, "/SceneRec")
        stage.SetDefaultPrim(root.GetPrim())

        groups = {}   # (state, pass) -> dict(prim attrs, last_nonempty)

        def group(key, frame_code):
            g = groups.get(key)
            if g is None:
                state_id, ps = key
                path = "/SceneRec/s%d_%s" % (state_id, "hand" if ps == screc.PASS_HAND else "world")
                mesh = UsdGeom.Mesh.Define(stage, path)
                mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
                pv = UsdGeom.PrimvarsAPI(mesh)
                g = {
                    "points": mesh.CreatePointsAttr(),
                    "counts": mesh.CreateFaceVertexCountsAttr(),
                    "indices": mesh.CreateFaceVertexIndicesAttr(),
                    "st": pv.CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex),
                    "col": pv.CreatePrimvar("Col", Sdf.ValueTypeNames.Color4fArray, UsdGeom.Tokens.vertex),
                    "em": pv.CreatePrimvar("mc_emission", Sdf.ValueTypeNames.FloatArray, UsdGeom.Tokens.vertex),
                    "nonempty": False,
                    "samples": 0,
                    "path": path,
                }
                groups[key] = g
                if frame_code > 1:
                    write_empty(g, frame_code - 1)
            return g

        empty3 = Vt.Vec3fArray()
        empty2 = Vt.Vec2fArray()
        empty4 = Vt.Vec4fArray()
        emptyf = Vt.FloatArray()
        emptyi = Vt.IntArray()

        def write_empty(g, code):
            g["points"].Set(empty3, code)
            g["counts"].Set(emptyi, code)
            g["indices"].Set(emptyi, code)
            g["st"].Set(empty2, code)
            g["col"].Set(empty4, code)
            g["em"].Set(emptyf, code)
            g["nonempty"] = False
            g["samples"] += 1

        transient = {}
        frame_buf = {}
        frame = -1
        t0 = time.time()
        for t, p in screc.iter_records(rec.path):
            if t == screc.FRAME_BEGIN:
                frame += 1
                frame_buf = {}
            elif t == screc.MESH:
                if p["transient"]:
                    transient[p["id"]] = p
            elif t == screc.DRAW:
                m = transient.pop(p["mesh"], None)
                if m is None:
                    continue
                st = rec.states[p["state"]]
                fmt = rec.formats[m["format"]]
                dec = fmt.decode(m["data"], m["vertex_count"])
                if dec["n"] == 0:
                    continue
                counts, idx = geom.faces_for_mode(m["mode"], dec["n"])
                if len(counts) == 0:
                    continue
                verts = geom.separate_coplanar(geom.transform_points(dec["positions"], p["matrix"], p["translation"], self.origin), counts, idx)
                em = geom.vertex_emission(dec, m["mode"], st, rec.sprite_lookup(st["texture"]), rec.sprite_emission)
                uv = dec["uv"] if dec["uv"] is not None else np.zeros((dec["n"], 2), np.float32)
                key = (p["state"], p["pass"])
                frame_buf.setdefault(key, []).append((verts, counts, idx, uv, dec["colors"], em))
            elif t == screc.FRAME_END:
                code = frame + 1
                for key, parts in frame_buf.items():
                    g = group(key, code)
                    base = 0
                    vs, cs, ix, uvs, cols, ems = [], [], [], [], [], []
                    for verts, counts, idx, uv, colors, em in parts:
                        vs.append(verts)
                        cs.append(counts)
                        ix.append(idx + base)
                        base += len(verts)
                        uvs.append(uv)
                        cols.append(colors)
                        ems.append(em)
                    uvc = np.concatenate(uvs).astype(np.float32)
                    uvc[:, 1] = 1.0 - uvc[:, 1]
                    g["points"].Set(Vt.Vec3fArray.FromNumpy(np.concatenate(vs).astype(np.float32)), code)
                    g["counts"].Set(Vt.IntArray.FromNumpy(np.concatenate(cs).astype(np.int32)), code)
                    g["indices"].Set(Vt.IntArray.FromNumpy(np.concatenate(ix).astype(np.int32)), code)
                    g["st"].Set(Vt.Vec2fArray.FromNumpy(uvc), code)
                    g["col"].Set(Vt.Vec4fArray.FromNumpy(np.concatenate(cols).astype(np.float32)), code)
                    g["em"].Set(Vt.FloatArray.FromNumpy(np.concatenate(ems).astype(np.float32)), code)
                    g["nonempty"] = True
                    g["samples"] += 1
                for key, g in groups.items():
                    if key not in frame_buf and g["nonempty"]:
                        write_empty(g, code)
                transient.clear()
                if frame % 200 == 0:
                    _log("frames.usdc: frame %d / %d (%.1fs)" % (frame, self.total, time.time() - t0))
        # A group sampled only once would be treated as static by the importer; end it explicitly.
        for g in groups.values():
            if g["samples"] < 2:
                write_empty(g, self.total + 1)
        stage.GetRootLayer().Save()
        del stage
        _log("wrote %s with %d groups" % (usd_path, len(groups)))
        if not groups:
            return

        before = set(bpy.data.objects)
        bpy.ops.wm.usd_import(filepath=usd_path, import_materials=False, set_frame_range=False,
                              read_mesh_uvs=True, read_mesh_colors=True, read_mesh_attributes=True,
                              relative_path=True, create_collection=False, import_cameras=False, import_lights=False)
        new_objs = [o for o in bpy.data.objects if o not in before]
        by_name = {}
        for (state_id, ps), g in groups.items():
            by_name[g["path"].rsplit("/", 1)[1]] = (state_id, ps)
        for o in new_objs:
            for c in list(o.users_collection):
                c.objects.unlink(o)
            key = by_name.get(o.name.split(".")[0])
            if key is None:
                if o.type == "EMPTY":
                    bpy.data.objects.remove(o)
                continue
            state_id, ps = key
            (self.col_hand if ps == screc.PASS_HAND else self.col_moving).objects.link(o)
            o.parent = None
            if o.type == "MESH":
                o.data.materials.clear()
                o.data.materials.append(self.material(state_id))
                st = self.rec.states[state_id]
                o.name = "MC frames %s %d%s" % (st["render_type"] or st["shader"], state_id, " hand" if ps == screc.PASS_HAND else "")


def import_recording(context, path, options=None):
    options = options or {}
    t0 = time.time()
    rec = Recording(path)
    rec.read_meta()
    _log("reading %s" % path)
    rec.pass1()
    if not rec.frames:
        raise RuntimeError("recording contains no frames")
    _log("%d frames, %d states, %d textures, %d retained meshes" % (len(rec.frames), len(rec.states), len(rec.textures), len(rec.retained)))
    b = Builder(context, rec, options)
    b.setup_scene()
    b.build_camera()
    b.build_retained()
    b.build_transient()
    context.scene.frame_set(1)
    _log("done in %.1fs" % (time.time() - t0))
    return b
