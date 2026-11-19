package dev.scenerec;

import com.mojang.logging.LogUtils;
import dev.scenerec.record.Config;
import net.neoforged.api.distmarker.Dist;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.fml.ModContainer;
import net.neoforged.fml.common.Mod;
import net.neoforged.fml.config.ModConfig;
import org.slf4j.Logger;

@Mod(SceneRec.MOD_ID)
public final class SceneRec {
    public static final String MOD_ID = "scenerec";
    public static final Logger LOGGER = LogUtils.getLogger();

    public SceneRec(IEventBus modBus, ModContainer container, Dist dist) {
        container.registerConfig(ModConfig.Type.CLIENT, Config.SPEC);
        if (dist.isClient()) {
            SceneRecClient.init(modBus);
        }
    }
}
