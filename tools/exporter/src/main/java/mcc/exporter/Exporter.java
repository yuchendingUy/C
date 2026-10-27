package mcc.exporter;

import java.io.IOException;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

import org.slf4j.Logger;

import com.mojang.logging.LogUtils;

import net.minecraft.core.BlockPos;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.ResourceLocation;
import net.minecraft.server.MinecraftServer;
import net.minecraft.tags.TagKey;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.entity.BlockEntityType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.VoxelShape;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.fml.common.Mod;
import net.neoforged.neoforge.common.NeoForge;
import net.neoforged.neoforge.event.server.ServerStartedEvent;

/**
 * 服务端启动完成后导出注册表，格式与 tools/gen_vanilla_data.py 一致（mcc 直接载入），然后关服。
 *
 * blocks.txt   方块与方块状态（状态编号 = 网络编号）
 * shapes.txt   每个方块状态的碰撞形状（运行时求值，机械动力的形状也准确）
 * items.txt    物品：I 名称 最大堆叠 最大耐久 对应方块
 * registries.txt         数据驱动注册表（配置阶段同步，条目顺序即网络编号）
 * static_registries.txt  内置注册表（方块、物品、实体、方块实体类型……，条目顺序即编号）
 * tags.txt     T 注册表 标签 条目数 条目...
 * block_entities.txt  E 方块实体类型 方块数 方块...
 */
@Mod("mcc_exporter")
public class Exporter {
    private static final Logger LOG = LogUtils.getLogger();

    public Exporter(IEventBus modBus) {
        NeoForge.EVENT_BUS.addListener(this::onStarted);
    }

    private void onStarted(ServerStartedEvent event) {
        MinecraftServer server = event.getServer();
        Path dir = Path.of(System.getProperty("mcc.export.dir", "mcc-export"));
        try {
            Files.createDirectories(dir);
            exportBlocks(dir);
            exportItems(dir);
            exportStaticRegistries(dir);
            exportDynamicRegistries(server, dir);
            exportTags(server, dir);
            exportBlockEntities(dir);
            LOG.info("mcc 导出完成：{}", dir.toAbsolutePath());
            Files.writeString(dir.resolve("DONE"), "ok\n");
        } catch (Throwable t) {
            LOG.error("mcc 导出失败", t);
        }
        server.halt(false);
    }

    private static PrintWriter open(Path dir, String name) throws IOException {
        return new PrintWriter(Files.newBufferedWriter(dir.resolve(name), StandardCharsets.UTF_8));
    }

    /** 与 Python %g 接近：去掉多余的 0 */
    private static String num(double v) {
        if (v == Math.rint(v) && Math.abs(v) < 1e9) return Long.toString((long) v);
        String s = String.format(Locale.ROOT, "%.6f", v);
        s = s.replaceAll("0+$", "");
        return s.endsWith(".") ? s.substring(0, s.length() - 1) : s;
    }

    @SuppressWarnings({ "unchecked", "rawtypes" })
    private static String valueName(Property p, Comparable v) {
        return p.getName(v);
    }

    // ---------------- 方块 ----------------

