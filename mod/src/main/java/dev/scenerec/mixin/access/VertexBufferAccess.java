package dev.scenerec.mixin.access;

import com.mojang.blaze3d.vertex.VertexBuffer;
import com.mojang.blaze3d.vertex.VertexFormat;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(VertexBuffer.class)
public interface VertexBufferAccess {
    @Accessor("vertexBufferId")
    int scenerec$getVertexBufferId();

    @Accessor("format")
    VertexFormat scenerec$getFormat();

    @Accessor("mode")
    VertexFormat.Mode scenerec$getMode();

    @Accessor("indexCount")
    int scenerec$getIndexCount();
}
