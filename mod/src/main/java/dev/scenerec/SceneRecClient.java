package dev.scenerec;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.brigadier.arguments.IntegerArgumentType;
import dev.scenerec.record.Config;
import dev.scenerec.record.Recorder;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.commands.Commands;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.neoforge.client.event.ClientTickEvent;
import net.neoforged.neoforge.client.event.RegisterClientCommandsEvent;
import net.neoforged.neoforge.client.event.RegisterKeyMappingsEvent;
import net.neoforged.neoforge.common.NeoForge;
import net.neoforged.neoforge.event.level.LevelEvent;
import org.lwjgl.glfw.GLFW;

public final class SceneRecClient {
    private SceneRecClient() {
    }

    public static final KeyMapping TOGGLE = new KeyMapping(
        "key.scenerec.toggle", InputConstants.Type.KEYSYM, GLFW.GLFW_KEY_F6, "key.categories.scenerec");

    static void init(IEventBus modBus) {
        modBus.addListener(RegisterKeyMappingsEvent.class, e -> e.register(TOGGLE));
        NeoForge.EVENT_BUS.addListener(ClientTickEvent.Post.class, e -> {
            while (TOGGLE.consumeClick()) {
                Recorder.toggle();
            }
            if (AutoTest.enabled()) {
                AutoTest.clientTick();
            }
        });
        NeoForge.EVENT_BUS.addListener(LevelEvent.Unload.class, e -> {
            if (e.getLevel().isClientSide() && Recorder.active()) {
                Recorder.stop();
            }
        });
        NeoForge.EVENT_BUS.addListener(RegisterClientCommandsEvent.class, e -> e.getDispatcher().register(
            Commands.literal("scenerec")
                .then(Commands.literal("start")
                    .executes(c -> {
                        Minecraft.getInstance().execute(() -> Recorder.start(Config.fps()));
                        return 1;
                    })
                    .then(Commands.argument("fps", IntegerArgumentType.integer(20, 1000))
                        .executes(c -> {
                            int fps = IntegerArgumentType.getInteger(c, "fps");
                            Minecraft.getInstance().execute(() -> Recorder.start(fps));
                            return 1;
                        })))
                .then(Commands.literal("stop")
                    .executes(c -> {
                        Minecraft.getInstance().execute(Recorder::stop);
                        return 1;
                    }))));
    }
}
