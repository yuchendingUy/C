package dev.scenerec.mixin;

import com.mojang.blaze3d.vertex.BufferBuilder;
import com.mojang.blaze3d.vertex.DefaultVertexFormat;
import com.mojang.blaze3d.vertex.VertexFormat;
import dev.scenerec.capture.TerrainTags;
import dev.scenerec.record.Recorder;
import org.lwjgl.system.MemoryUtil;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * BLOCK and NEW_ENTITY vertices end with one padding byte after the normal (see
 * {@code DefaultVertexFormat}) that OpenGL never reads. While recording it carries
 * {@code 0x80 | lightEmission} of the block the vertex belongs to (0 = vertex not produced by
 * block tesselation), so the importer knows which surfaces are light sources.
 */
@Mixin(BufferBuilder.class)
public abstract class BufferBuilderMixin {
    @Shadow @Final private VertexFormat format;
    @Shadow @Final private int vertexSize;

    @Inject(method = "beginVertex", at = @At("RETURN"))
    private void scenerec$tag(CallbackInfoReturnable<Long> cir) {
        if (Recorder.tagTerrain && (this.format == DefaultVertexFormat.BLOCK || this.format == DefaultVertexFormat.NEW_ENTITY)) {
            MemoryUtil.memPutByte(cir.getReturnValue() + this.vertexSize - 1, TerrainTags.paddingByte());
        }
    }
}
