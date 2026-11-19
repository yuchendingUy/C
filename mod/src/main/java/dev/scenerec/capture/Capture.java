package dev.scenerec.capture;

import com.mojang.blaze3d.shaders.Uniform;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vertex.MeshData;
import com.mojang.blaze3d.vertex.VertexBuffer;
import com.mojang.blaze3d.vertex.VertexFormat;
import com.mojang.blaze3d.vertex.VertexFormatElement;
import dev.scenerec.SceneRec;
import dev.scenerec.io.Rec;
import dev.scenerec.io.RecordBuffer;
import dev.scenerec.mixin.access.ShaderInstanceAccess;
import dev.scenerec.mixin.access.VertexBufferAccess;
import net.minecraft.client.Camera;
import net.minecraft.client.renderer.RenderType;
import net.minecraft.client.renderer.ShaderInstance;
import net.minecraft.world.phys.Vec3;
import org.joml.Matrix4f;
import org.joml.Quaternionf;
import org.joml.Vector3f;
import org.lwjgl.opengl.GL15;
import org.lwjgl.system.MemoryUtil;

import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.Objects;

/**
 * Captures geometry at the point where Minecraft hands it to OpenGL.
 *
 * <p>Every piece of world geometry in vanilla (terrain sections, entities, block entities,
 * particles, weather, the held item, ...) is drawn through {@link VertexBuffer#draw()}, either
 * from a retained buffer (terrain sections, also Sable's sub-level sections) or from the shared
 * "immediate" buffer of a vertex format (everything built per frame through {@code BufferUploader}).
 * We copy the vertex bytes when a buffer is uploaded and, at each draw, record which mesh was
 * drawn with which transform. The transform is recovered generically from the uniforms that
 * vanilla core shaders apply ({@code ModelViewMat} and {@code ChunkOffset}):
 * {@code world = cameraPos + inverse(viewRotation) * ModelViewMat * (vertex + ChunkOffset)}.
 * This is how a mod that moves geometry by changing those uniforms (e.g. Sable sub-levels) is
 * captured without being special-cased.</p>
 *
 * <p>All methods must be called on the render thread.</p>
 */
public final class Capture {
    private Capture() {
    }

    private static final class VbState {
        int meshId = -1;
        boolean immediate;
        int lastStateId = -1;
        final float[] lastXform = new float[12];
        final double[] lastTrans = new double[3];
        int lastPass = -1;
    }

    private static final Map<VertexBuffer, VbState> STATES = new IdentityHashMap<>();
    private static final Map<VertexFormat, Integer> FORMATS = new IdentityHashMap<>();
    private static final Map<String, Integer> DRAW_STATES = new HashMap<>();

    private static RecordBuffer out;
    private static int nextMeshId;
    private static int nextFormatId;
    private static int nextStateId;

    /** True between the start and end of a recorded frame's world rendering. */
    private static boolean inFrame;
    private static boolean frameHeaderWritten;
    private static int suppressDepth;
    private static int pass = Rec.PASS_WORLD;

    private static final Matrix4f FRUSTUM_INV = new Matrix4f();
    private static double camX, camY, camZ;

    private static RenderType currentRenderType;

    private static final Matrix4f TMP_MV = new Matrix4f();
    private static final Matrix4f TMP_M = new Matrix4f();
    private static final Vector3f TMP_V = new Vector3f();

    public static void begin(RecordBuffer buffer) {
        out = buffer;
        STATES.clear();
        FORMATS.clear();
        DRAW_STATES.clear();
        nextMeshId = 1;
        nextFormatId = 1;
        nextStateId = 1;
        inFrame = false;
        suppressDepth = 0;
        pass = Rec.PASS_WORLD;
    }

    public static void end() {
        out = null;
        STATES.clear();
        FORMATS.clear();
        DRAW_STATES.clear();
        inFrame = false;
    }

    public static boolean active() {
        return out != null;
    }

    // ------------------------------------------------------------------ frame window

    public static void beginFrame() {
        if (out == null) {
            return;
        }
        inFrame = true;
        frameHeaderWritten = false;
        pass = Rec.PASS_WORLD;
        suppressDepth = 0;
    }

