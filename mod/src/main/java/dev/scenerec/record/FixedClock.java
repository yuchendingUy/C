package dev.scenerec.record;

/**
 * Replaces the wall-clock driven {@code DeltaTracker.Timer#advanceGameTime} while recording.
 *
 * <p>Every rendered frame advances game time by exactly {@code 20 / fps} ticks, no matter how long
 * the frame took in real time. Frame {@code k} is therefore rendered at game time
 * {@code T(k) = T0 + k * 20 / fps} ticks: the client runs {@code floor(T(k))} ticks in total and
 * renders with partial tick {@code T(k) - floor(T(k))}, exactly like vanilla would at that moment.
 * A slow machine only makes the recording take longer in real time; no frame is ever skipped.</p>
 */
public final class FixedClock {
    private FixedClock() {
    }

    private static volatile boolean enabled;
    private static boolean warmup;
    private static int fps;
    private static double startResidual;
    private static long frame;          // index of the next frame to record
    private static long ticksRun;       // client ticks run since the clock was started
    private static boolean frameRecorded;
    private static long currentFrame = -1;
    private static double currentGameTime;

    public static void start(int framesPerSecond, float residual) {
        fps = framesPerSecond;
        startResidual = residual;
        frame = 0;
        ticksRun = 0;
        warmup = true;
        frameRecorded = false;
        currentFrame = -1;
        enabled = true;
    }

    public static void stop() {
        enabled = false;
        frameRecorded = false;
    }

    public static boolean enabled() {
        return enabled;
    }

    public static void endWarmup() {
        warmup = false;
    }

    public static boolean inWarmup() {
        return warmup;
    }

    /** Whether the frame currently being rendered is part of the recording. */
    public static boolean frameRecorded() {
        return enabled && frameRecorded;
    }

    public static long currentFrame() {
        return currentFrame;
    }

    /** Game time (in ticks, relative to the start of the recording) of the frame being rendered. */
    public static double currentGameTime() {
        return currentGameTime;
    }

    public static int fps() {
        return fps;
    }

    /**
     * Called instead of the vanilla body of {@code advanceGameTime}.
     *
     * @param paused whether the timer is paused (pause menu open in singleplayer)
     * @param out    {@code out[0]} receives the new partial tick ("deltaTickResidual")
     * @return number of client ticks to run before rendering this frame
     */
    public static int advance(boolean paused, float[] out) {
        if (warmup || paused) {
            frameRecorded = false;
            out[0] = Float.NaN; // keep the timer's residual untouched
            return 0;
        }
        double t = startResidual + frame * 20.0 / fps;
        long whole = (long) Math.floor(t);
        int ticks = (int) (whole - ticksRun);
        ticksRun = whole;
        out[0] = (float) (t - whole);
        currentFrame = frame;
        currentGameTime = frame * 20.0 / fps;
        frame++;
        frameRecorded = true;
        return ticks;
    }

    public static long framesRecorded() {
        return frame;
    }
}
