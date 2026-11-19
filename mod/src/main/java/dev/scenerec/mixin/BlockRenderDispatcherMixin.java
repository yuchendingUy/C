package dev.scenerec.mixin;

import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.scenerec.capture.TerrainTags;
import dev.scenerec.record.Recorder;
import net.minecraft.client.renderer.block.BlockRenderDispatcher;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockAndTintGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Remembers the light emission of the fluid block being tesselated (see {@link TerrainTags}). */
@Mixin(BlockRenderDispatcher.class)
public abstract class BlockRenderDispatcherMixin {
    @Inject(method = "renderLiquid", at = @At("HEAD"))
    private void scenerec$liquidStart(BlockPos pos, BlockAndTintGetter level, VertexConsumer consumer, BlockState blockState, FluidState fluidState, CallbackInfo ci) {
        if (Recorder.tagTerrain) {
            TerrainTags.set(blockState.getLightEmission(level, pos));
        }
    }

    @Inject(method = "renderLiquid", at = @At("RETURN"))
    private void scenerec$liquidEnd(BlockPos pos, BlockAndTintGetter level, VertexConsumer consumer, BlockState blockState, FluidState fluidState, CallbackInfo ci) {
        TerrainTags.clear();
    }
}