    public static boolean inFrame() {
        return inFrame;
    }

    /**
     * Called at the head of {@code LevelRenderer.renderLevel} with the camera and the view rotation
     * matrix ("frustumMatrix") used for this frame.
     */
    public static void frameCamera(Camera camera, Matrix4f frustumMatrix, int frameIndex, double gameTimeTicks,
                                   float fovDeg, float aspect, int cameraType) {
        if (!inFrame || frameHeaderWritten) {
            return;
        }
        frameHeaderWritten = true;
        Vec3 p = camera.getPosition();
        camX = p.x;
        camY = p.y;
        camZ = p.z;
        frustumMatrix.invert(FRUSTUM_INV);
        Quaternionf q = camera.rotation();
        out.u8(Rec.FRAME_BEGIN)
            .i32(frameIndex)
            .f64(gameTimeTicks)
            .f64(camX).f64(camY).f64(camZ)
            .f32(q.x).f32(q.y).f32(q.z).f32(q.w)
            .f32(fovDeg)
            .f32(aspect)
            .u8(cameraType);
    }

    public static void endFrame(int frameIndex) {
        if (!inFrame) {
            return;
        }
        if (frameHeaderWritten) {
            out.u8(Rec.FRAME_END).i32(frameIndex);
        }
        inFrame = false;
        frameHeaderWritten = false;
    }

    public static void pushSuppress() {
        suppressDepth++;
    }

    public static void popSuppress() {
        if (suppressDepth > 0) {
            suppressDepth--;
        }
    }

    public static void setPass(int p) {
        pass = p;
    }

    public static void setCurrentRenderType(RenderType type) {
        currentRenderType = type;
    }

    public static void clearCurrentRenderType(RenderType type) {
        if (currentRenderType == type) {
            currentRenderType = null;
        }
    }

    // ------------------------------------------------------------------ buffers

    private static boolean isImmediate(VertexBuffer vb, VertexFormat format) {
        return format != null && vb == format.getImmediateDrawVertexBuffer();
    }

    private static int formatId(VertexFormat format) {
        Integer id = FORMATS.get(format);
        if (id != null) {
            return id;
        }
        int nid = nextFormatId++;
        FORMATS.put(format, nid);
        out.u8(Rec.FORMAT).i32(nid).i32(format.getVertexSize());
        var elements = format.getElements();
        out.u8(elements.size());
        for (VertexFormatElement e : elements) {
            out.str(format.getElementName(e))
                .str(e.usage().name())
                .str(e.type().name())
                .u8(e.count())
                .i32(format.getOffset(e));
        }
        return nid;
    }

    private static void writeMesh(VbState st, VertexFormat format, VertexFormat.Mode mode, int vertexCount, ByteBuffer data, int byteLen) {
        if (st.meshId >= 0 && !st.immediate) {
            out.u8(Rec.MESH_END).i32(st.meshId);
        }
        int fid = formatId(format);
        int id = nextMeshId++;
        out.u8(Rec.MESH)
            .i32(id)
            .i32(fid)
            .str(mode.name())
            .u8(st.immediate ? 1 : 0)
            .i32(vertexCount)
            .i32(byteLen)
            .bytes(data, byteLen);
        st.meshId = id;
        st.lastStateId = -1;
        st.lastPass = -1;
    }

    /** Hook: head of {@link VertexBuffer#upload(MeshData)}. */
    public static void onUpload(VertexBuffer vb, MeshData meshData) {
        if (out == null || vb.isInvalid()) {
            return;
        }
        MeshData.DrawState ds = meshData.drawState();
        VertexFormat format = ds.format();
        VbState st = STATES.computeIfAbsent(vb, k -> new VbState());
        st.immediate = isImmediate(vb, format);
        if (st.immediate && (!inFrame || suppressDepth > 0)) {
            // GUI or other non-world immediate geometry: never recorded.
            st.meshId = -1;
            return;
        }
        if (!isRecordableMode(ds.mode())) {
            if (!st.immediate && st.meshId >= 0) {
                out.u8(Rec.MESH_END).i32(st.meshId);
            }
            st.meshId = -1;
            return;
        }
        ByteBuffer vbuf = meshData.vertexBuffer();
        int byteLen = ds.vertexCount() * format.getVertexSize();
        writeMesh(st, format, ds.mode(), ds.vertexCount(), vbuf, Math.min(byteLen, vbuf.remaining()));
    }

