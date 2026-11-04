#include "io/remux.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <stdexcept>

namespace bc {
namespace {
void check(int rc, const char* what) {
  if (rc < 0) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(rc, buf, sizeof(buf));
    throw std::runtime_error(std::string(what) + ": " + buf);
  }
}
}  // namespace

MediaPacket::~MediaPacket() { av_packet_free(&pkt); }

RemuxWriter::RemuxWriter(AVFormatContext* fmt) : fmt_(fmt) {
  scratch_ = av_packet_alloc();
  if (!scratch_) throw std::runtime_error("remux: no packet");
}

RemuxWriter::~RemuxWriter() { av_packet_free(&scratch_); }

AVStream* RemuxWriter::streamFor(const MediaPacket& p) {
  for (const Seen& s : streams_)
    if (s.key == p.params.get()) return s.out;
  if (headerWritten_) return nullptr;  // a live mpegts muxer takes no new stream
  AVStream* out = avformat_new_stream(fmt_, nullptr);
  if (!out) throw std::runtime_error("remux: no stream");
  check(avcodec_parameters_copy(out->codecpar, p.params.get()), "remux: stream parameters");
  out->codecpar->codec_tag = 0;
  out->time_base = p.timeBase;
  streams_.push_back({p.params.get(), out});
  return out;
}

void RemuxWriter::write(const MediaPacket& p) {
  if (!p.pkt) return;
  AVStream* st = streamFor(p);
  if (st == nullptr) throw std::runtime_error("remux: the input's streams changed");
  if (!headerWritten_) {
    // The header carries the stream list, so it waits for the first video keyframe; everything
    // before it (audio too — the muxer cannot know the audio stream later) is dropped.
    if (!p.video || !(p.pkt->flags & AV_PKT_FLAG_KEY)) {
      if (p.video) ++dropped_;
      return;
    }
    check(avformat_write_header(fmt_, nullptr), "remux: header");
    headerWritten_ = true;
  }
  check(av_packet_ref(scratch_, p.pkt), "remux: packet ref");
  av_packet_rescale_ts(scratch_, p.timeBase, st->time_base);
  scratch_->stream_index = st->index;
  scratch_->pos = -1;
  check(av_interleaved_write_frame(fmt_, scratch_), "remux: write");
}
}  // namespace bc