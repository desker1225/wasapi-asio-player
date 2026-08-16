#pragma once

#include "player/track_info.h"

#include <cstddef>
#include <random>
#include <string>
#include <vector>

namespace wasio {

enum class RepeatMode {
    Off,
    One,
    All,
};

const char* repeat_mode_name(RepeatMode mode);
// Parses "off" / "one" / "all"; returns false on anything else.
bool parse_repeat_mode(const std::string& text, RepeatMode* mode);

class Playlist {
public:
    static constexpr std::size_t kInvalidIndex = static_cast<std::size_t>(-1);

    // Deterministic by default so tests can assert on shuffle order; the CLI
    // and GUI seed it from the clock.
    explicit Playlist(std::uint32_t shuffle_seed = 12345u);

    // ---- contents ----
    void add(TrackInfo track);
    bool remove(std::size_t index);
    void clear();
    bool move_up(std::size_t index);
    bool move_down(std::size_t index);

    std::size_t size() const { return tracks_.size(); }
    bool empty() const { return tracks_.empty(); }
    const std::vector<TrackInfo>& tracks() const { return tracks_; }
    const TrackInfo& at(std::size_t index) const { return tracks_.at(index); }

    // ---- cursor ----
    std::size_t current_index() const { return current_; }
    bool set_current(std::size_t index);

    // ---- modes ----
    RepeatMode repeat() const { return repeat_; }
    void set_repeat(RepeatMode mode) { repeat_ = mode; }
    bool shuffle() const { return shuffle_; }
    // Turning shuffle on starts a fresh round from the current track, so the
    // track playing now is not repeated inside the round.
    void set_shuffle(bool on);

    // ---- sequencing ----
    // Where playback begins when nothing is current yet. This is the head of
    // the play order, which under shuffle is NOT track 0 — starting at 0 would
    // drop into the middle of the round and cut it short.
    std::size_t first_index() const;
    // `automatic` is true when the previous track ended by itself; that is the
    // only case where RepeatMode::One replays the same index. Returns
    // kInvalidIndex when the sequence is over.
    std::size_t next_index(bool automatic) const;
    std::size_t previous_index() const;
    // next_index()/previous_index() plus set_current().
    bool next(bool automatic);
    bool previous();

    // ---- M3U8 (UTF-8, no BOM) ----
    bool save_m3u8(const std::string& path, std::string* error_message) const;
    // Relative entries resolve against the playlist file's directory. Entries
    // whose file cannot be read stay in the list with valid == false so the
    // user can see and fix them.
    bool load_m3u8(const std::string& path, std::string* error_message);

private:
    void rebuild_order();
    std::size_t order_position_of(std::size_t track_index) const;

    std::vector<TrackInfo> tracks_;
    // Playback order. Identity while shuffle is off, a permutation while on.
    std::vector<std::size_t> order_;
    std::size_t current_ = kInvalidIndex;
    RepeatMode repeat_ = RepeatMode::Off;
    bool shuffle_ = false;
    mutable std::mt19937 random_;
};

} // namespace wasio
