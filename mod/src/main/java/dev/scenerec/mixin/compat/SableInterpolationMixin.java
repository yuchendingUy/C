package dev.scenerec.mixin.compat;

import dev.scenerec.record.Recorder;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Pseudo;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyVariable;

/**
 * Sable estimates the server tick rate from the real time between snapshot packets
 * ({@code ClientSableInterpolationState#receiveInfo(int msSinceLast, int gameTick, boolean stopped)},
 * value measured with {@code System.currentTimeMillis()} in {@code SubLevelTrackingSystem}).
 * While recording, game time is decoupled from real time, so report the nominal tick length instead.
 */
@Pseudo
@Mixin(targets = "dev.ryanhcode.sable.network.client.ClientSableInterpolationState", remap = false)
public abstract class SableInterpolationMixin {
    @ModifyVariable(method = "receiveInfo", at = @At("HEAD"), ordinal = 0, argsOnly = true, require = 0)
    private int scenerec$nominalInterval(int msSinceLast) {
        if (!Recorder.active() || Minecraft.getInstance().level == null) {
            return msSinceLast;
        }
        float rate = Minecraft.getInstance().level.tickRateManager().tickrate();
        return Math.max(1, Math.round(1000.0F / rate));
    }
}
