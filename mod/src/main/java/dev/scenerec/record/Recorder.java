package dev.scenerec.record;

import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import dev.scenerec.SceneRec;
import dev.scenerec.capture.Capture;
import dev.scenerec.capture.SpriteEmission;
import dev.scenerec.capture.Textures;
import dev.scenerec.compat.CreateCompat;
import dev.scenerec.compat.FlywheelCompat;
import dev.scenerec.io.Rec;
import dev.scenerec.io.RecordBuffer;
import dev.scenerec.io.StreamWriter;
import dev.scenerec.mixin.access.LevelRendererAccess;
import net.minecraft.client.renderer.chunk.SectionRenderDispatcher;
import net.minecraft.core.SectionPos;
import net.minecraft.SharedConstants;
import net.minecraft.client.Minecraft;
import net.minecraft.network.chat.Component;
import net.neoforged.fml.ModList;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;

/**
 * Owns a recording session: output files, the world/render settings that are changed while
 * recording, warm-up and the per-frame flush. Render thread only.
 */
public final class Recorder {
    private Recorder() {
    }

    /** Read from chunk-compile worker threads; see {@code ModelBlockRendererMixin} and {@code BufferBuilderMixin}. */
    public static volatile boolean tagTerrain;

    private static boolean active;
    private static Path dir;
    private static StreamWriter writer;
    private static RecordBuffer buffer;
    private static boolean prevSmartCull;
    private static int stableFrames;
    private static int warmupFrames;
    private static int lastVisibleCount;
    private static long startNanos;
    private static long framesWritten;

    private static final int WARMUP_STABLE_FRAMES = 20;
    private static final int WARMUP_MAX_FRAMES = 20 * 60 * 2;

    public static boolean active() {
        return active;
    }

    /** Number of frames completely written so far. */
    public static long framesWritten() {
        return framesWritten;
    }

    public static void toggle() {
        if (active) {
            stop();
        } else {
            start(Config.fps());
        }
    }

    public static void start(int fps) {
        Minecraft mc = Minecraft.getInstance();
        if (active || mc.level == null) {
            return;
        }
        try {
            String name = LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyy-MM-dd_HH-mm-ss"));
            dir = mc.gameDirectory.toPath().resolve("scenerec").resolve(name);
            Files.createDirectories(dir);
            writer = new StreamWriter(dir.resolve("scene.screc"));
            buffer = new RecordBuffer(8 << 20);
            Textures.begin(dir);
            Capture.begin(buffer);
        } catch (IOException e) {
            SceneRec.LOGGER.error("SceneRec: could not start recording", e);
            chat("SceneRec: could not start recording: " + e.getMessage());
            return;
        }
        active = true;
        startNanos = System.nanoTime();

        // Render everything that is loaded, not only what is in view (see FrustumMixin).
        prevSmartCull = mc.smartCull;
        mc.smartCull = false;
        // Make GPU-instanced renderers fall back to the vanilla pipeline.
        FlywheelCompat.forceOff();
        // Rebuild all sections so terrain is compiled without baked AO/shade and with emission tags.
        tagTerrain = true;
        mc.levelRenderer.allChanged();
        CreateCompat.invalidateCaches();
        SpriteEmission.write(buffer);

        float residual = mc.getTimer().getGameTimeDeltaPartialTick(true);
        FixedClock.start(fps, residual);
        framesWritten = 0;
        stableFrames = 0;
        warmupFrames = 0;
        lastVisibleCount = -1;
        if (mc.hasSingleplayerServer()) {
            Lockstep.enable();
        }
        writeMeta(false);
        chat("SceneRec: preparing recording (" + fps + " fps) ...");
    }

    public static void stop() {
        if (!active) {
            return;
        }
        Minecraft mc = Minecraft.getInstance();
        active = false;
        Lockstep.disable();
        FixedClock.stop();
        long frames = framesWritten;
        try {
            buffer.u8(Rec.END);
            writer.submit(buffer.takeBytes());
            writer.close();
        } catch (IOException e) {
            SceneRec.LOGGER.error("SceneRec: error while finishing recording", e);
        }
        Capture.end();
        Textures.end();
        writeMeta(true);

        mc.smartCull = prevSmartCull;
        FlywheelCompat.restore();
        tagTerrain = false;
        mc.levelRenderer.allChanged();
        CreateCompat.invalidateCaches();

        double seconds = frames / (double) FixedClock.fps();
        chat(String.format("SceneRec: saved %d frames (%.2f s of game time) to %s", frames, seconds, dir));
        writer = null;
        buffer = null;
    }

