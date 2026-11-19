package dev.scenerec.io;

import java.io.BufferedOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.Deflater;

/**
 * Writes the record stream as a sequence of independently deflated blocks:
 * <pre>
 *   file   := "SCREC001" block*
 *   block  := i32 rawLength, i32 compressedLength, byte[compressedLength] (zlib/deflate)
 * </pre>
 * Compression runs on a small thread pool; blocks are written strictly in submission order.
 * When too many blocks are in flight the submitting (render) thread waits, so memory stays
 * bounded and the recording simply slows down instead of dropping data.
 */
public final class StreamWriter implements AutoCloseable {
    public static final byte[] MAGIC = "SCREC001".getBytes(StandardCharsets.US_ASCII);
    private static final int MAX_IN_FLIGHT = 48;

    private final OutputStream out;
    private final ExecutorService compressors;
    private final ExecutorService writer;
    private final ArrayDeque<Future<?>> writes = new ArrayDeque<>();
    private volatile IOException failure;
    private long bytesWritten;

    public StreamWriter(Path file) throws IOException {
        this.out = new BufferedOutputStream(Files.newOutputStream(file), 1 << 20);
        this.out.write(MAGIC);
        int threads = Math.max(1, Math.min(4, Runtime.getRuntime().availableProcessors() - 2));
        AtomicInteger n = new AtomicInteger();
        this.compressors = Executors.newFixedThreadPool(threads, r -> {
            Thread t = new Thread(r, "SceneRec-Compress-" + n.incrementAndGet());
            t.setDaemon(true);
            return t;
        });
        this.writer = Executors.newSingleThreadExecutor(r -> {
            Thread t = new Thread(r, "SceneRec-Writer");
            t.setDaemon(true);
            return t;
        });
    }

    private static byte[] compress(byte[] raw) {
        Deflater deflater = new Deflater(1);
        try {
            deflater.setInput(raw);
            deflater.finish();
            byte[] buf = new byte[Math.max(64, raw.length / 2 + 64)];
            int len = 0;
            while (!deflater.finished()) {
                if (len == buf.length) {
                    byte[] nb = new byte[buf.length * 2];
                    System.arraycopy(buf, 0, nb, 0, len);
                    buf = nb;
                }
                len += deflater.deflate(buf, len, buf.length - len);
            }
            ByteBuffer block = ByteBuffer.allocate(8 + len).order(ByteOrder.LITTLE_ENDIAN);
            block.putInt(raw.length);
            block.putInt(len);
            block.put(buf, 0, len);
            return block.array();
        } finally {
            deflater.end();
        }
    }

    /** Queues one block of raw record bytes. Called from the render thread. */
    public void submit(byte[] raw) throws IOException {
        if (this.failure != null) {
            throw this.failure;
        }
        if (raw.length == 0) {
            return;
        }
        Future<byte[]> compressed = this.compressors.submit(() -> compress(raw));
        Future<?> write = this.writer.submit(() -> {
            try {
                byte[] block = compressed.get();
                this.out.write(block);
                this.bytesWritten += block.length;
            } catch (IOException e) {
                this.failure = e;
            } catch (Exception e) {
                this.failure = new IOException(e);
            }
        });
        this.writes.addLast(write);
        while (this.writes.size() > MAX_IN_FLIGHT) {
            waitFor(this.writes.removeFirst());
        }
        while (!this.writes.isEmpty() && this.writes.peekFirst().isDone()) {
            this.writes.removeFirst();
        }
    }

    private static void waitFor(Future<?> f) throws IOException {
        try {
            f.get();
        } catch (Exception e) {
            throw new IOException(e);
        }
    }

    public long bytesWritten() {
        return this.bytesWritten;
    }

    @Override
    public void close() throws IOException {
        try {
            while (!this.writes.isEmpty()) {
                waitFor(this.writes.removeFirst());
            }
        } finally {
            this.compressors.shutdown();
            this.writer.shutdown();
            this.out.close();
        }
        if (this.failure != null) {
            throw this.failure;
        }
    }
}
