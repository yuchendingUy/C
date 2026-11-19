"""Cycles materials for recorded draw states (Blender 5.x node API)."""
import math
import os

import bpy

SETTINGS_GROUP = "SceneRec Settings"

# LabPBR 1.3 predefined metals (green channel 230..237): complex IOR N and K per RGB channel,
# as listed in the LabPBR Material Standard (shaderLABS wiki).
LABPBR_METALS = [
    ("Iron", (2.9114, 2.9497, 2.5845), (3.0893, 2.9318, 2.7670)),
    ("Gold", (0.18299, 0.42108, 1.3734), (3.4242, 2.3459, 1.7704)),
    ("Aluminum", (1.3456, 0.96521, 0.61722), (7.4746, 6.3995, 5.3031)),
    ("Chrome", (3.1071, 3.1812, 2.3230), (3.3314, 3.3291, 3.1350)),
    ("Copper", (0.27105, 0.67693, 1.3164), (3.6092, 2.6248, 2.2921)),
    ("Lead", (1.9100, 1.8300, 1.4400), (3.5100, 3.4000, 3.1800)),
    ("Platinum", (2.3757, 2.0847, 1.8453), (4.2655, 3.7153, 3.1365)),
    ("Silver", (0.15943, 0.14512, 0.13547), (3.9291, 3.1900, 2.3808)),
]


def metal_f0(n, k):
    """Normal-incidence Fresnel reflectance of a conductor: ((n-1)^2 + k^2) / ((n+1)^2 + k^2)."""
    return tuple(((a - 1.0) ** 2 + b * b) / ((a + 1.0) ** 2 + b * b) for a, b in zip(n, k))


def ensure_settings_group():
    """A shared node group so emission strength etc. can be tuned for all materials at once."""
    g = bpy.data.node_groups.get(SETTINGS_GROUP)
    if g is not None:
        return g
    g = bpy.data.node_groups.new(SETTINGS_GROUP, "ShaderNodeTree")
    g.interface.new_socket("Emission Strength", in_out="OUTPUT", socket_type="NodeSocketFloat")
    g.interface.new_socket("Default Roughness", in_out="OUTPUT", socket_type="NodeSocketFloat")
    out = g.nodes.new("NodeGroupOutput")
    em = g.nodes.new("ShaderNodeValue")
    em.label = "Emission Strength (block light 15)"
    em.outputs[0].default_value = 8.0
    rough = g.nodes.new("ShaderNodeValue")
    rough.label = "Roughness without PBR"
    rough.outputs[0].default_value = 0.85
    em.location = (-200, 50)
    rough.location = (-200, -100)
    g.links.new(em.outputs[0], out.inputs[0])
    g.links.new(rough.outputs[0], out.inputs[1])
    return g


def load_image(path, non_color):
    if not path or not os.path.exists(path):
        return None
    img = bpy.data.images.load(path, check_existing=True)
    if non_color:
        img.colorspace_settings.name = "Non-Color"
    return img


class _B:
    """Tiny helper to place and link nodes."""

    def __init__(self, mat):
        self.nt = mat.node_tree
        self.nodes = self.nt.nodes
        self.links = self.nt.links
        self.x = -1400

    def node(self, kind, x, y, **props):
        n = self.nodes.new(kind)
        n.location = (x, y)
        for k, v in props.items():
            setattr(n, k, v)
        return n

    def link(self, a, b):
        self.links.new(a, b)

    def math(self, op, a, b=None, x=0, y=0, clamp=False):
        n = self.node("ShaderNodeMath", x, y, operation=op, use_clamp=clamp)
        self._in(n.inputs[0], a)
        if b is not None:
            self._in(n.inputs[1], b)
        return n.outputs[0]

    def madd(self, a, mul, add, x=0, y=0):
        """a * mul + add"""
        n = self.node("ShaderNodeMath", x, y, operation="MULTIPLY_ADD")
        self._in(n.inputs[0], a)
        self._in(n.inputs[1], mul)
        self._in(n.inputs[2], add)
        return n.outputs[0]

    def vmath(self, op, a, b=None, x=0, y=0):
        n = self.node("ShaderNodeVectorMath", x, y, operation=op)
        self._in(n.inputs[0], a)
        if b is not None:
            if op == "SCALE":
                self._in(n.inputs["Scale"], b)
            else:
                self._in(n.inputs[1], b)
        return n.outputs[0]

    def _in(self, sock, v):
        if isinstance(v, bpy.types.NodeSocket):
            self.link(v, sock)
        else:
            sock.default_value = v


def classify(state):
    """-> 'opaque' | 'cutout' | 'blend' | 'additive'."""
    rt = state["render_type"]
    tr = state["transparency"]
    if tr in ("additive_transparency", "lightning_transparency"):
        return "additive"
    if tr == "translucent_transparency" or state["shader"] == "particle":
        return "blend"
    if rt in ("solid", "entity_solid"):
        return "opaque"
    return "cutout"


