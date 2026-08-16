// Everything here runs without audio hardware. Each check builds a tiny
// synthetic file on the fly, so the suite has no dependency on fixture files,
// and the PlayerController checks drive a fake engine rather than a device.
//
// Covered: the SPSC ring buffer, the DSD packers, the WAV/DFF/DSF readers and
// their length/seek behaviour, probe_track, playlist editing and sequencing
// (repeat, shuffle), M3U8 round-trip, and the controller's track-change and
// error-classification state machines.

#include "audio/dsd_packer.h"
#include "formats/dff_reader.h"
#include "formats/dsf_reader.h"
#include "formats/utf8_file.h"
#include "formats/wav_reader.h"
#include "playback/ring_buffer.h"
#include "player/player_controller.h"
#include "player/playlist.h"
#include "player/track_info.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
// _wremove: deleting a fixture whose name is not representable in the ANSI
// code page needs the wide CRT entry point.
#include <wchar.h>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

// ---- little helper for building synthetic file bytes ----

class ByteWriter {
public:
    void ascii(const char* text)
    {
        for (const char* p = text; *p; ++p) bytes_.push_back(static_cast<std::uint8_t>(*p));
    }
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void be16(std::uint16_t value)
    {
        bytes_.push_back(static_cast<std::uint8_t>(value >> 8));
        bytes_.push_back(static_cast<std::uint8_t>(value));
    }
    void be32(std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void be64(std::uint64_t value)
    {
        for (int shift = 56; shift >= 0; shift -= 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void le16(std::uint16_t value)
    {
        bytes_.push_back(static_cast<std::uint8_t>(value));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 8));
    }
    void le32(std::uint32_t value)
    {
        for (int shift = 0; shift <= 24; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void le64(std::uint64_t value)
    {
        for (int shift = 0; shift <= 56; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void zeros(std::size_t count) { bytes_.resize(bytes_.size() + count, 0); }

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

// Writes the file on construction and deletes it on destruction. Declare it
// before the reader that opens it so the reader is destroyed first: on Windows
// std::remove fails while a handle is still open, which used to leave
// wasio_test_*.{wav,dff,dsf} behind in the working directory.
class TempFile {
public:
    TempFile(std::string path, const std::vector<std::uint8_t>& bytes) : path_(std::move(path))
    {
        // Same UTF-8 path handling the readers use, so a non-ASCII fixture name
        // is written and deleted under the name probe_track() will look for.
        auto file = wasio::open_utf8_ofstream(path_, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        ok_ = static_cast<bool>(file);
    }
    ~TempFile() { _wremove(wasio::widen_utf8(path_).c_str()); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    bool ok() const { return ok_; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    bool ok_ = false;
};

bool fail(const char* test, const char* what)
{
    std::cerr << "[" << test << "] " << what << "\n";
    return false;
}

bool close_enough(double actual, double expected)
{
    return std::fabs(actual - expected) < 1e-9;
}

// ---- synthetic file fixtures, shared by the reader and the seek checks ----

constexpr double kDsdRate64 = 2822400.0;

struct WavFixture {
    std::vector<std::uint8_t> file;
    std::vector<std::uint8_t> pcm; // the data chunk payload, frame-interleaved
    std::size_t frames = 4;
    std::size_t bytes_per_frame = 4; // 2 channels * Int16
};

WavFixture make_wav_fixture()
{
    // 4 stereo Int16 frames: (1,-1) (2,-2) (3,-3) (4,-4).
    const std::int16_t samples[8] = {1, -1, 2, -2, 3, -3, 4, -4};
    ByteWriter pcm;
    for (std::int16_t sample : samples) pcm.le16(static_cast<std::uint16_t>(sample));

    ByteWriter file;
    file.ascii("RIFF");
    file.le32(4 + (8 + 16) + static_cast<std::uint32_t>(8 + pcm.bytes().size()));
    file.ascii("WAVE");
    file.ascii("fmt ");
    file.le32(16);
    file.le16(1);     // PCM
    file.le16(2);     // channels
    file.le32(44100); // sample rate
    file.le32(44100 * 2 * 2);
    file.le16(2 * 2);
    file.le16(16); // bits per sample
    file.ascii("data");
    file.le32(static_cast<std::uint32_t>(pcm.bytes().size()));
    for (auto byte : pcm.bytes()) file.u8(byte);

    WavFixture fixture;
    fixture.file = file.bytes();
    fixture.pcm = pcm.bytes();
    return fixture;
}

struct DffFixture {
    std::vector<std::uint8_t> file;
    // What read_frames() should hand back: the raw bytes, bit-reversed.
    std::vector<std::uint8_t> expected;
    std::size_t frames = 3;
    std::size_t channels = 2;
};

DffFixture make_dff_fixture()
{
    // PROP payload: "SND " + FS(be32 rate) + CHNL(be16 count).
    ByteWriter prop_payload;
    prop_payload.ascii("SND ");
    prop_payload.ascii("FS  ");
    prop_payload.be64(4);
    prop_payload.be32(2822400);
    prop_payload.ascii("CHNL");
    prop_payload.be64(2);
    prop_payload.be16(2);

    const std::uint8_t dsd_raw[] = {0x01, 0x80, 0x0f, 0xf0, 0xaa, 0x55};

    ByteWriter file;
    file.ascii("FRM8");
    const std::uint64_t prop_chunk_total = 12 + prop_payload.bytes().size();
    const std::uint64_t dsd_chunk_total = 12 + sizeof(dsd_raw);
    file.be64(prop_chunk_total + dsd_chunk_total);
    file.ascii("DSD ");
    file.ascii("PROP");
    file.be64(prop_payload.bytes().size());
    for (auto byte : prop_payload.bytes()) file.u8(byte);
    file.ascii("DSD ");
    file.be64(sizeof(dsd_raw));
    for (auto byte : dsd_raw) file.u8(byte);

    DffFixture fixture;
    fixture.file = file.bytes();
    fixture.expected = {0x80, 0x01, 0xf0, 0x0f, 0x55, 0xaa};
    return fixture;
}

struct DsfFixture {
    std::vector<std::uint8_t> file;
    std::vector<std::uint8_t> ch0;
    std::vector<std::uint8_t> ch1;
    std::size_t frames = 9;     // bytes per channel
    std::uint32_t block_size = 4; // so frames span three blocks
};

DsfFixture make_dsf_fixture()
{
    constexpr std::uint32_t kChannels = 2;
    constexpr std::uint32_t kBlockSize = 4;
    constexpr std::uint64_t kSampleCount = 72; // 9 bytes/channel * 8 bits
    constexpr std::uint8_t kPad = 0xee;

    // 9 bytes per channel, laid out as 3 blocks of 4 bytes, block-major then
    // channel-minor, matching DsfDsdSource::load_block()'s offset formula.
    const std::uint8_t ch0[9] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const std::uint8_t ch1[9] = {0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88};

    ByteWriter data;
    for (std::uint32_t block = 0; block < 3; ++block) {
        for (const std::uint8_t* channel_bytes : {ch0, ch1}) {
            for (std::uint32_t i = 0; i < kBlockSize; ++i) {
                const std::uint32_t index = block * kBlockSize + i;
                data.u8(index < 9 ? channel_bytes[index] : kPad);
            }
        }
    }

    ByteWriter file;
    file.ascii("DSD ");
    file.zeros(24); // rest of the 28-byte master chunk; not validated by the reader

    file.ascii("fmt ");
    file.le64(52);
    file.le32(1);         // format version
    file.le32(0);         // format id (DSD raw)
    file.le32(kChannels); // channel type (unused by the reader)
    file.le32(kChannels);
    file.le32(2822400); // sample rate
    file.le32(1);       // bits per sample
    file.le64(kSampleCount);
    file.le32(kBlockSize);
    file.le32(0); // reserved

    file.ascii("data");
    file.le64(12 + data.bytes().size());
    for (auto byte : data.bytes()) file.u8(byte);

    DsfFixture fixture;
    fixture.file = file.bytes();
    fixture.ch0.assign(ch0, ch0 + 9);
    fixture.ch1.assign(ch1, ch1 + 9);
    return fixture;
}

// ---- ring buffer ----

bool test_ring_buffer()
{
    wasio::SpscByteRing ring(8);
    const std::uint8_t input[] = {0, 1, 2, 3, 4, 5};
    if (ring.write(input, sizeof(input)) != sizeof(input)) return fail("ring", "short write");
    std::uint8_t output[4] = {};
    if (ring.read(output, sizeof(output)) != sizeof(output)) return fail("ring", "short read");
    if (output[0] != 0 || output[3] != 3) return fail("ring", "wrong values before wrap");

    if (ring.write(input, sizeof(input)) != sizeof(input)) return fail("ring", "short write 2");
    std::uint8_t wrapped[8] = {};
    if (ring.read(wrapped, sizeof(wrapped)) != sizeof(wrapped)) return fail("ring", "short wrapped read");
    if (wrapped[0] != 4 || wrapped[1] != 5 || wrapped[2] != 0 || wrapped[7] != 5)
        return fail("ring", "wrong values after wrap");

    // clear() must drop any unread bytes and leave the ring usable, since the
    // seek handshake relies on this to discard stale data.
    if (ring.write(input, 4) != 4) return fail("ring", "short write before clear");
    if (ring.available_to_read() != 4) return fail("ring", "wrong available_to_read before clear");
    ring.clear();
    if (ring.available_to_read() != 0) return fail("ring", "clear() left bytes readable");
    if (ring.write(input, 3) != 3) return fail("ring", "write failed after clear");
    std::uint8_t after_clear[3] = {};
    if (ring.read(after_clear, 3) != 3) return fail("ring", "read failed after clear");
    if (after_clear[0] != 0 || after_clear[1] != 1 || after_clear[2] != 2)
        return fail("ring", "wrong values after clear");

    return true;
}

// ---- DSD packer ----

bool test_dsd_packer()
{
    const std::uint8_t lsb[] = {0x01, 0x80};
    std::uint8_t msb[2] = {};
    wasio::DsdPacker::native_msb1(lsb, msb, 2);
    if (msb[0] != 0x80 || msb[1] != 0x01) return fail("dsd_packer", "native_msb1 mismatch");

    const std::uint8_t dop_input[] = {0x12, 0x34};
    std::uint8_t dop_output[4] = {};
    wasio::DsdPacker::dop_int32(dop_input, 0x05, dop_output);
    if (dop_output[0] != 0 || dop_output[1] != 0x2c || dop_output[2] != 0x48 || dop_output[3] != 0x05)
        return fail("dsd_packer", "dop_int32 mismatch");

    std::uint8_t dop24_output[3] = {};
    wasio::DsdPacker::dop_int24(dop_input, 0xfa, dop24_output);
    if (dop24_output[0] != 0x2c || dop24_output[1] != 0x48 || dop24_output[2] != 0xfa)
        return fail("dsd_packer", "dop_int24 mismatch");

    const std::uint8_t native_input[] = {0x12, 0x34, 0x01, 0x80};
    std::uint8_t native_word[4] = {};
    wasio::DsdPacker::native_word32(native_input, native_word);
    if (native_word[0] != 0x48 || native_word[1] != 0x2c || native_word[2] != 0x80 || native_word[3] != 0x01)
        return fail("dsd_packer", "native_word32 mismatch");

    std::uint8_t round_trip[4] = {};
    wasio::DsdPacker::native_msb1(native_word, round_trip, 4);
    if (round_trip[0] != native_input[0] || round_trip[1] != native_input[1] ||
        round_trip[2] != native_input[2] || round_trip[3] != native_input[3])
        return fail("dsd_packer", "native_word32/native_msb1 round trip mismatch");

    return true;
}

// ---- WAV reader ----

bool test_wav_reader()
{
    const auto fixture = make_wav_fixture();
    TempFile temp("wasio_test_wav.wav", fixture.file);
    if (!temp.ok()) return fail("wav", "could not write temp file");

    wasio::WavPcmSource source(temp.path());
    if (!source.valid()) return fail("wav", ("parse failed: " + source.error()).c_str());
    if (source.format().sample_rate != 44100.0) return fail("wav", "wrong sample rate");
    if (source.format().channels != 2) return fail("wav", "wrong channel count");
    if (source.format().encoding != wasio::PcmEncoding::Int16) return fail("wav", "wrong encoding");

    std::vector<std::uint8_t> read_back(fixture.pcm.size());
    if (source.read_frames(read_back.data(), 4) != 4) return fail("wav", "read_frames returned short count");
    if (read_back != fixture.pcm) return fail("wav", "read back bytes do not match");
    if (!source.finished()) return fail("wav", "not finished after reading all frames");
    if (source.read_frames(read_back.data(), 1) != 0) return fail("wav", "read past end returned frames");

    return true;
}

bool test_wav_seek()
{
    const auto fixture = make_wav_fixture();
    TempFile temp("wasio_test_wav_seek.wav", fixture.file);
    if (!temp.ok()) return fail("wav_seek", "could not write temp file");

    wasio::WavPcmSource source(temp.path());
    if (!source.valid()) return fail("wav_seek", ("parse failed: " + source.error()).c_str());
    if (source.total_frames() != fixture.frames) return fail("wav_seek", "wrong total_frames");
    if (source.position_frames() != 0) return fail("wav_seek", "position is not 0 after open");

    std::vector<std::uint8_t> buffer(fixture.pcm.size());
    if (source.read_frames(buffer.data(), 2) != 2) return fail("wav_seek", "short read before seek");
    if (source.position_frames() != 2) return fail("wav_seek", "position did not advance with reads");

    // Seek back to the start and confirm the same bytes come out again.
    if (!source.seek_frames(0)) return fail("wav_seek", "seek to 0 failed");
    if (source.position_frames() != 0) return fail("wav_seek", "position not 0 after seek to 0");
    if (source.read_frames(buffer.data(), 4) != 4) return fail("wav_seek", "short read after rewind");
    if (buffer != fixture.pcm) return fail("wav_seek", "bytes after rewind do not match");

    // Seek into the middle: frame 2 must yield the second half of the payload.
    if (!source.seek_frames(2)) return fail("wav_seek", "seek to frame 2 failed");
    if (source.position_frames() != 2) return fail("wav_seek", "wrong position after seek to frame 2");
    if (source.finished()) return fail("wav_seek", "finished() true in the middle of the file");
    if (source.read_frames(buffer.data(), 2) != 2) return fail("wav_seek", "short read after mid seek");
    for (std::size_t i = 0; i < 2 * fixture.bytes_per_frame; ++i) {
        if (buffer[i] != fixture.pcm[2 * fixture.bytes_per_frame + i])
            return fail("wav_seek", "bytes after mid seek do not match");
    }

    // Seeking to total_frames() is the "seek to end" case and leaves it finished.
    if (!source.seek_frames(fixture.frames)) return fail("wav_seek", "seek to end failed");
    if (!source.finished()) return fail("wav_seek", "not finished after seek to end");
    if (source.position_frames() != fixture.frames) return fail("wav_seek", "wrong position after seek to end");

    // Out of range must fail without moving the position.
    if (source.seek_frames(fixture.frames + 1)) return fail("wav_seek", "seek past end reported success");
    if (source.position_frames() != fixture.frames)
        return fail("wav_seek", "failed seek moved the position");

    return true;
}

// ---- DFF (DSDIFF) reader ----

bool test_dff_reader()
{
    const auto fixture = make_dff_fixture();
    TempFile temp("wasio_test_dff.dff", fixture.file);
    if (!temp.ok()) return fail("dff", "could not write temp file");

    wasio::DffDsdSource source(temp.path());
    if (!source.valid()) return fail("dff", ("parse failed: " + source.error()).c_str());
    if (source.format().sample_rate != 2822400) return fail("dff", "wrong sample rate");
    if (source.format().channels != 2) return fail("dff", "wrong channel count");

    std::uint8_t frames[6] = {};
    if (source.read_frames(frames, 3) != 3) return fail("dff", "read_frames returned short count");
    for (int i = 0; i < 6; ++i)
        if (frames[i] != fixture.expected[static_cast<std::size_t>(i)])
            return fail("dff", "bit-reversed bytes do not match");
    if (!source.finished()) return fail("dff", "not finished after reading all frames");

    return true;
}

bool test_dff_seek()
{
    const auto fixture = make_dff_fixture();
    TempFile temp("wasio_test_dff_seek.dff", fixture.file);
    if (!temp.ok()) return fail("dff_seek", "could not write temp file");

    wasio::DffDsdSource source(temp.path());
    if (!source.valid()) return fail("dff_seek", ("parse failed: " + source.error()).c_str());
    if (source.total_frames() != fixture.frames) return fail("dff_seek", "wrong total_frames");
    if (source.position_frames() != 0) return fail("dff_seek", "position is not 0 after open");

    std::uint8_t buffer[6] = {};
    if (source.read_frames(buffer, 3) != 3) return fail("dff_seek", "short read before seek");
    if (source.position_frames() != 3) return fail("dff_seek", "position did not advance with reads");
    if (!source.finished()) return fail("dff_seek", "not finished after reading all frames");

    // Rewind an exhausted source and read it again.
    if (!source.seek_frames(0)) return fail("dff_seek", "seek to 0 failed");
    if (source.finished()) return fail("dff_seek", "still finished after rewind");
    if (source.read_frames(buffer, 3) != 3) return fail("dff_seek", "short read after rewind");
    for (int i = 0; i < 6; ++i)
        if (buffer[i] != fixture.expected[static_cast<std::size_t>(i)])
            return fail("dff_seek", "bytes after rewind do not match");

    // Seek to frame 1 and confirm the payload is still bit-reversed correctly.
    if (!source.seek_frames(1)) return fail("dff_seek", "seek to frame 1 failed");
    if (source.position_frames() != 1) return fail("dff_seek", "wrong position after seek to frame 1");
    if (source.read_frames(buffer, 2) != 2) return fail("dff_seek", "short read after mid seek");
    for (int i = 0; i < 4; ++i)
        if (buffer[i] != fixture.expected[static_cast<std::size_t>(i) + 2])
            return fail("dff_seek", "bytes after mid seek do not match");

    if (source.seek_frames(fixture.frames + 1)) return fail("dff_seek", "seek past end reported success");
    if (source.position_frames() != fixture.frames)
        return fail("dff_seek", "failed seek moved the position");

    return true;
}

// ---- DSF reader ----

bool test_dsf_reader()
{
    const auto fixture = make_dsf_fixture();
    TempFile temp("wasio_test_dsf.dsf", fixture.file);
    if (!temp.ok()) return fail("dsf", "could not write temp file");

    wasio::DsfDsdSource source(temp.path());
    if (!source.valid()) return fail("dsf", ("parse failed: " + source.error()).c_str());
    if (source.format().sample_rate != 2822400) return fail("dsf", "wrong sample rate");
    if (source.format().channels != 2) return fail("dsf", "wrong channel count");

    std::uint8_t frames[18] = {};
    if (source.read_frames(frames, 9) != 9) return fail("dsf", "read_frames returned short count");
    for (int i = 0; i < 9; ++i) {
        if (frames[i * 2] != fixture.ch0[static_cast<std::size_t>(i)])
            return fail("dsf", "channel 0 byte mismatch");
        if (frames[i * 2 + 1] != fixture.ch1[static_cast<std::size_t>(i)])
            return fail("dsf", "channel 1 byte mismatch");
    }
    if (!source.finished()) return fail("dsf", "not finished after reading all frames");

    return true;
}

bool test_dsf_seek()
{
    const auto fixture = make_dsf_fixture();
    TempFile temp("wasio_test_dsf_seek.dsf", fixture.file);
    if (!temp.ok()) return fail("dsf_seek", "could not write temp file");

    wasio::DsfDsdSource source(temp.path());
    if (!source.valid()) return fail("dsf_seek", ("parse failed: " + source.error()).c_str());
    if (source.total_frames() != fixture.frames) return fail("dsf_seek", "wrong total_frames");
    if (source.position_frames() != 0) return fail("dsf_seek", "position is not 0 after open");

    std::uint8_t buffer[18] = {};
    if (source.read_frames(buffer, 9) != 9) return fail("dsf_seek", "short read before seek");
    if (source.position_frames() != fixture.frames)
        return fail("dsf_seek", "position did not advance with reads");

    // Rewind: block 0 is already cached, so this also covers the cache-hit path.
    if (!source.seek_frames(0)) return fail("dsf_seek", "seek to 0 failed");
    if (source.finished()) return fail("dsf_seek", "still finished after rewind");
    if (source.read_frames(buffer, 2) != 2) return fail("dsf_seek", "short read after rewind");
    if (buffer[0] != fixture.ch0[0] || buffer[1] != fixture.ch1[0])
        return fail("dsf_seek", "frame 0 wrong after rewind");

    // Frame 5 lives in block 1 (block_size 4), so this forces load_block() to
    // reload both channels from a different block than the cached one.
    if (!source.seek_frames(5)) return fail("dsf_seek", "seek to frame 5 failed");
    if (source.position_frames() != 5) return fail("dsf_seek", "wrong position after seek to frame 5");
    if (source.read_frames(buffer, 4) != 4) return fail("dsf_seek", "short read after mid seek");
    for (std::size_t i = 0; i < 4; ++i) {
        if (buffer[i * 2] != fixture.ch0[5 + i]) return fail("dsf_seek", "channel 0 wrong after mid seek");
        if (buffer[i * 2 + 1] != fixture.ch1[5 + i])
            return fail("dsf_seek", "channel 1 wrong after mid seek");
    }

    if (!source.seek_frames(fixture.frames)) return fail("dsf_seek", "seek to end failed");
    if (!source.finished()) return fail("dsf_seek", "not finished after seek to end");
    if (source.seek_frames(fixture.frames + 1)) return fail("dsf_seek", "seek past end reported success");
    if (source.position_frames() != fixture.frames)
        return fail("dsf_seek", "failed seek moved the position");

    return true;
}

// ---- probe_track ----

bool test_probe_track()
{
    {
        const auto fixture = make_wav_fixture();
        TempFile temp("wasio_test_probe.wav", fixture.file);
        if (!temp.ok()) return fail("probe_track", "could not write temp WAV");
        const auto info = wasio::probe_track(temp.path());
        if (!info.valid) return fail("probe_track", ("WAV probe failed: " + info.error).c_str());
        if (info.kind != wasio::TrackKind::PcmFile) return fail("probe_track", "WAV kind is not PcmFile");
        if (info.sample_rate != 44100.0) return fail("probe_track", "WAV sample rate wrong");
        if (info.channels != 2) return fail("probe_track", "WAV channel count wrong");
        if (info.encoding != wasio::PcmEncoding::Int16) return fail("probe_track", "WAV encoding wrong");
        if (info.total_frames != fixture.frames) return fail("probe_track", "WAV total_frames wrong");
        if (!close_enough(info.duration_seconds, 4.0 / 44100.0))
            return fail("probe_track", "WAV duration wrong");
        if (info.display_name != "wasio_test_probe.wav")
            return fail("probe_track", "WAV display_name wrong");
    }

    {
        const auto fixture = make_dff_fixture();
        TempFile temp("wasio_test_probe.dff", fixture.file);
        if (!temp.ok()) return fail("probe_track", "could not write temp DFF");
        const auto info = wasio::probe_track(temp.path());
        if (!info.valid) return fail("probe_track", ("DFF probe failed: " + info.error).c_str());
        if (info.kind != wasio::TrackKind::DsdFile) return fail("probe_track", "DFF kind is not DsdFile");
        if (info.sample_rate != kDsdRate64) return fail("probe_track", "DFF sample rate wrong");
        if (info.total_frames != fixture.frames) return fail("probe_track", "DFF total_frames wrong");
        // One DSD frame carries 8 one-bit samples per channel.
        if (!close_enough(info.duration_seconds, 3.0 * 8.0 / kDsdRate64))
            return fail("probe_track", "DFF duration wrong");
    }

    {
        const auto fixture = make_dsf_fixture();
        TempFile temp("wasio_test_probe.dsf", fixture.file);
        if (!temp.ok()) return fail("probe_track", "could not write temp DSF");
        const auto info = wasio::probe_track(temp.path());
        if (!info.valid) return fail("probe_track", ("DSF probe failed: " + info.error).c_str());
        if (info.kind != wasio::TrackKind::DsdFile) return fail("probe_track", "DSF kind is not DsdFile");
        if (info.total_frames != fixture.frames) return fail("probe_track", "DSF total_frames wrong");
        if (!close_enough(info.duration_seconds, 9.0 * 8.0 / kDsdRate64))
            return fail("probe_track", "DSF duration wrong");
    }

    // probe_track must not hold the file open: a reader can still open it, and
    // the TempFile destructor above must be able to delete it.
    {
        const auto fixture = make_wav_fixture();
        TempFile temp("wasio_test_probe_reopen.wav", fixture.file);
        if (!temp.ok()) return fail("probe_track", "could not write reopen fixture");
        const auto info = wasio::probe_track(temp.path());
        if (!info.valid) return fail("probe_track", "reopen fixture probe failed");
        wasio::WavPcmSource source(temp.path());
        if (!source.valid()) return fail("probe_track", "file still locked after probe_track");
    }

    const auto missing = wasio::probe_track("wasio_test_does_not_exist.wav");
    if (missing.valid) return fail("probe_track", "missing file reported valid");
    if (missing.error.empty()) return fail("probe_track", "missing file has no error text");

    const auto unsupported = wasio::probe_track("track.flac");
    if (unsupported.valid) return fail("probe_track", "unsupported extension reported valid");
    if (unsupported.kind != wasio::TrackKind::Unknown)
        return fail("probe_track", "unsupported extension has a kind");

    return true;
}

// ---- playlist ----

wasio::TrackInfo fake_track(const std::string& name, double duration = 60.0)
{
    wasio::TrackInfo track;
    track.path = "C:\\music\\" + name;
    track.display_name = name;
    track.kind = wasio::TrackKind::PcmFile;
    track.sample_rate = 44100.0;
    track.channels = 2;
    track.total_frames = static_cast<std::uint64_t>(duration * 44100.0);
    track.duration_seconds = duration;
    track.valid = true;
    return track;
}

wasio::Playlist make_playlist(std::size_t count)
{
    wasio::Playlist playlist;
    for (std::size_t i = 0; i < count; ++i) {
        playlist.add(fake_track(std::to_string(i) + ".wav"));
    }
    playlist.set_current(0);
    return playlist;
}

bool test_playlist_edit()
{
    wasio::Playlist playlist = make_playlist(3);
    if (playlist.size() != 3) return fail("playlist", "wrong size after add");

    // Moving the current track must carry the cursor with it.
    if (!playlist.move_down(0)) return fail("playlist", "move_down(0) failed");
    if (playlist.at(0).display_name != "1.wav") return fail("playlist", "move_down did not swap");
    if (playlist.current_index() != 1) return fail("playlist", "cursor did not follow move_down");
    if (!playlist.move_up(1)) return fail("playlist", "move_up(1) failed");
    if (playlist.at(0).display_name != "0.wav") return fail("playlist", "move_up did not swap back");
    if (playlist.current_index() != 0) return fail("playlist", "cursor did not follow move_up");

    if (playlist.move_up(0)) return fail("playlist", "move_up at the top reported success");
    if (playlist.move_down(2)) return fail("playlist", "move_down at the bottom reported success");

    // Removing a track before the cursor keeps the same track current.
    playlist.set_current(2);
    if (!playlist.remove(0)) return fail("playlist", "remove(0) failed");
    if (playlist.size() != 2) return fail("playlist", "wrong size after remove");
    if (playlist.current_index() != 1) return fail("playlist", "cursor did not shift after remove");
    if (playlist.at(playlist.current_index()).display_name != "2.wav")
        return fail("playlist", "cursor points at the wrong track after remove");

    playlist.clear();
    if (!playlist.empty()) return fail("playlist", "clear() left tracks");
    if (playlist.current_index() != wasio::Playlist::kInvalidIndex)
        return fail("playlist", "clear() left a cursor");
    return true;
}

bool test_playlist_repeat()
{
    // Repeat Off stops at the end and at the start.
    wasio::Playlist playlist = make_playlist(3);
    playlist.set_repeat(wasio::RepeatMode::Off);
    if (!playlist.next(true) || playlist.current_index() != 1)
        return fail("repeat", "Off did not advance to 1");
    if (!playlist.next(true) || playlist.current_index() != 2)
        return fail("repeat", "Off did not advance to 2");
    if (playlist.next(true)) return fail("repeat", "Off advanced past the last track");
    playlist.set_current(0);
    if (playlist.previous()) return fail("repeat", "Off moved before the first track");

    // Repeat All wraps in both directions.
    playlist.set_repeat(wasio::RepeatMode::All);
    playlist.set_current(2);
    if (!playlist.next(true) || playlist.current_index() != 0)
        return fail("repeat", "All did not wrap forwards");
    if (!playlist.previous() || playlist.current_index() != 2)
        return fail("repeat", "All did not wrap backwards");

    // Repeat One replays only on automatic advance; a pressed next still moves.
    playlist.set_repeat(wasio::RepeatMode::One);
    playlist.set_current(1);
    if (!playlist.next(true) || playlist.current_index() != 1)
        return fail("repeat", "One did not replay on automatic advance");
    if (!playlist.next(false) || playlist.current_index() != 2)
        return fail("repeat", "One did not move on a requested next");
    return true;
}

bool test_playlist_shuffle()
{
    constexpr std::size_t kCount = 6;
    wasio::Playlist playlist = make_playlist(kCount);
    playlist.set_current(3);
    playlist.set_shuffle(true);
    if (!playlist.shuffle()) return fail("shuffle", "shuffle() did not report on");
    // Turning shuffle on starts the round at the current track, so it must not
    // be revisited later in the round.
    if (playlist.current_index() != 3) return fail("shuffle", "shuffle moved the current track");

    // One round must cover every track exactly once.
    std::set<std::size_t> seen;
    seen.insert(playlist.current_index());
    playlist.set_repeat(wasio::RepeatMode::Off);
    for (std::size_t i = 0; i + 1 < kCount; ++i) {
        if (!playlist.next(true)) return fail("shuffle", "round ended early");
        if (!seen.insert(playlist.current_index()).second)
            return fail("shuffle", "a track repeated inside one round");
    }
    if (seen.size() != kCount) return fail("shuffle", "round did not cover every track");
    if (playlist.next(true)) return fail("shuffle", "Off advanced past the end of the round");

    // Turning shuffle off restores plain order from the current track.
    const std::size_t current = playlist.current_index();
    playlist.set_shuffle(false);
    if (playlist.shuffle()) return fail("shuffle", "shuffle() did not report off");
    if (current + 1 < kCount) {
        if (!playlist.next(true) || playlist.current_index() != current + 1)
            return fail("shuffle", "order did not return to sequential");
    }

    // Starting a shuffled list from scratch has to enter at the head of the
    // permutation. Entering at track 0 instead lands mid-round and silently
    // drops every track ordered before it.
    wasio::Playlist fresh;
    for (std::size_t i = 0; i < kCount; ++i) fresh.add(fake_track(std::to_string(i) + ".wav"));
    fresh.set_shuffle(true);
    fresh.set_repeat(wasio::RepeatMode::Off);
    if (fresh.current_index() != wasio::Playlist::kInvalidIndex)
        return fail("shuffle", "a fresh list should have no current track");
    const std::size_t start = fresh.first_index();
    if (start == wasio::Playlist::kInvalidIndex) return fail("shuffle", "first_index() is invalid");
    fresh.set_current(start);
    std::set<std::size_t> fresh_seen{start};
    while (fresh.next(true)) {
        if (!fresh_seen.insert(fresh.current_index()).second)
            return fail("shuffle", "a track repeated in a round started from first_index()");
    }
    if (fresh_seen.size() != kCount)
        return fail("shuffle", "round started from first_index() skipped tracks");
    return true;
}

bool test_m3u8_round_trip()
{
    // Two real files so probing succeeds, one of them with a non-ASCII name to
    // prove UTF-8 paths survive the write/read cycle and still open.
    const auto fixture = make_wav_fixture();
    TempFile ascii("wasio_test_list_a.wav", fixture.file);
    TempFile unicode("wasio_test_\xE6\xB8\xAC\xE8\xA9\xA6.wav", fixture.file);
    if (!ascii.ok() || !unicode.ok()) return fail("m3u8", "could not write fixture WAVs");

    wasio::Playlist saved;
    saved.add(wasio::probe_track(ascii.path()));
    saved.add(wasio::probe_track(unicode.path()));
    saved.add(wasio::probe_track("wasio_test_missing_entry.wav"));
    if (!saved.at(0).valid) return fail("m3u8", "ASCII fixture did not probe");
    if (!saved.at(1).valid) return fail("m3u8", "non-ASCII fixture did not probe");

    const std::string list_path = "wasio_test_playlist.m3u8";
    std::string error;
    if (!saved.save_m3u8(list_path, &error)) return fail("m3u8", ("save failed: " + error).c_str());

    // The file must be UTF-8 without a BOM.
    {
        std::ifstream raw(list_path, std::ios::binary);
        std::string first(3, '\0');
        raw.read(first.data(), 3);
        if (static_cast<unsigned char>(first[0]) == 0xEF &&
            static_cast<unsigned char>(first[1]) == 0xBB &&
            static_cast<unsigned char>(first[2]) == 0xBF) {
            std::remove(list_path.c_str());
            return fail("m3u8", "saved file starts with a BOM");
        }
    }

    wasio::Playlist loaded;
    const bool ok = loaded.load_m3u8(list_path, &error);
    std::remove(list_path.c_str());
    if (!ok) return fail("m3u8", ("load failed: " + error).c_str());

    if (loaded.size() != 3) return fail("m3u8", "wrong entry count after load");
    if (loaded.at(0).path != saved.at(0).path) return fail("m3u8", "ASCII path did not round trip");
    if (loaded.at(1).path != saved.at(1).path)
        return fail("m3u8", "non-ASCII path did not round trip");
    if (!loaded.at(1).valid) return fail("m3u8", "non-ASCII entry did not reopen after load");
    if (loaded.at(1).channels != 2 || loaded.at(1).sample_rate != 44100.0)
        return fail("m3u8", "non-ASCII entry lost its format");
    // A missing file stays in the list, flagged rather than dropped.
    if (loaded.at(2).valid) return fail("m3u8", "missing entry reported valid");
    if (loaded.at(2).error.empty()) return fail("m3u8", "missing entry has no error text");
    return true;
}

bool test_m3u8_relative_paths()
{
    const auto fixture = make_wav_fixture();
    TempFile track("wasio_test_relative.wav", fixture.file);
    if (!track.ok()) return fail("m3u8_relative", "could not write fixture WAV");

    // Hand-write a playlist that refers to the track by bare file name; it must
    // resolve against the playlist file's own directory.
    const std::string list_path = "wasio_test_relative.m3u8";
    {
        std::ofstream list(list_path, std::ios::binary | std::ios::trunc);
        if (!list) return fail("m3u8_relative", "could not write playlist");
        list << "#EXTM3U\n#EXTINF:1,relative entry\nwasio_test_relative.wav\n";
    }

    wasio::Playlist loaded;
    std::string error;
    const bool ok = loaded.load_m3u8(list_path, &error);
    std::remove(list_path.c_str());
    if (!ok) return fail("m3u8_relative", ("load failed: " + error).c_str());
    if (loaded.size() != 1) return fail("m3u8_relative", "wrong entry count");
    if (!loaded.at(0).valid) return fail("m3u8_relative", "relative entry did not resolve");
    if (loaded.at(0).channels != 2) return fail("m3u8_relative", "relative entry lost its format");

    // An unreadable entry keeps the name and duration the playlist claimed.
    const std::string missing_path = "wasio_test_missing.m3u8";
    {
        std::ofstream list(missing_path, std::ios::binary | std::ios::trunc);
        if (!list) return fail("m3u8_relative", "could not write missing-entry playlist");
        list << "#EXTM3U\n#EXTINF:123,Ghost Track\nno_such_directory\\ghost.wav\n";
    }
    wasio::Playlist ghosts;
    const bool ghost_ok = ghosts.load_m3u8(missing_path, &error);
    std::remove(missing_path.c_str());
    if (!ghost_ok) return fail("m3u8_relative", "missing-entry playlist did not load");
    if (ghosts.size() != 1) return fail("m3u8_relative", "missing entry was dropped");
    if (ghosts.at(0).valid) return fail("m3u8_relative", "missing entry reported valid");
    if (ghosts.at(0).display_name != "Ghost Track")
        return fail("m3u8_relative", "missing entry lost its EXTINF name");
    if (!close_enough(ghosts.at(0).duration_seconds, 123.0))
        return fail("m3u8_relative", "missing entry lost its EXTINF duration");
    return true;
}

// ---- PlayerController with a fake engine ----

// Stands in for a real backend so the track-change state machine can be driven
// without touching hardware. finish() is what a real engine reports when the
// source drains.
class FakeEngine final : public wasio::IPlaybackEngine {
public:
    struct Shared {
        int started = 0;
        int stopped = 0;
        std::vector<std::string> played;
        bool fail_start = false;
        // What a refused start() should report, so the controller's mapping
        // onto PlayerErrorKind can be checked both ways.
        wasio::EngineStartFailure start_failure = wasio::EngineStartFailure::DeviceOpen;
        std::string render_error;
    };

    FakeEngine(Shared* shared, std::string name) : shared_(shared), name_(std::move(name)) {}

    bool start(std::string* error_message) override
    {
        if (shared_->fail_start) {
            if (error_message) *error_message = "fake start failure";
            return false;
        }
        ++shared_->started;
        shared_->played.push_back(name_);
        running_ = true;
        return true;
    }
    wasio::EngineStartFailure start_failure() const override { return shared_->start_failure; }
    void stop() override
    {
        if (running_) ++shared_->stopped;
        running_ = false;
    }
    bool is_running() const override { return running_; }
    bool playback_finished() const override { return finished_; }
    void set_paused(bool paused) override { paused_ = paused; }
    bool is_paused() const override { return paused_; }
    bool request_seek(std::uint64_t source_frame) override
    {
        position_ = source_frame;
        return true;
    }
    bool seek_in_progress() const override { return false; }
    std::uint64_t output_position_frames() const override { return position_; }
    std::uint64_t total_output_frames() const override { return 44100 * 60; }
    double output_frame_rate() const override { return 44100.0; }
    std::size_t source_frames_per_output() const override { return 1; }
    std::uint64_t underrun_count() const override { return 0; }
    std::uint64_t callback_count() const override { return 42; }
    std::string format_description() const override { return "fake " + name_; }
    // Mirrors WasapiPlayback: only reported once stop() has joined the thread.
    std::string error_message() const override
    {
        return running_ ? std::string{} : shared_->render_error;
    }

    void finish() { finished_ = true; }

private:
    Shared* shared_;
    std::string name_;
    bool running_ = false;
    bool finished_ = false;
    bool paused_ = false;
    std::uint64_t position_ = 0;
};

bool test_player_controller()
{
    FakeEngine::Shared shared;
    FakeEngine* live = nullptr;
    // The factory is the test seam; capturing the latest engine lets the test
    // say "this track just ended".
    auto factory = [&shared, &live](const wasio::EngineRequest& request,
                                    std::string* error_message)
        -> std::unique_ptr<wasio::IPlaybackEngine> {
        auto engine = std::make_unique<FakeEngine>(&shared, request.track.display_name);
        if (shared.fail_start) {
            // Mirror the real factory: a refused track never becomes an engine.
            if (error_message) *error_message = "fake factory failure";
            return nullptr;
        }
        live = engine.get();
        return engine;
    };

    wasio::PlayerController controller(factory);
    auto& playlist = controller.playlist();
    playlist.add(fake_track("a.wav"));
    playlist.add(fake_track("b.wav"));
    playlist.add(fake_track("c.wav"));

    if (!controller.play(0)) return fail("controller", "play(0) failed");
    if (controller.status().state != wasio::PlayerState::Playing)
        return fail("controller", "state is not Playing after play()");
    if (controller.status().track_name != "a.wav")
        return fail("controller", "wrong track name after play()");

    // Pause and resume drive the engine flag and the reported state together.
    controller.pause();
    if (controller.status().state != wasio::PlayerState::Paused)
        return fail("controller", "state is not Paused after pause()");
    if (!live->is_paused()) return fail("controller", "engine was not paused");
    controller.resume();
    if (controller.status().state != wasio::PlayerState::Playing)
        return fail("controller", "state is not Playing after resume()");
    if (live->is_paused()) return fail("controller", "engine stayed paused after resume()");

    // poll() must notice the end of a track and start the next one.
    live->finish();
    auto status = controller.poll();
    if (status.state != wasio::PlayerState::Playing)
        return fail("controller", "auto-advance did not keep playing");
    if (status.track_name != "b.wav") return fail("controller", "auto-advance picked wrong track");
    if (playlist.current_index() != 1)
        return fail("controller", "playlist cursor did not follow auto-advance");
    if (shared.stopped != 1) return fail("controller", "previous engine was not stopped");

    // Repeat Off stops after the last track.
    live->finish();
    status = controller.poll();
    if (status.track_name != "c.wav") return fail("controller", "did not advance to the last track");
    live->finish();
    status = controller.poll();
    if (status.state != wasio::PlayerState::Stopped)
        return fail("controller", "did not stop at the end with Repeat Off");

    // Repeat All wraps instead of stopping.
    playlist.set_repeat(wasio::RepeatMode::All);
    if (!controller.play(2)) return fail("controller", "play(2) failed");
    live->finish();
    status = controller.poll();
    if (status.state != wasio::PlayerState::Playing || status.track_name != "a.wav")
        return fail("controller", "Repeat All did not wrap to the first track");

    // Repeat One replays the same track on an automatic advance.
    playlist.set_repeat(wasio::RepeatMode::One);
    live->finish();
    status = controller.poll();
    if (status.track_name != "a.wav") return fail("controller", "Repeat One did not replay");
    // ...but a pressed next still moves on.
    if (!controller.next()) return fail("controller", "next() failed");
    if (controller.status().track_name != "b.wav")
        return fail("controller", "next() did not move past Repeat One");
    if (!controller.previous()) return fail("controller", "previous() failed");
    if (controller.status().track_name != "a.wav")
        return fail("controller", "previous() went to the wrong track");

    // seek() converts seconds into source frames through the engine's rate.
    if (!controller.seek(10.0)) return fail("controller", "seek() failed");
    if (live->output_position_frames() != static_cast<std::uint64_t>(10.0 * 44100.0))
        return fail("controller", "seek() converted seconds incorrectly");
    // Out-of-range seeks clamp instead of failing.
    if (!controller.seek(-5.0)) return fail("controller", "negative seek failed");
    if (live->output_position_frames() != 0) return fail("controller", "negative seek did not clamp");

    controller.stop();
    if (controller.status().state != wasio::PlayerState::Stopped)
        return fail("controller", "stop() did not reach Stopped");

    // A factory that refuses leaves the controller stopped with the reason.
    shared.fail_start = true;
    if (controller.play(0)) return fail("controller", "play() succeeded with a failing factory");
    if (controller.status().state != wasio::PlayerState::Stopped)
        return fail("controller", "failed play() did not leave Stopped");
    if (controller.status().error.empty())
        return fail("controller", "failed play() reported no error");
    return true;
}

// The CLI needs distinct exit codes, so the controller has to say *why* a
// track was refused rather than leaving the CLI to parse message text.
bool test_player_error_kinds()
{
    FakeEngine::Shared shared;
    auto factory = [&shared](const wasio::EngineRequest& request, std::string* error_message)
        -> std::unique_ptr<wasio::IPlaybackEngine> {
        if (shared.fail_start) {
            if (error_message) *error_message = "fake factory failure";
            return nullptr;
        }
        return std::make_unique<FakeEngine>(&shared, request.track.display_name);
    };

    {
        // An unreadable track is a file problem, not a device problem.
        wasio::PlayerController controller(factory);
        wasio::TrackInfo broken;
        broken.path = "C:\\music\\broken.wav";
        broken.display_name = "broken.wav";
        broken.valid = false;
        broken.error = "not a RIFF/WAVE file";
        controller.playlist().add(broken);
        if (controller.play(0)) return fail("error_kinds", "play() accepted an invalid track");
        if (controller.status().error_kind != wasio::PlayerErrorKind::FileFailed)
            return fail("error_kinds", "invalid track is not FileFailed");
    }

    {
        // DSD512 asked for as DoP must be refused with its own code, before any
        // device is touched.
        wasio::PlayerController controller(factory);
        controller.set_dsd_mode(wasio::DsdOutputMode::DoP);
        wasio::TrackInfo dsd512 = fake_track("dsd512.dff");
        dsd512.kind = wasio::TrackKind::DsdFile;
        dsd512.sample_rate = 22579200.0;
        controller.playlist().add(dsd512);
        if (controller.play(0)) return fail("error_kinds", "play() accepted DoP512");
        if (controller.status().error_kind != wasio::PlayerErrorKind::DoP512Rejected)
            return fail("error_kinds", "DoP512 is not DoP512Rejected");

        // The same file in Native DSD mode is fine.
        controller.set_dsd_mode(wasio::DsdOutputMode::Native);
        if (!controller.play(0)) return fail("error_kinds", "Native DSD512 was refused");
        controller.stop();
    }

    {
        // A device that opens but refuses the format maps to its own code.
        wasio::PlayerController controller(factory);
        controller.playlist().add(fake_track("a.wav"));
        shared.fail_start = false;
        shared.start_failure = wasio::EngineStartFailure::FormatNegotiation;
        // The factory hands back an engine whose start() then fails.
        auto failing = [&shared](const wasio::EngineRequest& request, std::string* message)
            -> std::unique_ptr<wasio::IPlaybackEngine> {
            (void)message;
            shared.fail_start = true; // makes the engine's start() refuse
            return std::make_unique<FakeEngine>(&shared, request.track.display_name);
        };
        wasio::PlayerController format_controller(failing);
        format_controller.playlist().add(fake_track("a.wav"));
        if (format_controller.play(0)) return fail("error_kinds", "play() ignored a start failure");
        if (format_controller.status().error_kind !=
            wasio::PlayerErrorKind::FormatNegotiationFailed) {
            return fail("error_kinds", "format refusal is not FormatNegotiationFailed");
        }

        shared.start_failure = wasio::EngineStartFailure::DeviceOpen;
        wasio::PlayerController device_controller(failing);
        device_controller.playlist().add(fake_track("a.wav"));
        if (device_controller.play(0)) return fail("error_kinds", "play() ignored a start failure");
        if (device_controller.status().error_kind != wasio::PlayerErrorKind::DeviceOpenFailed)
            return fail("error_kinds", "device refusal is not DeviceOpenFailed");
    }

    {
        // A render thread that dies mid-track is only reported once stop() has
        // joined it, which is what the CLI's exit code 7 depends on.
        shared.fail_start = false;
        shared.start_failure = wasio::EngineStartFailure::DeviceOpen;
        shared.render_error = "the WASAPI render event timed out";
        wasio::PlayerController controller(factory);
        controller.playlist().add(fake_track("a.wav"));
        if (!controller.play(0)) return fail("error_kinds", "play() failed unexpectedly");
        if (!controller.status().error.empty())
            return fail("error_kinds", "render error surfaced before stop()");
        controller.stop();
        if (controller.status().error != shared.render_error)
            return fail("error_kinds", "render error was not captured by stop()");
        if (controller.status().error_kind != wasio::PlayerErrorKind::PlaybackInterrupted)
            return fail("error_kinds", "render error is not PlaybackInterrupted");
        shared.render_error.clear();
    }

    return true;
}

} // namespace

int main()
{
    const struct {
        const char* name;
        bool (*run)();
    } tests[] = {
        {"ring_buffer", test_ring_buffer}, {"dsd_packer", test_dsd_packer},
        {"wav_reader", test_wav_reader},   {"dff_reader", test_dff_reader},
        {"dsf_reader", test_dsf_reader},   {"wav_seek", test_wav_seek},
        {"dff_seek", test_dff_seek},       {"dsf_seek", test_dsf_seek},
        {"probe_track", test_probe_track}, {"playlist_edit", test_playlist_edit},
        {"playlist_repeat", test_playlist_repeat},
        {"playlist_shuffle", test_playlist_shuffle},
        {"m3u8_round_trip", test_m3u8_round_trip},
        {"m3u8_relative", test_m3u8_relative_paths},
        {"player_controller", test_player_controller},
        {"player_error_kinds", test_player_error_kinds},
    };

    for (std::size_t i = 0; i < std::size(tests); ++i) {
        if (!tests[i].run()) {
            std::cerr << "FAILED: " << tests[i].name << "\n";
            return static_cast<int>(i) + 1;
        }
    }

    std::cout << "All checks passed:";
    for (const auto& test : tests) std::cout << " " << test.name;
    std::cout << "\n";
    return 0;
}
