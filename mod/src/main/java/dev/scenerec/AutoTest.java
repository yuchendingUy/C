package dev.scenerec;

import dev.scenerec.record.Config;
import dev.scenerec.record.Recorder;
import net.minecraft.client.Minecraft;
import net.minecraft.server.MinecraftServer;

/**
 * Unattended test driver, only active when the JVM is started with
 * {@code -Dscenerec.autotest.frames=N}. After the world has loaded it runs the optional
 * {@code -Dscenerec.autotest.commands="cmd1;cmd2"} on the integrated server, records N frames and
 * quits the game. Used for automated end-to-end tests; does nothing in normal play.
 */
final class AutoTest {
    private AutoTest() {
    }

    private static final int FRAMES = Integer.getInteger("scenerec.autotest.frames", 0);
    private static final String COMMANDS = System.getProperty("scenerec.autotest.commands", "");
    /** Run when the recording starts (e.g. assembling a physics structure so it moves while recorded). */
    private static final String COMMANDS_AT_START = System.getProperty("scenerec.autotest.commandsAtStart", "");
    private static final boolean SWITCH_CAMERA = Boolean.getBoolean("scenerec.autotest.switchCamera");
    /** Degrees per client tick to turn the player while recording (exercises a moving camera). */
    private static final float SPIN = Float.parseFloat(System.getProperty("scenerec.autotest.spin", "0"));
    private static final boolean HIDE_GUI = Boolean.getBoolean("scenerec.autotest.hideGui");
    private static final int SETTLE_TICKS = Integer.getInteger("scenerec.autotest.settleTicks", 100);

    private static int ticksInWorld;
    private static boolean started;
    private static boolean done;
    private static int quitCountdown = -1;

    static boolean enabled() {
        return FRAMES > 0;
    }

    private static void run(Minecraft mc, String commands) {
        MinecraftServer server = mc.getSingleplayerServer();
        if (server == null || commands.isBlank()) {
            return;
        }
        for (String cmd : commands.split(";")) {
            String c = cmd.trim();
            if (!c.isEmpty()) {
                server.execute(() -> {
                    SceneRec.LOGGER.info("SceneRec autotest: /{}", c);
                    server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), c);
                });
            }
        }
    }

    static void clientTick() {
        Minecraft mc = Minecraft.getInstance();
        if (quitCountdown >= 0) {
            if (quitCountdown-- == 0) {
                SceneRec.LOGGER.info("SceneRec autotest: quitting");
                mc.stop();
            }
            return;
        }
        if (done || mc.level == null || mc.player == null) {
            return;
        }
        ticksInWorld++;
        if (!started && ticksInWorld == 20) {
            run(mc, COMMANDS);
        }
        if (!started && ticksInWorld >= SETTLE_TICKS) {
            started = true;
            if (HIDE_GUI) {
                mc.options.hideGui = true; // also hides the first-person hand (GameRenderer#renderItemInHand)
            }
            SceneRec.LOGGER.info("SceneRec autotest: starting recording of {} frames", FRAMES);
            Recorder.start(Config.fps());
            run(mc, COMMANDS_AT_START);
        }
        if (started && Recorder.active() && SPIN != 0.0F && mc.player != null) {
            mc.player.setYRot(mc.player.getYRot() + SPIN);
            mc.player.setXRot(10.0F * (float) Math.sin(Recorder.framesWritten() / 40.0));
        }
        if (started && Recorder.active() && SWITCH_CAMERA && Recorder.framesWritten() >= FRAMES / 2
            && mc.options.getCameraType() == net.minecraft.client.CameraType.FIRST_PERSON) {
            SceneRec.LOGGER.info("SceneRec autotest: switching to third person at frame {}", Recorder.framesWritten());
            mc.options.setCameraType(net.minecraft.client.CameraType.THIRD_PERSON_BACK);
        }
        if (started && Recorder.active() && Recorder.framesWritten() >= FRAMES) {
            Recorder.stop();
            done = true;
            quitCountdown = 40;
        }
    }
}
