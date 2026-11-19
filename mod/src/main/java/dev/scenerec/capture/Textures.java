package dev.scenerec.capture;

import com.mojang.blaze3d.platform.GlStateManager;
import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.blaze3d.systems.RenderSystem;
import dev.scenerec.SceneRec;
import dev.scenerec.io.Rec;
import dev.scenerec.io.RecordBuffer;
import dev.scenerec.mixin.access.TextureManagerAccess;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.texture.AbstractTexture;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.packs.resources.Resource;
import net.minecraft.server.packs.resources.ResourceManager;

import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * Exports every OpenGL texture that recorded geometry samples as its main texture ("Sampler0").
 *
 * <p>The colour image is read back from the GPU (so whatever the game actually shows, including
 * resource packs and textures generated at runtime, is exported). For textures that come from a
 * resource ({@code TextureManager} path) the LabPBR companion maps {@code <name>_n.png} (normal,
 * AO, height) and {@code <name>_s.png} (smoothness, F0/metal, porosity/SSS, emission) are looked up
 * in the active resource packs. For atlases this is done per sprite and the maps are assembled in
 * the atlas layout so the same UVs work for all three images.</p>
 */
public final class Textures {
    private Textures() {
    }

    /** LabPBR neutral normal: X=128, Y=128 (flat), AO=255, height=255. ABGR int as stored by NativeImage. */
    private static final int DEFAULT_N = 0xFFFF8080;
    /** LabPBR neutral specular: smoothness 0, F0 10/255 (~0.04 dielectric), porosity 0, emission 255 (= none). */
    private static final int DEFAULT_S = 0xFF000A00;
    private static final int MASK_ON = 0xFF0000FF;
    private static final int MASK_OFF = 0xFF000000;

    private static final Map<Integer, Integer> GL_TO_ID = new HashMap<>();
    private static final List<Integer> PENDING = new ArrayList<>();
    private static int nextId;
    private static Path dir;
    private static ExecutorService io;

