#pragma once

#include <cstdint>
#include <cstddef>

namespace videovault::app {

// Random-access byte source behind the decoder's custom AVIO context.
//
// FFmpeg reads the media through this interface instead of a file, which is what
// keeps plaintext off the disk. Two implementations exist:
//
//   * VaultByteSource  - authenticated range reads from an encrypted package.
//   * RemoteStreamSource - progressive HTTP(S) fetch with a bounded RAM buffer.
//
// `read` is allowed to block. That is the whole trick behind progressive
// playback: the demuxer asks for the next bytes, the source waits until the
// network delivered them, and decoding resumes without the player deciding when
// "enough" has arrived. A blocking read must never run on the Qt UI thread.
class ByteSource {
public:
    virtual ~ByteSource() = default;

    // Copies at most `size` bytes at the current position.
    // Returns >0 bytes copied, 0 at end of stream, or a negative value on a
    // real I/O failure. Implementations that block (network) return 0 only once
    // the stream is known to be complete.
    virtual int read(unsigned char* buffer, int size) = 0;

    // Seeks the way AVIO expects: `whence` is SEEK_SET / SEEK_CUR / SEEK_END, or
    // AVSEEK_SIZE (0x10000) to ask for the total size. Returns the resulting
    // absolute position, the size for AVSEEK_SIZE, or a negative number when the
    // operation is not possible.
    virtual std::int64_t seek(std::int64_t offset, int whence) = 0;

    // Total plaintext size, or -1 when it is not known up front (a provider that
    // used chunked transfer encoding, for example).
    virtual std::int64_t size() const = 0;

    // Size of the internal read-ahead/buffer budget in bytes. Sources that serve
    // memory-mapped content ignore it.
    virtual void set_buffer_budget(std::size_t) {}

    // Drops the connection / outstanding work and wakes every blocked reader,
    // which then sees end-of-stream. Safe to call from another thread.
    virtual void cancel() {}
};

// AVSEEK_SIZE from libavutil, restated so this header stays FFmpeg-free.
inline constexpr std::int64_t kSeekSize = 0x10000;

} // namespace videovault::app
