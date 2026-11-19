package dev.scenerec.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.scenerec.capture.TerrainTags;
import dev.scenerec.record.Recorder;
import net.minecraft.client.renderer.RenderType;
import net.minecraft.client.renderer.block.ModelBlockRenderer;
import net.minecraft.client.resources.model.BakedModel;
import net.minecraft.core.BlockPos;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.BlockAndTintGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.neoforged.neoforge.client.model.data.ModelData;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Terrain vertex colours are {@code tint * brightness}, where brightness is the baked ambient
 * occlusion and directional face shade. A path tracer computes both itself, so while recording
 * the brightness is forced to 1 and only the biome/block tint remains in the vertex colour.
 * Also remembers the light emission of the block being tesselated (see {@link TerrainTags}).
 */
@Mixin(ModelBlockRenderer.class)
public abstract class ModelBlockRendererMixin {
    private static final String TESSELATE = "tesselateBlock(Lnet/minecraft/world/level/BlockAndTintGetter;Lnet/minecraft/client/resources/model/BakedModel;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/core/BlockPos;Lcom/mojang/blaze3d/vertex/PoseStack;Lcom/mojang/blaze3d/vertex/VertexConsumer;ZLnet/minecraft/util/RandomSource;JILnet/neoforged/neoforge/client/model/data/ModelData;Lnet/minecraft/client/renderer/RenderType;)V";

    @Inject(method = TESSELATE, at = @At("HEAD"))
    private void scenerec$tesselateStart(BlockAndTintGetter level, BakedModel model, BlockState state, BlockPos pos, PoseStack poseStack,
                                         VertexConsumer consumer, boolean checkSides, RandomSource random, long seed, int packedOverlay,
                                         ModelData modelData, RenderType renderType, CallbackInfo ci) {
        if (Recorder.tagTerrain) {
            TerrainTags.set(state.getLightEmission(level, pos));
        }
    }

    @Inject(method = TESSELATE, at = @At("RETURN"))
    private void scenerec$tesselateEnd(BlockAndTintGetter level, BakedModel model, BlockState state, BlockPos pos, PoseStack poseStack,
                                       VertexConsumer consumer, boolean checkSides, RandomSource random, long seed, int packedOverlay,
                                       ModelData modelData, RenderType renderType, CallbackInfo ci) {
        TerrainTags.clear();
    }
    @ModifyVariable(method = "putQuadData", at = @At("HEAD"), ordinal = 0, argsOnly = true)
    private float scenerec$b0(float v) {
        return Recorder.tagTerrain ? 1.0F : v;
    }

    @ModifyVariable(method = "putQuadData", at = @At("HEAD"), ordinal = 1, argsOnly = true)
    private float scenerec$b1(float v) {
        return Recorder.tagTerrain ? 1.0F : v;
    }

    @ModifyVariable(method = "putQuadData", at = @At("HEAD"), ordinal = 2, argsOnly = true)
    private float scenerec$b2(float v) {
        return Recorder.tagTerrain ? 1.0F : v;
    }

    @ModifyVariable(method = "putQuadData", at = @At("HEAD"), ordinal = 3, argsOnly = true)
    private float scenerec$b3(float v) {
        return Recorder.tagTerrain ? 1.0F : v;
    }
}
