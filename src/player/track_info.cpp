#include "player/track_info.h"

#include "formats/dff_reader.h"
#include "formats/dsf_reader.h"
#include "formats/wav_reader.h"

#include <algorithm>
#include <cctype>

namespace wasio {
namespace {

std::string lowercase_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string file_extension(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    // A dot in a directory name is not an extension.
    const std::size_t separator = path.find_last_of("/\\");
    if (separator != std::string::npos && dot < separator) return {};
    return lowercase_ascii(path.substr(dot + 1));
}

std::string file_name(const std::string& path)
{
    const std::size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

void fill_dsd(TrackInfo& info, const DsdSource& source)
{
    info.kind = TrackKind::DsdFile;
    info.sample_rate = static_cast<double>(source.format().sample_rate);
    info.channels = source.format().channels;
    info.total_frames = source.total_frames();
    // One DSD frame is one packed byte per channel, so it carries 8 one-bit
    // samples of playing time.
    if (info.sample_rate > 0.0) {
        info.duration_seconds = static_cast<double>(info.total_frames) * 8.0 / info.sample_rate;
    }
    info.valid = true;
}

} // namespace

const char* track_kind_name(TrackKind kind)
{
    switch (kind) {
    case TrackKind::PcmFile: return "PCM";
    case TrackKind::DsdFile: return "DSD";
    case TrackKind::Unknown: return "unknown";
    }
    return "unknown";
}

TrackInfo probe_track(const std::string& path)
{
    TrackInfo info;
    info.path = path;
    info.display_name = file_name(path);

    const std::string extension = file_extension(path);
    if (extension == "wav") {
        WavPcmSource source(path);
        if (!source.valid()) {
            info.error = source.error();
            return info;
        }
        info.kind = TrackKind::PcmFile;
        info.sample_rate = source.format().sample_rate;
        info.channels = source.format().channels;
        info.encoding = source.format().encoding;
        info.total_frames = source.total_frames();
        if (info.sample_rate > 0.0) {
            info.duration_seconds = static_cast<double>(info.total_frames) / info.sample_rate;
        }
        info.valid = true;
        return info;
    }
    if (extension == "dff") {
        DffDsdSource source(path);
        if (!source.valid()) {
            info.error = source.error();
            return info;
        }
        fill_dsd(info, source);
        return info;
    }
    if (extension == "dsf") {
        DsfDsdSource source(path);
        if (!source.valid()) {
            info.error = source.error();
            return info;
        }
        fill_dsd(info, source);
        return info;
    }

    info.error = extension.empty() ? "file has no extension"
                                   : "unsupported file extension: " + extension;
    return info;
}

} // namespace wasio