    /** Hook: head of {@link VertexBuffer#close()}. */
    public static void onClose(VertexBuffer vb) {
        VbState st = STATES.remove(vb);
        if (out != null && st != null && st.meshId >= 0 && !st.immediate) {
            out.u8(Rec.MESH_END).i32(st.meshId);
        }
    }

    private static boolean isRecordableMode(VertexFormat.Mode mode) {
        return mode == VertexFormat.Mode.QUADS
            || mode == VertexFormat.Mode.TRIANGLES
            || mode == VertexFormat.Mode.TRIANGLE_STRIP
            || mode == VertexFormat.Mode.TRIANGLE_FAN;
    }

    /**
     * A retained buffer that was uploaded before the recording started (or outside of a frame) has
     * no CPU copy; read its vertex data back from the GPU once.
     */
    private static boolean readBack(VertexBuffer vb, VbState st) {
        VertexBufferAccess acc = (VertexBufferAccess) vb;
        VertexFormat format = acc.scenerec$getFormat();
        VertexFormat.Mode mode = acc.scenerec$getMode();
        if (format == null || mode == null || !isRecordableMode(mode)) {
            return false;
        }
        int glId = acc.scenerec$getVertexBufferId();
        if (glId <= 0) {
            return false;
        }
        int vsize = format.getVertexSize();
        int vertexCount = expectedVertexCount(mode, acc.scenerec$getIndexCount());
        GL15.glBindBuffer(GL15.GL_ARRAY_BUFFER, glId);
        int bufSize = GL15.glGetBufferParameteri(GL15.GL_ARRAY_BUFFER, GL15.GL_BUFFER_SIZE);
        vertexCount = Math.min(vertexCount, bufSize / vsize);
        if (vertexCount <= 0) {
            GL15.glBindBuffer(GL15.GL_ARRAY_BUFFER, 0);
            return false;
        }
        ByteBuffer tmp = MemoryUtil.memAlloc(vertexCount * vsize);
        try {
            GL15.glGetBufferSubData(GL15.GL_ARRAY_BUFFER, 0, tmp);
            GL15.glBindBuffer(GL15.GL_ARRAY_BUFFER, 0);
            st.immediate = false;
            writeMesh(st, format, mode, vertexCount, tmp, vertexCount * vsize);
        } finally {
            MemoryUtil.memFree(tmp);
        }
        return true;
    }

    private static int expectedVertexCount(VertexFormat.Mode mode, int indexCount) {
        return switch (mode) {
            case QUADS -> indexCount / 6 * 4;
            default -> indexCount;
        };
    }

    // ------------------------------------------------------------------ draws

