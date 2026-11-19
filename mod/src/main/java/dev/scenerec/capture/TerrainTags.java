package dev.scenerec.capture;

/**
 * Per-thread light emission (0..15) of the block currently being tesselated by
 * {@code ModelBlockRenderer.tesselateBlock} / {@code BlockRenderDispatcher.renderLiquid},
 * or -1 when no block is being tesselated.
 */
public final class TerrainTags {
    private TerrainTags() {
    }

    public static final int NONE = -1;
    private static final ThreadLocal<int[]> EMISSION = ThreadLocal.withInitial(() -> new int[]{NONE});

    public static void set(int emission) {
        EMISSION.get()[0] = Math.max(0, Math.min(15, emission));
    }

    public static void clear() {
        EMISSION.get()[0] = NONE;
    }

    /** Value for the vertex padding byte: {@code 0x80 | emission} while tesselating a block, 0 otherwise. */
    public static byte paddingByte() {
        int e = EMISSION.get()[0];
        return e < 0 ? 0 : (byte) (0x80 | e);
    }
}
