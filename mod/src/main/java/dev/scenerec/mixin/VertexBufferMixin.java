package dev.scenerec.mixin;

import com.mojang.blaze3d.vertex.MeshData;
import com.mojang.blaze3d.vertex.VertexBuffer;
import dev.scenerec.capture.Capture;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(VertexBuffer.class)
public abstract class VertexBufferMixin {
    @Inject(method = "upload", at = @At("HEAD"))
    private void scenerec$upload(MeshData meshData, CallbackInfo ci) {
        if (Capture.active()) {
            Capture.onUpload((VertexBuffer) (Object) this, meshData);
        }
    }

    @Inject(method = "draw", at = @At("HEAD"))
    private void scenerec$draw(CallbackInfo ci) {
        if (Capture.active()) {
            Capture.onDraw((VertexBuffer) (Object) this);
        }
    }

    @Inject(method = "close", at = @At("HEAD"))
    private void scenerec$close(CallbackInfo ci) {
        if (Capture.active()) {
            Capture.onClose((VertexBuffer) (Object) this);
        }
    }
}
