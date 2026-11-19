package dev.scenerec.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.scenerec.capture.Capture;
import dev.scenerec.mixin.access.GameRendererAccess;
import dev.scenerec.record.FixedClock;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.LightTexture;
import org.joml.Matrix4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LevelRenderer.class)
public abstract class LevelRendererMixin {
    @Inject(method = "renderLevel", at = @At("HEAD"))
    private void scenerec$frameCamera(DeltaTracker deltaTracker, boolean renderBlockOutline, Camera camera, GameRenderer gameRenderer,
                                      LightTexture lightTexture, Matrix4f frustumMatrix, Matrix4f projectionMatrix, CallbackInfo ci) {
        if (!Capture.inFrame()) {
            return;
        }
        Minecraft mc = Minecraft.getInstance();
        float partial = deltaTracker.getGameTimeDeltaPartialTick(true);
        float fov = (float) ((GameRendererAccess) gameRenderer).scenerec$getFov(camera, partial, true);
        float aspect = mc.getWindow().getWidth() / (float) Math.max(1, mc.getWindow().getHeight());
        Capture.frameCamera(camera, frustumMatrix, (int) FixedClock.currentFrame(), FixedClock.currentGameTime(),
            fov, aspect, mc.options.getCameraType().ordinal());
    }

    // Sky, sun/moon/stars, clouds and the world border are not part of the 3D scene.

    @Inject(method = "renderSky", at = @At("HEAD"))
    private void scenerec$skyStart(Matrix4f frustumMatrix, Matrix4f projectionMatrix, float partialTick, Camera camera, boolean isFoggy, Runnable skyFogSetup, CallbackInfo ci) {
        Capture.pushSuppress();
    }

    @Inject(method = "renderSky", at = @At("RETURN"))
    private void scenerec$skyEnd(Matrix4f frustumMatrix, Matrix4f projectionMatrix, float partialTick, Camera camera, boolean isFoggy, Runnable skyFogSetup, CallbackInfo ci) {
        Capture.popSuppress();
    }

    @Inject(method = "renderClouds", at = @At("HEAD"))
    private void scenerec$cloudsStart(PoseStack poseStack, Matrix4f frustumMatrix, Matrix4f projectionMatrix, float partialTick, double camX, double camY, double camZ, CallbackInfo ci) {
        Capture.pushSuppress();
    }

    @Inject(method = "renderClouds", at = @At("RETURN"))
    private void scenerec$cloudsEnd(PoseStack poseStack, Matrix4f frustumMatrix, Matrix4f projectionMatrix, float partialTick, double camX, double camY, double camZ, CallbackInfo ci) {
        Capture.popSuppress();
    }

    @Inject(method = "renderWorldBorder", at = @At("HEAD"))
    private void scenerec$borderStart(Camera camera, CallbackInfo ci) {
        Capture.pushSuppress();
    }

    @Inject(method = "renderWorldBorder", at = @At("RETURN"))
    private void scenerec$borderEnd(Camera camera, CallbackInfo ci) {
        Capture.popSuppress();
    }
}
