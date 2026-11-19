package dev.scenerec.mixin.access;

import net.minecraft.client.Camera;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

@Mixin(GameRenderer.class)
public interface GameRendererAccess {
    @Invoker("getFov")
    double scenerec$getFov(Camera camera, float partialTicks, boolean useFovSetting);
}
