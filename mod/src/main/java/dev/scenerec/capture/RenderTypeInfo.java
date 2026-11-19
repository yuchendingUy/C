package dev.scenerec.capture;

import dev.scenerec.mixin.access.CompositeRenderTypeAccess;
import dev.scenerec.mixin.access.CompositeStateAccess;
import net.minecraft.client.renderer.RenderType;

/**
 * Classification of vanilla render types (names taken from {@code RenderType.create(...)} calls in
 * the 1.21.1 sources).
 */
final class RenderTypeInfo {
    private RenderTypeInfo() {
    }

    /** Render types whose geometry is an overlay/effect that must not end up in the 3D scene. */
    static boolean isSkipped(String name) {
        if (name.isEmpty()) {
            return false;
        }
        switch (name) {
            case "lines", "line_strip", "outline", "water_mask", "crumbling", "entity_shadow", "clouds",
                 "text_see_through", "text_background_see_through", "text_intensity_see_through" -> {
                return true;
            }
            default -> {
            }
        }
        return name.startsWith("debug_") || name.startsWith("gui") || name.contains("glint");
    }

    /** Name of the transparency shard of a composite render type, or "" if unknown. */
    static String transparency(RenderType rt) {
        if (rt instanceof CompositeRenderTypeAccess crt) {
            RenderType.CompositeState state = crt.scenerec$getState();
            if (state != null) {
                var shard = ((CompositeStateAccess) (Object) state).scenerec$getTransparencyState();
                if (shard != null) {
                    return shard.name;
                }
            }
        }
        return "";
    }
}
