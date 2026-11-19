package dev.scenerec.mixin;

import dev.scenerec.capture.Capture;
import net.minecraft.client.renderer.RenderStateShard;
import net.minecraft.client.renderer.RenderType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Tracks which RenderType is set up, so draws can be tagged with its name and transparency. */
@Mixin(RenderStateShard.class)
public abstract class RenderStateShardMixin {
    @Inject(method = "setupRenderState", at = @At("HEAD"))
    private void scenerec$setup(CallbackInfo ci) {
        if ((Object) this instanceof RenderType rt) {
            Capture.setCurrentRenderType(rt);
        }
    }

    @Inject(method = "clearRenderState", at = @At("HEAD"))
    private void scenerec$clear(CallbackInfo ci) {
        if ((Object) this instanceof RenderType rt) {
            Capture.clearCurrentRenderType(rt);
        }
    }
}
