#include "player/playlist.h"

#include "formats/utf8_file.h"

#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <sstream>

namespace wasio {
namespace {

bool is_absolute_path(const std::string& path)
{
    if (path.size() >= 2 && path[1] == ':') return true;              // C:\...
    if (path.size() >= 2 && (path[0] == '\\' || path[0] == '/') &&
        (path[1] == '\\' || path[1] == '/')) {
        return true; // UNC \\server\share
    }
    return !path.empty() && (path[0] == '\\' || path[0] == '/');
}

std::string directory_of(const std::string& path)
{
    const std::size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? std::string{} : path.substr(0, separator);
}

std::string trim(const std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Turns a playlist entry into something openable, leaving absolute paths alone.
std::string resolve_entry(const std::string& entry, const std::string& base_directory)
{
    if (entry.empty() || is_absolute_path(entry) || base_directory.empty()) return entry;
    return base_directory + "\\" + entry;
}

} // namespace

const char* repeat_mode_name(RepeatMode mode)
{
    switch (mode) {
    case RepeatMode::Off: return "off";
    case RepeatMode::One: return "one";
    case RepeatMode::All: return "all";
    }
    return "off";
}

bool parse_repeat_mode(const std::string& text, RepeatMode* mode)
{
    if (mode == nullptr) return false;
    if (text == "off") { *mode = RepeatMode::Off; return true; }
    if (text == "one") { *mode = RepeatMode::One; return true; }
    if (text == "all") { *mode = RepeatMode::All; return true; }
    return false;
}

Playlist::Playlist(std::uint32_t shuffle_seed) : random_(shuffle_seed) {}

void Playlist::add(TrackInfo track)
{
    tracks_.push_back(std::move(track));
    rebuild_order();
}

bool Playlist::remove(std::size_t index)
{
    if (index >= tracks_.size()) return false;
    tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(index));
    if (tracks_.empty()) {
        current_ = kInvalidIndex;
    } else if (current_ != kInvalidIndex) {
        // Removing the current track leaves the cursor on whatever slid up into
        // its slot, so the list keeps playing from the same place.
        if (current_ == index) {
            current_ = std::min(index, tracks_.size() - 1);
        } else if (current_ > index) {
            --current_;
        }
    }
    rebuild_order();
    return true;
}

void Playlist::clear()
{
    tracks_.clear();
    order_.clear();
    current_ = kInvalidIndex;
}

bool Playlist::move_up(std::size_t index)
{
    if (index == 0 || index >= tracks_.size()) return false;
    std::swap(tracks_[index - 1], tracks_[index]);
    if (current_ == index) current_ = index - 1;
    else if (current_ == index - 1) current_ = index;
    rebuild_order();
    return true;
}

bool Playlist::move_down(std::size_t index)
{
    if (tracks_.empty() || index + 1 >= tracks_.size()) return false;
    std::swap(tracks_[index], tracks_[index + 1]);
    if (current_ == index) current_ = index + 1;
    else if (current_ == index + 1) current_ = index;
    rebuild_order();
    return true;
}

bool Playlist::set_current(std::size_t index)
{
    if (index >= tracks_.size()) return false;
    current_ = index;
    return true;
}

void Playlist::set_shuffle(bool on)
{
    if (shuffle_ == on) return;
    shuffle_ = on;
    rebuild_order();
}

void Playlist::rebuild_order()
{
    order_.resize(tracks_.size());
    std::iota(order_.begin(), order_.end(), std::size_t{0});
    if (!shuffle_ || tracks_.size() < 2) return;

    std::shuffle(order_.begin(), order_.end(), random_);
    // The current track starts the round so it is not played twice in a row and
    // still appears exactly once.
    if (current_ != kInvalidIndex) {
        const auto found = std::find(order_.begin(), order_.end(), current_);
        if (found != order_.end()) std::iter_swap(order_.begin(), found);
    }
}

std::size_t Playlist::order_position_of(std::size_t track_index) const
{
    const auto found = std::find(order_.begin(), order_.end(), track_index);
    if (found == order_.end()) return kInvalidIndex;
    return static_cast<std::size_t>(found - order_.begin());
}

std::size_t Playlist::first_index() const
{
    return order_.empty() ? kInvalidIndex : order_.front();
}

std::size_t Playlist::next_index(bool automatic) const
{
    if (tracks_.empty()) return kInvalidIndex;
    if (current_ == kInvalidIndex) return order_.empty() ? kInvalidIndex : order_.front();
    // Repeat One only applies when the track ended on its own; pressing next
    // still moves on, which is what every player does.
    if (automatic && repeat_ == RepeatMode::One) return current_;

    const std::size_t position = order_position_of(current_);
    if (position == kInvalidIndex) return order_.empty() ? kInvalidIndex : order_.front();
    if (position + 1 < order_.size()) return order_[position + 1];
    return repeat_ == RepeatMode::All ? order_.front() : kInvalidIndex;
}

std::size_t Playlist::previous_index() const
{
    if (tracks_.empty()) return kInvalidIndex;
    if (current_ == kInvalidIndex) return order_.empty() ? kInvalidIndex : order_.front();
    const std::size_t position = order_position_of(current_);
    if (position == kInvalidIndex) return order_.empty() ? kInvalidIndex : order_.front();
    if (position > 0) return order_[position - 1];
    return repeat_ == RepeatMode::All ? order_.back() : kInvalidIndex;
}

bool Playlist::next(bool automatic)
{
    const std::size_t index = next_index(automatic);
    if (index == kInvalidIndex) return false;
    // A new shuffle round starts when All wraps past the end, so every track
    // gets a fresh permutation instead of repeating the same order forever.
    const bool wrapped = shuffle_ && repeat_ == RepeatMode::All && current_ != kInvalidIndex &&
                         order_position_of(current_) + 1 >= order_.size();
    current_ = index;
    if (wrapped) rebuild_order();
    return true;
}

bool Playlist::previous()
{
    const std::size_t index = previous_index();
    if (index == kInvalidIndex) return false;
    current_ = index;
    return true;
}

bool Playlist::save_m3u8(const std::string& path, std::string* error_message) const
{
    auto file = open_utf8_ofstream(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error_message) *error_message = "cannot write playlist file";
        return false;
    }
    // UTF-8 without BOM, LF line endings.
    file << "#EXTM3U\n";
    for (const auto& track : tracks_) {
        const auto seconds = static_cast<long long>(track.duration_seconds + 0.5);
        file << "#EXTINF:" << seconds << "," << track.display_name << "\n";
        file << track.path << "\n";
    }
    if (!file) {
        if (error_message) *error_message = "writing the playlist file failed";
        return false;
    }
    return true;
}

