package dev.scenerec.io;

/**
 * Record type tags of the SCREC stream. The layout of every record is documented in
 * {@code docs/FORMAT.md}; the Blender importer ({@code blender/scenerec_importer/screc.py})
 * must be kept in sync with this file.
 */
public final class Rec {
    private Rec() {
    }

    public static final int FORMAT = 1;
    public static final int MESH = 2;
    public static final int MESH_END = 3;
    public static final int STATE = 4;
    public static final int TEXTURE = 5;
    public static final int FRAME_BEGIN = 6;
    public static final int DRAW = 7;
    public static final int DRAW_SAME = 8;
    public static final int FRAME_END = 9;
    public static final int SPRITES = 10;
    public static final int END = 11;
    public static final int SPRITE_EMISSION = 12;

    public static final int PASS_WORLD = 0;
    public static final int PASS_HAND = 1;
}