    private void exportBlocks(Path dir) throws IOException {
        Map<String, Integer> shapeIds = new LinkedHashMap<>();
        List<String> shapeLines = new ArrayList<>();
        List<String> blockShapeLines = new ArrayList<>();
        int expect = 0, blocks = 0, shapeFallbacks = 0;
        try (PrintWriter f = open(dir, "blocks.txt")) {
            f.println("# mcc 方块注册表 1.21.1 + 机械动力 + Sable（由 tools/exporter 从 NeoForge 运行时导出）");
            for (Block b : BuiltInRegistries.BLOCK) {
                ResourceLocation name = BuiltInRegistries.BLOCK.getKey(b);
                List<BlockState> states = b.getStateDefinition().getPossibleStates();
                int first = Block.getId(states.get(0));
                if (first != expect) throw new IllegalStateException(name + " 首个状态编号 " + first + "，期望 " + expect);
                for (int i = 0; i < states.size(); i++)
                    if (Block.getId(states.get(i)) != first + i) throw new IllegalStateException(name + " 状态编号不连续");

                /* 属性顺序：按步长从大到小，最后一个变化最快（mcc 的编号规则），再逐个状态验证 */
                Collection<Property<?>> propsC = b.getStateDefinition().getProperties();
                List<Property<?>> props = new ArrayList<>(propsC);
                Map<Property<?>, Integer> stride = new HashMap<>();
                for (Property<?> p : props) {
                    int s = 0;
                    for (int i = 1; i < states.size(); i++)
                        if (!states.get(i).getValue(p).equals(states.get(0).getValue(p))) { s = i; break; }
                    stride.put(p, s);
                }
                props.sort(Comparator.comparingInt((Property<?> p) -> stride.get(p)).reversed());
                for (int i = 0; i < states.size(); i++) {
                    int idx = 0, mul = 1;
                    for (int k = props.size() - 1; k >= 0; k--) {
                        Property p = props.get(k);
                        List<Comparable> vals = new ArrayList<>(p.getPossibleValues());
                        idx += vals.indexOf(states.get(i).getValue(p)) * mul;
                        mul *= vals.size();
                    }
                    if (idx != i) throw new IllegalStateException(name + " 状态顺序与属性步长不符");
                }

                BlockState def = b.defaultBlockState();
                float hardness;
                try {
                    hardness = def.getDestroySpeed(EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
                } catch (Throwable t) {
                    hardness = -1;
                }
                boolean full;
                int filter;
                try {
                    full = def.isCollisionShapeFullBlock(EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
                    filter = def.getLightBlock(EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
                } catch (Throwable t) {
                    full = false;
                    filter = 0;
                }
                int flags = (def.canOcclude() ? 0 : 1) | (full ? 2 : 0) | (hardness >= 0 ? 4 : 0);
                f.printf(Locale.ROOT, "B %s %d %d %d %s %s %d %d %d %d%n", name, first, states.size(), Block.getId(def), num(hardness),
                         num(b.getExplosionResistance()), flags, def.getLightEmission(), filter, props.size());
                for (Property p : props) {
                    StringBuilder sb = new StringBuilder();
                    Collection<Comparable> vals = p.getPossibleValues();
                    for (Comparable v : vals) sb.append(' ').append(valueName(p, v));
                    f.printf(Locale.ROOT, "P %s %d%s%n", p.getName(), vals.size(), sb);
                }

                /* 碰撞形状：空世界里求值；需要世界上下文而抛异常的退回到轮廓形状 */
                StringBuilder ids = new StringBuilder();
                List<Integer> list = new ArrayList<>();
                for (BlockState st : states) {
                    VoxelShape vs;
                    try {
                        vs = st.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
                    } catch (Throwable t) {
                        shapeFallbacks++;
                        try {
                            vs = st.getShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
                        } catch (Throwable t2) {
                            vs = null;
                        }
                    }
                    StringBuilder key = new StringBuilder();
                    int n = 0;
                    if (vs != null)
                        for (AABB a : vs.toAabbs()) {
                            key.append(' ').append(num(a.minX)).append(' ').append(num(a.minY)).append(' ').append(num(a.minZ))
                               .append(' ').append(num(a.maxX)).append(' ').append(num(a.maxY)).append(' ').append(num(a.maxZ));
                            n++;
                        }
                    String k = n + key.toString();
                    Integer id = shapeIds.get(k);
                    if (id == null) {
                        id = shapeIds.size();
                        shapeIds.put(k, id);
                        shapeLines.add("S " + id + " " + k);
                    }
                    list.add(id);
                }
                boolean same = list.stream().allMatch(x -> x.equals(list.get(0)));
                if (same) ids.append(' ').append(list.get(0));
                else for (int x : list) ids.append(' ').append(x);
                blockShapeLines.add("C " + name + " " + (same ? 1 : list.size()) + ids);
                expect += states.size();
                blocks++;
            }
        }
        try (PrintWriter f = open(dir, "shapes.txt")) {
            f.println("# mcc 方块碰撞形状（由 tools/exporter 从 NeoForge 运行时导出）");
            shapeLines.forEach(f::println);
            blockShapeLines.forEach(f::println);
        }
        LOG.info("mcc 导出：方块 {} 种，状态 {} 个，碰撞形状 {} 种（{} 个状态退回轮廓形状）", blocks, expect, shapeIds.size(), shapeFallbacks);
    }

    // ---------------- 物品 ----------------

    private void exportItems(Path dir) throws IOException {
        try (PrintWriter f = open(dir, "items.txt")) {
            f.println("# I 名称 编号 最大堆叠 最大耐久 对应方块（- 无）");
            for (Item it : BuiltInRegistries.ITEM) {
                ItemStack st = new ItemStack(it);
                String block = it instanceof BlockItem bi ? BuiltInRegistries.BLOCK.getKey(bi.getBlock()).toString() : "-";
                f.printf(Locale.ROOT, "I %s %d %d %d %s%n", BuiltInRegistries.ITEM.getKey(it), BuiltInRegistries.ITEM.getId(it),
                         st.getMaxStackSize(), st.getMaxDamage(), block);
            }
        }
    }

    // ---------------- 注册表 ----------------

    @SuppressWarnings({ "unchecked", "rawtypes" })
    private void exportStaticRegistries(Path dir) throws IOException {
        try (PrintWriter f = open(dir, "static_registries.txt")) {
            f.println("# 内置注册表（条目顺序即编号）");
            for (Registry reg : BuiltInRegistries.REGISTRY) {
                f.printf("R %s %d%n", reg.key().location(), reg.size());
                for (Object o : reg) f.printf("E %s%n", reg.getKey(o));
            }
        }
    }

    @SuppressWarnings({ "unchecked", "rawtypes" })
    private void exportDynamicRegistries(MinecraftServer server, Path dir) throws IOException {
        try (PrintWriter f = open(dir, "registries.txt")) {
            f.println("# 数据驱动注册表（条目顺序即网络编号）");
            server.registryAccess().registries().forEach(e -> {
                Registry reg = e.value();
                if (BuiltInRegistries.REGISTRY.containsKey(reg.key().location())) return;
                f.printf("R %s %d%n", reg.key().location(), reg.size());
                for (Object o : reg) f.printf("E %s%n", reg.getKey(o));
            });
        }
    }

    @SuppressWarnings({ "unchecked", "rawtypes" })
    private void exportTags(MinecraftServer server, Path dir) throws IOException {
        try (PrintWriter f = open(dir, "tags.txt")) {
            f.println("# T 注册表 标签 条目数 条目...");
            server.registryAccess().registries().forEach(e -> {
                Registry reg = e.value();
                reg.getTagNames().forEach(tk -> {
                    TagKey key = (TagKey) tk;
                    reg.getTag(key).ifPresent(set -> {
                        StringBuilder sb = new StringBuilder();
                        int n = 0;
                        for (Object h : (Iterable) set) {
                            net.minecraft.core.Holder hh = (net.minecraft.core.Holder) h;
                            sb.append(' ').append(hh.unwrapKey().map(k -> ((net.minecraft.resources.ResourceKey) k).location().toString()).orElse("?"));
                            n++;
                        }
                        f.printf("T %s %s %d%s%n", reg.key().location(), key.location(), n, sb);
                    });
                });
            });
        }
    }

    private void exportBlockEntities(Path dir) throws IOException {
        try (PrintWriter f = open(dir, "block_entities.txt")) {
            f.println("# E 方块实体类型 方块数 方块...");
            for (BlockEntityType<?> t : BuiltInRegistries.BLOCK_ENTITY_TYPE) {
                StringBuilder sb = new StringBuilder();
                int n = 0;
                for (Block b : BuiltInRegistries.BLOCK) {
                    if (t.isValid(b.defaultBlockState())) {
                        sb.append(' ').append(BuiltInRegistries.BLOCK.getKey(b));
                        n++;
                    }
                }
                f.printf("E %s %d%s%n", BuiltInRegistries.BLOCK_ENTITY_TYPE.getKey(t), n, sb);
            }
        }
    }
}
