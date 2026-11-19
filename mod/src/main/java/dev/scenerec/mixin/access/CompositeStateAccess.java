package dev.scenerec.mixin.access;

import net.minecraft.client.renderer.RenderStateShard;
import net.minecraft.client.renderer.RenderType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(RenderType.CompositeState.class)
public interface CompositeStateAccess {
    @Accessor("transparencyState")
    RenderStateShard.TransparencyStateShard scenerec$getTransparencyState();
}
