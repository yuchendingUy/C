package dev.scenerec.mixin.compat;

import dev.scenerec.record.Recorder;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Pseudo;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Create (via Ponder's catnip library) renders kinetic block entities and contraption structures
 * with {@code SuperByteBuffer}s whose {@code renderInto} multiplies each vertex colour by a
 * directional "diffuse" factor computed from the normal (catnip
 * {@code ShadeSeparatingSuperByteBuffer#renderInto}, {@code DefaultSuperByteBuffer#renderInto}).
 * That would bake fake shading into the recording, so it is disabled while recording through the
 * buffers' own {@code disableDiffuse} flag (reset by {@code renderInto} itself after each call).
 */
@Pseudo
@Mixin(targets = {
    "net.createmod.catnip.render.ShadeSeparatingSuperByteBuffer",
    "net.createmod.catnip.render.DefaultSuperByteBuffer"
}, remap = false)
public abstract class CreateSuperByteBufferMixin {
    @Shadow(remap = false)
    private boolean disableDiffuse;

    @Inject(method = "renderInto", at = @At("HEAD"), remap = false, require = 0)
    private void scenerec$noBakedDiffuse(CallbackInfo ci) {
        if (Recorder.active()) {
            this.disableDiffuse = true;
        }
    }
}