    /** Called at the head of {@code GameRenderer.renderLevel}, after the clock decided about this frame. */
    public static void onRenderLevelStart() {
        if (!active) {
            return;
        }
        if (FixedClock.inWarmup()) {
            warmup();
            return;
        }
        if (FixedClock.frameRecorded()) {
            Capture.beginFrame();
        }
    }

    /** Called at the end of {@code GameRenderer.renderLevel}. */
    public static void onRenderLevelEnd() {
        if (!active || !Capture.inFrame()) {
            return;
        }
        Capture.endFrame((int) FixedClock.currentFrame());
        framesWritten++;
        Textures.flushPending(buffer);
        try {
            writer.submit(buffer.takeBytes());
        } catch (IOException e) {
            SceneRec.LOGGER.error("SceneRec: write failed, stopping", e);
            chat("SceneRec: write failed, stopping: " + e.getMessage());
            stop();
        }
    }

    /** Called every client frame (before ticks) to tell the lockstep whether the game is paused. */
    public static void onFrameStart(boolean paused) {
        if (active) {
            Lockstep.setClientPaused(paused);
        }
    }

    private static void warmup() {
        Minecraft mc = Minecraft.getInstance();
        var dispatcher = mc.levelRenderer.getSectionRenderDispatcher();
        warmupFrames++;
        // Done when nothing is queued/uploading, no visible section still needs a (re)build, and the
        // set of visible sections (which grows while the occlusion graph is rebuilt) is stable.
        var visible = ((LevelRendererAccess) mc.levelRenderer).scenerec$getVisibleSections();
        int pending = 0;
        var light = mc.level.getLightEngine();
        for (SectionRenderDispatcher.RenderSection s : visible) {
            // Vanilla only builds dirty sections whose light data is present (LevelRenderer#compileSections).
            if (light.lightOnInSection(SectionPos.of(s.getOrigin()))
                && (s.isDirty() || s.getCompiled() == SectionRenderDispatcher.CompiledSection.UNCOMPILED)) {
                pending++;
            }
        }
        boolean idle = dispatcher != null && dispatcher.isQueueEmpty() && dispatcher.getToUpload() == 0 && dispatcher.getToBatchCount() == 0
            && pending == 0 && visible.size() == lastVisibleCount && visible.size() > 0;
        lastVisibleCount = visible.size();
        stableFrames = idle ? stableFrames + 1 : 0;
        if (stableFrames >= WARMUP_STABLE_FRAMES || warmupFrames >= WARMUP_MAX_FRAMES) {
            FixedClock.endWarmup();
            chat("SceneRec: recording");
            SceneRec.LOGGER.info("SceneRec: warm-up finished after {} frames ({} sections, {} still pending), recording to {}",
                warmupFrames, visible.size(), pending, dir);
        }
    }

    private static void writeMeta(boolean finished) {
        JsonObject o = new JsonObject();
        o.addProperty("format", "SCREC001");
        o.addProperty("minecraft", SharedConstants.getCurrentVersion().getName());
        o.addProperty("fps", FixedClock.fps());
        o.addProperty("ticksPerSecond", 20);
        o.addProperty("finished", finished);
        o.addProperty("frames", framesWritten);
        o.addProperty("realSeconds", (System.nanoTime() - startNanos) / 1e9);
        o.addProperty("singleplayerLockstep", Minecraft.getInstance().hasSingleplayerServer());
        o.addProperty("bytes", writer != null ? writer.bytesWritten() : -1);
        JsonArray mods = new JsonArray();
        ModList.get().getMods().forEach(m -> mods.add(m.getModId() + "@" + m.getVersion()));
        o.add("mods", mods);
        try {
            Files.writeString(dir.resolve("meta.json"), new GsonBuilder().setPrettyPrinting().create().toJson(o), StandardCharsets.UTF_8);
        } catch (IOException e) {
            SceneRec.LOGGER.warn("SceneRec: could not write meta.json", e);
        }
    }

    private static void chat(String msg) {
        Minecraft mc = Minecraft.getInstance();
        if (mc.player != null) {
            mc.player.displayClientMessage(Component.literal(msg), false);
        }
        SceneRec.LOGGER.info(msg);
    }
}
