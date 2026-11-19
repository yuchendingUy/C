package dev.scenerec.mixin.access;

import net.minecraft.client.renderer.ShaderInstance;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

import java.util.Map;

@Mixin(ShaderInstance.class)
public interface ShaderInstanceAccess {
    @Accessor("lastAppliedShader")
    static ShaderInstance scenerec$getLastAppliedShader() {
        throw new AssertionError();
    }

    @Accessor("samplerMap")
    Map<String, Object> scenerec$getSamplerMap();
}
