#pragma once

namespace wasio {

// Handshake between the control thread, the realtime consumer (ASIO callback
// or WASAPI render thread) and the source worker. The realtime side never
// blocks: it only inspects the state once per period and moves it along.
//
//   None        Normal playback.
//   Requested   The control thread asked for a seek. The realtime side outputs
//               silence and stops consuming. The worker finishes whatever write
//               it was in the middle of, seeks the source, and parks.
//   SourceReady The worker has repositioned the source and is provably idle, so
//               the realtime side can clear the ring (consumer-side clear is
//               the SPSC-safe direction) and install the new position without
//               racing the producer. It then returns the state to None.
//
// SourceReady is what makes this correct. Clearing the ring as soon as the
// realtime side sees Requested leaves a window in which the worker is still
// writing pre-seek audio, so up to one worker batch of stale frames survives
// the clear and is played after the seek. Clearing only while the producer is
// parked closes that window.
enum class SeekState : int {
    None = 0,
    Requested = 1,
    SourceReady = 2,
};

} // namespace wasio
