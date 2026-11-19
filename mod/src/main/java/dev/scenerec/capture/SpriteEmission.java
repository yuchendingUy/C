package dev.scenerec.capture;

import dev.scenerec.io.Rec;
import dev.scenerec.io.RecordBuffer;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.block.BlockRenderDispatcher;
import net.minecraft.client.renderer.block.model.BakedQuad;
import net.minecraft.client.resources.model.BakedModel;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import net.neoforged.neoforge.client.extensions.common.IClientFluidTypeExtensions;
import net.neoforged.neoforge.client.model.data.ModelData;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Table "block-atlas sprite -> light emission of the blocks that use it", built from the block
 * registry and the baked block models. The importer uses it for block geometry whose vertices
 * don't carry an emission tag (meshes that mods bake once and re-emit every frame, such as Create
 * contraption structures). Sprites that are also used by non-emitting states (e.g. lit and unlit
 * candles) are marked ambiguous.
 */
public final class SpriteEmission {
    private SpriteEmission() {
    }

    private static final Direction[] SIDES = {null, Direction.DOWN, Direction.UP, Direction.NORTH, Direction.SOUTH, Direction.WEST, Direction.EAST};

    public static void write(RecordBuffer out) {
        BlockRenderDispatcher dispatcher = Minecraft.getInstance().getBlockRenderer();
        Map<ResourceLocation, Integer> emission = new HashMap<>();
        Set<ResourceLocation> usedDark = new HashSet<>();
        RandomSource random = RandomSource.create();
        for (Block block : BuiltInRegistries.BLOCK) {
            for (BlockState state : block.getStateDefinition().getPossibleStates()) {
                int light = state.getLightEmission();
                try {
                    BakedModel model = dispatcher.getBlockModel(state);
                    for (Direction side : SIDES) {
                        random.setSeed(42L);
                        for (BakedQuad quad : model.getQuads(state, side, random, ModelData.EMPTY, null)) {
                            ResourceLocation sprite = quad.getSprite().contents().name();
                            if (light > 0) {
                                emission.merge(sprite, light, Math::max);
                            } else {
                                usedDark.add(sprite);
                            }
                        }
                    }
                } catch (Throwable ignored) {
                    // Some modded models need real level data; they simply don't contribute.
                }
                FluidState fluid = state.getFluidState();
                if (light > 0 && !fluid.isEmpty()) {
                    try {
                        IClientFluidTypeExtensions ext = IClientFluidTypeExtensions.of(fluid);
                        ResourceLocation still = ext.getStillTexture();
                        ResourceLocation flowing = ext.getFlowingTexture();
                        if (still != null) {
                            emission.merge(still, light, Math::max);
                        }
                        if (flowing != null) {
                            emission.merge(flowing, light, Math::max);
                        }
                    } catch (Throwable ignored) {
                    }
                }
            }
        }
        out.u8(Rec.SPRITE_EMISSION).i32(emission.size());
        for (Map.Entry<ResourceLocation, Integer> e : emission.entrySet()) {
            out.str(e.getKey().toString()).u8(e.getValue()).u8(usedDark.contains(e.getKey()) ? 1 : 0);
        }
    }
}