bool Playlist::load_m3u8(const std::string& path, std::string* error_message)
{
    auto file = open_utf8_ifstream(path, std::ios::binary);
    if (!file) {
        if (error_message) *error_message = "cannot read playlist file";
        return false;
    }
    const std::string base = directory_of(path);

    std::vector<TrackInfo> loaded;
    std::string line;
    std::string pending_name;
    double pending_duration = 0.0;
    bool first_line = true;
    while (std::getline(file, line)) {
        if (first_line) {
            // Tolerate a BOM even though we never write one.
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            first_line = false;
        }
        const std::string entry = trim(line);
        if (entry.empty()) continue;
        if (entry[0] == '#') {
            if (entry.rfind("#EXTINF:", 0) == 0) {
                const std::string payload = entry.substr(8);
                const std::size_t comma = payload.find(',');
                pending_duration = std::atof(payload.substr(0, comma).c_str());
                pending_name = comma == std::string::npos ? std::string{} : payload.substr(comma + 1);
            }
            continue;
        }

        TrackInfo track = probe_track(resolve_entry(entry, base));
        if (!track.valid) {
            // Keep unreadable entries visible instead of dropping them, and
            // show whatever the playlist claimed about them.
            if (!pending_name.empty()) track.display_name = pending_name;
            if (pending_duration > 0.0) track.duration_seconds = pending_duration;
        }
        loaded.push_back(std::move(track));
        pending_name.clear();
        pending_duration = 0.0;
    }

    tracks_ = std::move(loaded);
    // Nothing is playing yet, so leave the cursor unset: pinning it to track 0
    // would make set_shuffle() anchor the round there and always start with the
    // same track. first_index() is what picks the entry point.
    current_ = kInvalidIndex;
    rebuild_order();
    return true;
}

} // namespace wasio
