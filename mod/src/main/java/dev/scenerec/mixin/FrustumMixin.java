package dev.scenerec.mixin;

import dev.scenerec.record.Recorder;
import net.minecraft.client.renderer.culling.Frustum;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * While recording everything that is loaded is treated as visible, so terrain sections, entities
 * and block entities behind the camera are rendered (and therefore captured) too.
 */
@Mixin(Frustum.class)
public abstract class FrustumMixin {
    @Inject(method = "cubeInFrustum", at = @At("HEAD"), cancellable = true)
    private void scenerec$allVisible(double minX, double minY, double minZ, double maxX, double maxY, double maxZ, CallbackInfoReturnable<Boolean> cir) {
        if (Recorder.active()) {
            cir.setReturnValue(true);
        }
    }
}