    public static void begin(Path recordingDir) throws java.io.IOException {
        dir = recordingDir.resolve("textures");
        Files.createDirectories(dir);
        GL_TO_ID.clear();
        PENDING.clear();
        nextId = 1;
        io = Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, "SceneRec-TextureIO");
            t.setDaemon(true);
            return t;
        });
    }

    public static void end() {
        if (io != null) {
            io.shutdown();
            try {
                io.awaitTermination(2, TimeUnit.MINUTES);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            io = null;
        }
        GL_TO_ID.clear();
        PENDING.clear();
    }

    /** Returns the recording-local texture id for a GL texture name, scheduling its export. 0 = none. */
    static int onTextureUsed(int glId) {
        if (glId <= 0) {
            return 0;
        }
        Integer id = GL_TO_ID.get(glId);
        if (id != null) {
            return id;
        }
        int nid = nextId++;
        GL_TO_ID.put(glId, nid);
        PENDING.add(glId);
        return nid;
    }

    /** Exports textures first used during the frame that just ended. Render thread, outside of drawing. */
    public static void flushPending(RecordBuffer out) {
        if (PENDING.isEmpty()) {
            return;
        }
        Map<Integer, ResourceLocation> names = new HashMap<>();
        Map<Integer, AbstractTexture> byGl = new HashMap<>();
        var tm = Minecraft.getInstance().getTextureManager();
        for (Map.Entry<ResourceLocation, AbstractTexture> e : ((TextureManagerAccess) tm).scenerec$getByPath().entrySet()) {
            int id = e.getValue().getId();
            names.put(id, e.getKey());
            byGl.put(id, e.getValue());
        }
        for (int glId : PENDING) {
            try {
                export(out, glId, GL_TO_ID.get(glId), names.get(glId), byGl.get(glId));
            } catch (Exception ex) {
                SceneRec.LOGGER.warn("SceneRec: failed to export texture {}", glId, ex);
            }
        }
        PENDING.clear();
        RenderSystem.bindTexture(0);
    }

    private static void export(RecordBuffer out, int glId, int id, ResourceLocation name, AbstractTexture tex) {
        RenderSystem.activeTexture(33984); // GL_TEXTURE0
        RenderSystem.bindTexture(glId);
        int w = GlStateManager._getTexLevelParameter(3553, 0, 4096); // GL_TEXTURE_WIDTH
        int h = GlStateManager._getTexLevelParameter(3553, 0, 4097); // GL_TEXTURE_HEIGHT
        if (w <= 0 || h <= 0) {
            return;
        }
        NativeImage color = new NativeImage(w, h, false);
        color.downloadTexture(0, false);

        String base = "tex_" + id;
        String colorFile = base + ".png";
        String normalFile = "";
        String specFile = "";
        String maskFile = "";
        NativeImage normal = null;
        NativeImage spec = null;
        NativeImage mask = null;

        ResourceManager rm = Minecraft.getInstance().getResourceManager();
        if (tex instanceof TextureAtlas atlas) {
            Map<ResourceLocation, TextureAtlasSprite> sprites = atlas.getTextures();
            out.u8(Rec.SPRITES).i32(id).i32(sprites.size());
            boolean anyPbr = false;
            normal = filled(w, h, DEFAULT_N);
            spec = filled(w, h, DEFAULT_S);
            mask = filled(w, h, MASK_OFF);
            for (Map.Entry<ResourceLocation, TextureAtlasSprite> e : sprites.entrySet()) {
                TextureAtlasSprite s = e.getValue();
                int sw = s.contents().width();
                int sh = s.contents().height();
                out.str(e.getKey().toString())
                    .i32(s.getX()).i32(s.getY()).i32(sw).i32(sh)
                    .f32(s.getU0()).f32(s.getV0()).f32(s.getU1()).f32(s.getV1());
                ResourceLocation file = spriteFile(e.getKey());
                boolean n = blit(rm, suffixed(file, "_n"), normal, s.getX(), s.getY(), sw, sh);
                boolean sp = blit(rm, suffixed(file, "_s"), spec, s.getX(), s.getY(), sw, sh);
                if (sp) {
                    mask.fillRect(s.getX(), s.getY(), sw, sh, MASK_ON);
                }
                anyPbr |= n | sp;
            }
            if (anyPbr) {
                normalFile = base + "_n.png";
                specFile = base + "_s.png";
                maskFile = base + "_m.png";
            } else {
                normal.close();
                spec.close();
                mask.close();
                normal = spec = mask = null;
            }
        } else if (name != null && name.getPath().endsWith(".png")) {
            NativeImage n = load(rm, suffixed(name, "_n"));
            NativeImage sp = load(rm, suffixed(name, "_s"));
            if (n != null) {
                normal = n;
                normalFile = base + "_n.png";
            }
            if (sp != null) {
                spec = sp;
                specFile = base + "_s.png";
            }
        }

        out.u8(Rec.TEXTURE)
            .i32(id)
            .str(name != null ? name.toString() : "")
            .str(colorFile)
            .i32(w).i32(h)
            .u8(tex instanceof TextureAtlas ? 1 : 0)
            .str(normalFile)
            .str(specFile)
            .str(maskFile);

        write(color, colorFile);
        if (normal != null) {
            write(normal, normalFile);
        }
        if (spec != null) {
            write(spec, specFile);
        }
        if (mask != null) {
            write(mask, maskFile);
        }
    }

    private static void write(NativeImage img, String file) {
        Path p = dir.resolve(file);
        io.execute(() -> {
            try (img) {
                img.writeToFile(p);
            } catch (Exception e) {
                SceneRec.LOGGER.warn("SceneRec: could not write {}", p, e);
            }
        });
    }

    private static NativeImage filled(int w, int h, int abgr) {
        NativeImage img = new NativeImage(w, h, false);
        img.fillRect(0, 0, w, h, abgr);
        return img;
    }

    /** Sprite name {@code ns:block/stone} -> texture file {@code ns:textures/block/stone.png} (vanilla SpriteSource convention). */
    private static ResourceLocation spriteFile(ResourceLocation sprite) {
        return ResourceLocation.fromNamespaceAndPath(sprite.getNamespace(), "textures/" + sprite.getPath() + ".png");
    }

    private static ResourceLocation suffixed(ResourceLocation png, String suffix) {
        String p = png.getPath();
        String stem = p.endsWith(".png") ? p.substring(0, p.length() - 4) : p;
        return ResourceLocation.fromNamespaceAndPath(png.getNamespace(), stem + suffix + ".png");
    }

    private static NativeImage load(ResourceManager rm, ResourceLocation file) {
        Optional<Resource> res = rm.getResource(file);
        if (res.isEmpty()) {
            return null;
        }
        try (InputStream in = res.get().open()) {
            return NativeImage.read(NativeImage.Format.RGBA, in);
        } catch (Exception e) {
            SceneRec.LOGGER.warn("SceneRec: could not read {}", file, e);
            return null;
        }
    }

    /**
     * Copies the first animation frame of a PBR map into the atlas-sized target at the sprite's
     * position, resampling (nearest) if the PBR map has a different resolution than the sprite.
     */
    private static boolean blit(ResourceManager rm, ResourceLocation file, NativeImage target, int x, int y, int sw, int sh) {
        NativeImage src = load(rm, file);
        if (src == null) {
            return false;
        }
        try (src) {
            int srcW = src.getWidth();
            // Animated sprites are vertical strips of square-ish frames; the frame has the sprite's aspect.
            int frameH = Math.max(1, Math.round(srcW * (sh / (float) sw)));
            frameH = Math.min(frameH, src.getHeight());
            for (int j = 0; j < sh; j++) {
                int sy = Math.min(frameH - 1, j * frameH / sh);
                for (int i = 0; i < sw; i++) {
                    int sx = Math.min(srcW - 1, i * srcW / sw);
                    target.setPixelRGBA(x + i, y + j, src.getPixelRGBA(sx, sy));
                }
            }
        }
        return true;
    }
}
