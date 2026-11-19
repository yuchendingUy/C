package dev.scenerec.mixin;

import dev.scenerec.capture.Capture;
import dev.scenerec.io.Rec;
import dev.scenerec.record.Recorder;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.renderer.GameRenderer;
import org.joml.Matrix4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
public abstract class GameRendererMixin {
    @Inject(method = "renderLevel", at = @At("HEAD"))
    private void scenerec$levelStart(DeltaTracker deltaTracker, CallbackInfo ci) {
        Recorder.onRenderLevelStart();
    }

    @Inject(method = "renderLevel", at = @At("RETURN"))
    private void scenerec$levelEnd(DeltaTracker deltaTracker, CallbackInfo ci) {
        Recorder.onRenderLevelEnd();
    }

    @Inject(method = "renderItemInHand", at = @At("HEAD"))
    private void scenerec$handStart(Camera camera, float partialTick, Matrix4f projectionMatrix, CallbackInfo ci) {
        Capture.setPass(Rec.PASS_HAND);
    }

    @Inject(method = "renderItemInHand", at = @At("RETURN"))
    private void scenerec$handEnd(Camera camera, float partialTick, Matrix4f projectionMatrix, CallbackInfo ci) {
        Capture.setPass(Rec.PASS_WORLD);
    }
}
