package dev.scenerec.io;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * Growable little-endian byte buffer used to assemble the records of one frame
 * before they are handed to the compressor threads.
 */
public final class RecordBuffer {
    private ByteBuffer buf;

    public RecordBuffer(int initialCapacity) {
        this.buf = ByteBuffer.allocate(initialCapacity).order(ByteOrder.LITTLE_ENDIAN);
    }

    private void ensure(int extra) {
        if (this.buf.remaining() >= extra) {
            return;
        }
        int needed = this.buf.position() + extra;
        int cap = Math.max(needed, this.buf.capacity() * 2);
        ByteBuffer n = ByteBuffer.allocate(cap).order(ByteOrder.LITTLE_ENDIAN);
        this.buf.flip();
        n.put(this.buf);
        this.buf = n;
    }

    public RecordBuffer u8(int v) {
        ensure(1);
        this.buf.put((byte) v);
        return this;
    }

    public RecordBuffer i32(int v) {
        ensure(4);
        this.buf.putInt(v);
        return this;
    }

    public RecordBuffer i64(long v) {
        ensure(8);
        this.buf.putLong(v);
        return this;
    }

    public RecordBuffer f32(float v) {
        ensure(4);
        this.buf.putFloat(v);
        return this;
    }

    public RecordBuffer f64(double v) {
        ensure(8);
        this.buf.putDouble(v);
        return this;
    }

    public RecordBuffer str(String s) {
        byte[] b = (s == null ? "" : s).getBytes(StandardCharsets.UTF_8);
        if (b.length > 0xFFFF) {
            throw new IllegalArgumentException("string too long");
        }
        ensure(2 + b.length);
        this.buf.putShort((short) b.length);
        this.buf.put(b);
        return this;
    }

    /** Copies {@code len} bytes from {@code src} starting at its current position (position is not modified). */
    public RecordBuffer bytes(ByteBuffer src, int len) {
        ensure(len);
        ByteBuffer dup = src.duplicate();
        dup.limit(dup.position() + len);
        this.buf.put(dup);
        return this;
    }

    public RecordBuffer bytes(byte[] src) {
        ensure(src.length);
        this.buf.put(src);
        return this;
    }

    public int size() {
        return this.buf.position();
    }

    /** Returns the written bytes and resets this buffer. */
    public byte[] takeBytes() {
        byte[] out = new byte[this.buf.position()];
        this.buf.flip();
        this.buf.get(out);
        this.buf.clear();
        return out;
    }
}
