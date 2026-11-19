package dev.scenerec.compat;

import dev.scenerec.SceneRec;
import net.neoforged.fml.ModList;
import net.neoforged.neoforge.common.ModConfigSpec;

/**
 * Flywheel (used by Create) renders block entities with GPU instancing: the final vertex positions
 * only exist on the GPU, so they cannot be captured from vertex buffers. Flywheel's own
 * "flywheel:off" backend makes everything fall back to vanilla block entity renderers.
 *
 * <p>Implementation follows Flywheel's {@code /flywheel backend <id>} command
 * ({@code neoforge/.../impl/FlwCommands.java}): set {@code NeoForgeFlwConfig.INSTANCE.client.backend}
 * and call {@code levelRenderer.allChanged()} (which the recorder does anyway). Accessed reflectively
 * so Flywheel stays an optional dependency.</p>
 */
public final class FlywheelCompat {
    private FlywheelCompat() {
    }

    private static final String OFF = "flywheel:off";
    private static String previous;

    @SuppressWarnings("unchecked")
    private static ModConfigSpec.ConfigValue<String> backendValue() throws ReflectiveOperationException {
        Class<?> cfg = Class.forName("dev.engine_room.flywheel.impl.NeoForgeFlwConfig");
        Object instance = cfg.getField("INSTANCE").get(null);
        Object client = cfg.getField("client").get(instance);
        return (ModConfigSpec.ConfigValue<String>) client.getClass().getField("backend").get(client);
    }

    public static void forceOff() {
        previous = null;
        if (!ModList.get().isLoaded("flywheel")) {
            return;
        }
        try {
            ModConfigSpec.ConfigValue<String> v = backendValue();
            String cur = v.get();
            if (!OFF.equals(cur)) {
                previous = cur;
                v.set(OFF);
                SceneRec.LOGGER.info("SceneRec: switched Flywheel backend from {} to {} for recording", cur, OFF);
            }
        } catch (Throwable t) {
            SceneRec.LOGGER.warn("SceneRec: could not switch Flywheel backend off; instanced block entities will be missing", t);
        }
    }

    public static void restore() {
        if (previous == null) {
            return;
        }
        try {
            backendValue().set(previous);
        } catch (Throwable t) {
            SceneRec.LOGGER.warn("SceneRec: could not restore Flywheel backend", t);
        }
        previous = null;
    }
}
