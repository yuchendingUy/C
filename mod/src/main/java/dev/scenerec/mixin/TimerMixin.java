package dev.scenerec.mixin;

import dev.scenerec.record.FixedClock;
import dev.scenerec.record.Lockstep;
import dev.scenerec.record.Recorder;
import net.minecraft.client.DeltaTracker;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Drives game time from the fixed recording clock instead of the wall clock. */
@Mixin(DeltaTracker.Timer.class)
public abstract class TimerMixin {
    @Shadow private float deltaTicks;
    @Shadow private float deltaTickResidual;
    @Shadow private long lastMs;
    @Shadow private boolean paused;

    @Unique
    private final float[] scenerec$out = new float[1];

    @Inject(method = "advanceGameTime", at = @At("HEAD"), cancellable = true)
    private void scenerec$fixedStep(long time, CallbackInfoReturnable<Integer> cir) {
        if (!FixedClock.enabled()) {
            return;
        }
        Recorder.onFrameStart(this.paused);
        int ticks = FixedClock.advance(this.paused, this.scenerec$out);
        this.lastMs = time;
        float residual = this.scenerec$out[0];
        if (Float.isNaN(residual)) {
            this.deltaTicks = 0.0F;
        } else {
            this.deltaTicks = 20.0F / FixedClock.fps();
            this.deltaTickResidual = residual;
        }
        if (ticks > 0) {
            Lockstep.grantAndWait(ticks);
        }
        cir.setReturnValue(ticks);
    }
}
