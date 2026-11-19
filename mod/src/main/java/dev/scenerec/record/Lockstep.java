package dev.scenerec.record;

import dev.scenerec.SceneRec;
import net.minecraft.server.MinecraftServer;

import java.util.concurrent.locks.LockSupport;

/**
 * Keeps the integrated (singleplayer) server in step with the fixed client clock while recording.
 *
 * <p>Normally the integrated server ticks on its own thread every 50 ms of real time. While
 * recording, the client may render far slower (or faster) than real time, so the server would run
 * ahead or fall behind. In lockstep mode the server only starts a tick when the client grants one
 * (one grant per client tick) and the client waits until the granted server ticks have finished
 * before running its own tick and rendering. A server that is slow (heavy physics mods) therefore
 * cannot make the recording stutter either.</p>
 */
public final class Lockstep {
    private Lockstep() {
    }

    private static volatile boolean enabled;
    private static volatile long granted;
    private static volatile long started;
    private static volatile long completed;
    /** While the client is paused the server runs freely (it only does its pause bookkeeping then). */
    private static volatile boolean clientPaused;

    private static final long CLIENT_WAIT_LIMIT_NANOS = 60_000_000_000L;

    public static void enable() {
        long c = completed;
        granted = c;
        started = c;
        enabled = true;
    }

    public static void disable() {
        enabled = false;
    }

    public static boolean enabled() {
        return enabled;
    }

    public static void setClientPaused(boolean paused) {
        clientPaused = paused;
    }

    // ------------------------------------------------------------------ client side

    /** Lets the server run {@code ticks} more ticks and waits until they are done. Client thread. */
    public static void grantAndWait(int ticks) {
        if (!enabled || ticks <= 0) {
            return;
        }
        long target = granted + ticks;
        granted = target;
        long start = System.nanoTime();
        while (enabled && completed < target) {
            if (System.nanoTime() - start > CLIENT_WAIT_LIMIT_NANOS) {
                SceneRec.LOGGER.warn("SceneRec: integrated server did not finish its tick within 60 s, continuing without waiting");
                return;
            }
            LockSupport.parkNanos(20_000L);
        }
    }

    // ------------------------------------------------------------------ server side

    /** Called on the server thread right before {@code tickServer}. Blocks (while running server tasks) until a tick is granted. */
    public static void awaitPermit(MinecraftServer server) {
        if (!enabled || clientPaused) {
            return;
        }
        server.managedBlock(() -> !enabled || clientPaused || started < granted || !server.isRunning());
        if (enabled && !clientPaused && started < granted) {
            started++;
        }
    }

    /** Called on the server thread after a tick finished. */
    public static void tickFinished() {
        if (completed < started) {
            completed = started;
        }
    }
}
