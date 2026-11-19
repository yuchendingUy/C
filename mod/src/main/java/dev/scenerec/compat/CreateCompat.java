package dev.scenerec.compat;

import dev.scenerec.SceneRec;
import net.neoforged.fml.ModList;

/**
 * Create caches the baked meshes of contraption structures and kinetic parts in catnip's
 * {@code SuperByteBufferCache}. Those meshes are baked with ambient occlusion through
 * {@code ModelBlockRenderer}, so the cache is invalidated when recording starts (rebuilt without
 * baked AO, see ModelBlockRendererMixin) and again when it stops.
 */
public final class CreateCompat {
    private CreateCompat() {
    }

    public static void invalidateCaches() {
        if (!ModList.get().isLoaded("create") && !ModList.get().isLoaded("ponder")) {
            return;
        }
        try {
            Class<?> c = Class.forName("net.createmod.catnip.render.SuperByteBufferCache");
            Object cache = c.getMethod("getInstance").invoke(null);
            c.getMethod("invalidate").invoke(cache);
        } catch (Throwable t) {
            SceneRec.LOGGER.warn("SceneRec: could not invalidate Create's SuperByteBufferCache", t);
        }
    }
}
