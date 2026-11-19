package dev.scenerec.mixin;

import dev.scenerec.record.Lockstep;
import net.minecraft.Util;
import net.minecraft.server.MinecraftServer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Lockstep for the integrated server (see {@link Lockstep}): wait for a granted tick before
 * {@code tickServer}, and don't sleep until the next 50 ms boundary afterwards.
 */
@Mixin(MinecraftServer.class)
public abstract class MinecraftServerMixin {
    @Shadow protected long nextTickTimeNanos;

    @Shadow public abstract net.minecraft.server.ServerTickRateManager tickRateManager();

    @Unique
    private boolean scenerec$waiting;

    @Inject(method = "runServer", at = @At(value = "INVOKE", target = "Lnet/minecraft/server/MinecraftServer;tickServer(Ljava/util/function/BooleanSupplier;)V"))
    private void scenerec$beforeTick(CallbackInfo ci) {
        if (Lockstep.enabled()) {
            Lockstep.awaitPermit((MinecraftServer) (Object) this);
            if (Lockstep.enabled()) {
                this.nextTickTimeNanos = Util.getNanos() + this.tickRateManager().nanosecondsPerTick();
            }
        }
    }

    @Inject(method = "waitUntilNextTick", at = @At("HEAD"))
    private void scenerec$tickDone(CallbackInfo ci) {
        Lockstep.tickFinished();
        this.scenerec$waiting = Lockstep.enabled();
    }

    @Inject(method = "waitUntilNextTick", at = @At("RETURN"))
    private void scenerec$waitDone(CallbackInfo ci) {
        this.scenerec$waiting = false;
    }

    /** While in lockstep the post-tick wait only runs pending tasks; pacing comes from the client. */
    @Inject(method = "haveTime", at = @At("HEAD"), cancellable = true)
    private void scenerec$noWallClockWait(CallbackInfoReturnable<Boolean> cir) {
        if (this.scenerec$waiting && Lockstep.enabled()) {
            cir.setReturnValue(false);
        }
    }
}