    /** Hook: head of {@link VertexBuffer#draw()}. */
    public static void onDraw(VertexBuffer vb) {
        if (out == null || !inFrame || !frameHeaderWritten || suppressDepth > 0) {
            return;
        }
        VbState st = STATES.get(vb);
        if (st == null) {
            st = new VbState();
            STATES.put(vb, st);
            VertexBufferAccess acc = (VertexBufferAccess) vb;
            st.immediate = isImmediate(vb, acc.scenerec$getFormat());
        }
        if (st.meshId < 0) {
            if (st.immediate || !readBack(vb, st)) {
                return;
            }
        }

        RenderType rt = currentRenderType;
        String rtName = rt != null ? rt.name : "";
        if (RenderTypeInfo.isSkipped(rtName)) {
            return;
        }

        ShaderInstance shader = ShaderInstanceAccess.scenerec$getLastAppliedShader();
        if (shader == null) {
            shader = RenderSystem.getShader();
        }

        // ---- transform
        Matrix4f mv = TMP_MV;
        Uniform mvU = shader != null ? shader.MODEL_VIEW_MATRIX : null;
        if (mvU != null) {
            mv.set(0, mvU.getFloatBuffer());
        } else {
            mv.set(RenderSystem.getModelViewMatrix());
        }
        float cox = 0, coy = 0, coz = 0;
        Uniform coU = shader != null ? shader.CHUNK_OFFSET : null;
        if (coU != null) {
            var fb = coU.getFloatBuffer();
            cox = fb.get(0);
            coy = fb.get(1);
            coz = fb.get(2);
        }
        Matrix4f m = FRUSTUM_INV.mul(mv, TMP_M);
        // world = cam + M * (p + CO)  ->  linear part M3, translation cam + M*(CO)
        Vector3f t = m.transformPosition(cox, coy, coz, TMP_V);
        double tx = camX + t.x;
        double ty = camY + t.y;
        double tz = camZ + t.z;
        float[] x = new float[]{
            m.m00(), m.m10(), m.m20(),
            m.m01(), m.m11(), m.m21(),
            m.m02(), m.m12(), m.m22(),
            0, 0, 0
        };

        // ---- material state
        int texGl = shader != null ? sampler0(shader) : RenderSystem.getShaderTexture(0);
        int texId = Textures.onTextureUsed(texGl);
        String shaderName = shader != null ? shader.getName() : "";
        String transparency = RenderTypeInfo.transparency(rt);
        int stateId = drawState(rtName, shaderName, texId, transparency);

        float[] col = RenderSystem.getShaderColor();
        Uniform cm = shader != null ? shader.COLOR_MODULATOR : null;
        float cr = col[0], cg = col[1], cb = col[2], ca = col[3];
        if (cm != null) {
            var fb = cm.getFloatBuffer();
            cr = fb.get(0);
            cg = fb.get(1);
            cb = fb.get(2);
            ca = fb.get(3);
        }

        if (!st.immediate && st.lastStateId == stateId && st.lastPass == pass
            && st.lastTrans[0] == tx && st.lastTrans[1] == ty && st.lastTrans[2] == tz
            && sameXform(st.lastXform, x)) {
            out.u8(Rec.DRAW_SAME).i32(st.meshId);
            return;
        }
        st.lastStateId = stateId;
        st.lastPass = pass;
        st.lastTrans[0] = tx;
        st.lastTrans[1] = ty;
        st.lastTrans[2] = tz;
        System.arraycopy(x, 0, st.lastXform, 0, 9);

        out.u8(Rec.DRAW)
            .i32(st.meshId)
            .i32(stateId)
            .u8(pass)
            .f64(tx).f64(ty).f64(tz);
        for (int i = 0; i < 9; i++) {
            out.f32(x[i]);
        }
        out.f32(cr).f32(cg).f32(cb).f32(ca);
    }

    private static boolean sameXform(float[] a, float[] b) {
        for (int i = 0; i < 9; i++) {
            if (a[i] != b[i]) {
                return false;
            }
        }
        return true;
    }

    private static int sampler0(ShaderInstance shader) {
        Object o = ((ShaderInstanceAccess) shader).scenerec$getSamplerMap().get("Sampler0");
        if (o instanceof com.mojang.blaze3d.pipeline.RenderTarget rtg) {
            return rtg.getColorTextureId();
        } else if (o instanceof net.minecraft.client.renderer.texture.AbstractTexture at) {
            return at.getId();
        } else if (o instanceof Integer i) {
            return i;
        }
        return 0;
    }

    private static int drawState(String rtName, String shaderName, int texId, String transparency) {
        String key = rtName + '\u0000' + shaderName + '\u0000' + texId + '\u0000' + transparency;
        Integer id = DRAW_STATES.get(key);
        if (id != null) {
            return id;
        }
        int nid = nextStateId++;
        DRAW_STATES.put(key, nid);
        out.u8(Rec.STATE)
            .i32(nid)
            .str(rtName)
            .str(shaderName)
            .i32(texId)
            .str(transparency);
        return nid;
    }

    static RecordBuffer out() {
        return Objects.requireNonNull(out);
    }

    static boolean hasOut() {
        return out != null;
    }

    static void log(String msg) {
        SceneRec.LOGGER.info(msg);
    }
}
