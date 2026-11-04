#pragma once
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/rational.h>
}

struct AVCodecParameters;
struct AVFormatContext;
struct AVPacket;
struct AVStream;

namespace bc {
// One coded packet as the input demuxed it, with what a muxer needs to take it as is.
struct MediaPacket {
  std::shared_ptr<const AVCodecParameters> params;  // shared per stream; replaced when the stream changes
  AVRational timeBase{1, 90000};
  bool video = true;
  AVPacket* pkt = nullptr;  // owned (av_packet_free on destruction); a ref of the input's packet
  MediaPacket() = default;
  MediaPacket(const MediaPacket&) = delete;
  MediaPacket& operator=(const MediaPacket&) = delete;
  ~MediaPacket();
};

// Writes MediaPackets into an AVFormatContext it does not own: makes one stream per input stream on
// first sight, writes the header once, drops packets until a video keyframe, rescales timestamps.
// A stream first seen after the header cannot be added to a live mpegts muxer: write throws and
// the owner reopens its connection.
class RemuxWriter {
 public:
  explicit RemuxWriter(AVFormatContext* fmt);
  ~RemuxWriter();
  RemuxWriter(const RemuxWriter&) = delete;
  RemuxWriter& operator=(const RemuxWriter&) = delete;
  void write(const MediaPacket& p);  // throws on a write error
  uint64_t dropped() const { return dropped_; }  // packets dropped while waiting for a keyframe
  bool wroteHeader() const { return headerWritten_; }

 private:
  AVStream* streamFor(const MediaPacket& p);  // null when a new stream arrives after the header
  AVFormatContext* fmt_;
  AVPacket* scratch_ = nullptr;
  struct Seen {
    const AVCodecParameters* key;
    AVStream* out;
  };
  std::vector<Seen> streams_;
  bool headerWritten_ = false;
  uint64_t dropped_ = 0;
};
}  // namespace bc