package dev.scenerec.record;

import net.neoforged.neoforge.common.ModConfigSpec;

public final class Config {
    private Config() {
    }

    public static final ModConfigSpec SPEC;
    public static final ModConfigSpec.IntValue FPS;

    static {
        ModConfigSpec.Builder b = new ModConfigSpec.Builder();
        FPS = b.comment("Frames per second of the recording (game time). Every rendered frame advances the game by exactly 1/fps seconds.")
            .defineInRange("fps", 200, 20, 1000);
        SPEC = b.build();
    }

    public static int fps() {
        return FPS.get();
    }
}