def build_material(name, state, tex, tex_dir):
    """Creates the material for one draw state.

    tex: TEXTURE record dict (or None).  Geometry provides the colour attribute "Col" (vertex tint)
    and the float attribute "mc_emission" (block light emission 0..1).
    """
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    b = _B(mat)
    b.nodes.clear()
    kind = classify(state)

    out = b.node("ShaderNodeOutputMaterial", 900, 0)
    bsdf = b.node("ShaderNodeBsdfPrincipled", 500, 0)
    settings = b.node("ShaderNodeGroup", -1300, -700)
    settings.node_tree = ensure_settings_group()

    vcol = b.node("ShaderNodeVertexColor", -1300, 300, layer_name="Col")
    emis_attr = b.node("ShaderNodeAttribute", -1300, -500, attribute_name="mc_emission")

    color_img = load_image(os.path.join(tex_dir, tex["file"]), False) if tex else None
    if color_img is not None:
        timg = b.node("ShaderNodeTexImage", -1300, 600, image=color_img, interpolation="Closest")
        base = b.vmath("MULTIPLY", timg.outputs["Color"], vcol.outputs["Color"], x=-900, y=500)
        alpha = b.math("MULTIPLY", timg.outputs["Alpha"], vcol.outputs["Alpha"], x=-900, y=350)
    else:
        base = vcol.outputs["Color"]
        alpha = vcol.outputs["Alpha"]

    # ---- LabPBR
    n_img = load_image(os.path.join(tex_dir, tex["normal"]), True) if tex and tex.get("normal") else None
    s_img = load_image(os.path.join(tex_dir, tex["specular"]), True) if tex and tex.get("specular") else None
    m_img = load_image(os.path.join(tex_dir, tex["pbr_mask"]), True) if tex and tex.get("pbr_mask") else None

    roughness = settings.outputs["Default Roughness"]
    metallic = 0.0
    ior = 1.45
    sss = 0.0
    pbr_emission = None
    has_spec = None

    if kind == "blend" and s_img is None:
        roughness = 0.05  # water, glass, ice, slime... read better as smooth surfaces

    if n_img is not None:
        tn = b.node("ShaderNodeTexImage", -1300, -100, image=n_img, interpolation="Closest")
        sep = b.node("ShaderNodeSeparateColor", -1050, -100)
        b.link(tn.outputs["Color"], sep.inputs["Color"])
        # LabPBR normals are DirectX convention (green = down): flip Y for Blender (OpenGL).
        nx = b.madd(sep.outputs["Red"], 2.0, -1.0, x=-850, y=-50)
        ny = b.madd(sep.outputs["Green"], -2.0, 1.0, x=-850, y=-200)
        # z = sqrt(1 - x^2 - y^2)
        xx = b.math("MULTIPLY", nx, nx, x=-650, y=-50)
        yy = b.math("MULTIPLY", ny, ny, x=-650, y=-200)
        s = b.math("ADD", xx, yy, x=-500, y=-120)
        z = b.math("SQRT", b.math("MAXIMUM", b.math("SUBTRACT", 1.0, s, x=-380, y=-120), 0.0, x=-260, y=-120), x=-140, y=-120)
        comb = b.node("ShaderNodeCombineColor", 100, -100)
        b.link(b.madd(nx, 0.5, 0.5, x=-20, y=0), comb.inputs["Red"])
        b.link(b.madd(ny, 0.5, 0.5, x=-20, y=-150), comb.inputs["Green"])
        b.link(b.madd(z, 0.5, 0.5, x=-20, y=-300), comb.inputs["Blue"])
        nmap = b.node("ShaderNodeNormalMap", 250, -150, space="TANGENT", uv_map="st")
        b.link(comb.outputs["Color"], nmap.inputs["Color"])
        b.link(nmap.outputs["Normal"], bsdf.inputs["Normal"])
        # Blue = material AO (linear, 255 = none)
        base = b.vmath("SCALE", base, sep.outputs["Blue"], x=-600, y=550)

    if s_img is not None:
        ts = b.node("ShaderNodeTexImage", -1300, -300, image=s_img, interpolation="Closest")
        ss = b.node("ShaderNodeSeparateColor", -1050, -300)
        b.link(ts.outputs["Color"], ss.inputs["Color"])
        if m_img is not None:
            tm = b.node("ShaderNodeTexImage", -1300, -900, image=m_img, interpolation="Closest")
            has_spec = b.math("GREATER_THAN", tm.outputs["Color"], 0.5, x=-1050, y=-900)
        else:
            has_spec = 1.0
        # Red: perceptual smoothness -> roughness = (1 - s)^2
        r = b.math("POWER", b.math("SUBTRACT", 1.0, ss.outputs["Red"], x=-850, y=-300), 2.0, x=-700, y=-300)
        # Green (x255): 0..229 F0 (linear), 230..254 predefined metals, 255 albedo metal
        g255 = b.math("MULTIPLY", ss.outputs["Green"], 255.0, x=-850, y=-420)
        is_metal = b.math("GREATER_THAN", g255, 229.5, x=-700, y=-420)
        f0 = b.math("MINIMUM", b.math("DIVIDE", g255, 255.0, x=-700, y=-520), 0.9, x=-580, y=-520)
        sq = b.math("SQRT", f0, x=-460, y=-520)
        ior_s = b.math("DIVIDE", b.math("ADD", 1.0, sq, x=-340, y=-480), b.math("MAXIMUM", b.math("SUBTRACT", 1.0, sq, x=-340, y=-560), 0.001, x=-230, y=-560), x=-120, y=-520)
        # Predefined metal colours: F0 computed from the LabPBR N/K table
        ramp = b.node("ShaderNodeValToRGB", -600, -650)
        ramp.color_ramp.interpolation = "CONSTANT"
        els = ramp.color_ramp.elements
        els[0].position = 0.0
        els[0].color = (1, 1, 1, 1)
        els[1].position = 237.5 / 255.0
        els[1].color = (1, 1, 1, 1)
        for i, (_, nn, kk) in enumerate(LABPBR_METALS):
            e = els.new((229.5 + i) / 255.0)
            f = metal_f0(nn, kk)
            e.color = (f[0], f[1], f[2], 1.0)
        b.link(ss.outputs["Green"], ramp.inputs["Factor"])
        is_predef = b.math("MULTIPLY", is_metal, b.math("LESS_THAN", g255, 237.5, x=-580, y=-760), x=-460, y=-760)
        metal_base = b.vmath("ADD", base, b.vmath("SCALE", b.vmath("SUBTRACT", ramp.outputs["Color"], base, x=-350, y=-650), is_predef, x=-230, y=-650), x=-110, y=-650)
        # Blue: 65..255 subsurface scattering
        b255 = b.math("MULTIPLY", ss.outputs["Blue"], 255.0, x=-850, y=-800)
        sss_s = b.math("DIVIDE", b.math("MAXIMUM", b.math("SUBTRACT", b255, 65.0, x=-700, y=-800), 0.0, x=-580, y=-800), 190.0, x=-460, y=-800)
        # Alpha: emission 0..254, 255 = none
        a255 = b.math("MULTIPLY", ts.outputs["Alpha"], 255.0, x=-850, y=-950)
        pbr_emission = b.math("MULTIPLY", b.math("LESS_THAN", a255, 254.5, x=-700, y=-950), b.math("DIVIDE", a255, 254.0, x=-700, y=-1050), x=-560, y=-1000)

        def sel(pbr_value, default, x, y):
            # default + (pbr - default) * has_spec
            return b.math("ADD", default, b.math("MULTIPLY", b.math("SUBTRACT", pbr_value, default, x=x - 220, y=y), has_spec, x=x - 110, y=y), x=x, y=y)

        roughness = sel(r, roughness, 200, -300)
        metallic = b.math("MULTIPLY", is_metal, has_spec, x=200, y=-420)
        ior = sel(ior_s, 1.45, 200, -520)
        sss = b.math("MULTIPLY", sss_s, has_spec, x=200, y=-800)
        base = b.vmath("ADD", base, b.vmath("SCALE", b.vmath("SUBTRACT", metal_base, base, x=0, y=650), metallic, x=120, y=650), x=240, y=650)

    b._in(bsdf.inputs["Base Color"], base)
    b._in(bsdf.inputs["Roughness"], roughness)
    b._in(bsdf.inputs["Metallic"], metallic)
    b._in(bsdf.inputs["IOR"], ior)
    b._in(bsdf.inputs["Subsurface Weight"], sss)

    # ---- emission: block light emission from the geometry, or the PBR emission channel where the
    # texture pack provides a specular map for that pixel.
    vem = emis_attr.outputs["Factor"]
    if pbr_emission is not None:
        em = b.math("ADD", vem, b.math("MULTIPLY", b.math("SUBTRACT", pbr_emission, vem, x=-300, y=-1000), has_spec, x=-180, y=-1000), x=-60, y=-1000)
    else:
        em = vem
    b.link(base, bsdf.inputs["Emission Color"])
    b.link(b.math("MULTIPLY", em, settings.outputs["Emission Strength"], x=200, y=-1000), bsdf.inputs["Emission Strength"])

    # ---- transparency
    if kind == "cutout":
        b.link(b.math("GREATER_THAN", alpha, 0.1, x=-600, y=350), bsdf.inputs["Alpha"])
        b.link(bsdf.outputs["BSDF"], out.inputs["Surface"])
    elif kind == "blend":
        b._in(bsdf.inputs["Alpha"], alpha)
        b.link(bsdf.outputs["BSDF"], out.inputs["Surface"])
    elif kind == "additive":
        emis = b.node("ShaderNodeEmission", 500, 300)
        b.link(base, emis.inputs["Color"])
        b.link(b.math("MULTIPLY", alpha, settings.outputs["Emission Strength"], x=300, y=350), emis.inputs["Strength"])
        transp = b.node("ShaderNodeBsdfTransparent", 500, 450)
        add = b.node("ShaderNodeAddShader", 700, 350)
        b.link(emis.outputs[0], add.inputs[0])
        b.link(transp.outputs[0], add.inputs[1])
        b.link(add.outputs[0], out.inputs["Surface"])
    else:
        b.link(bsdf.outputs["BSDF"], out.inputs["Surface"])
    return mat
