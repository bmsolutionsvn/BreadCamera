#include <doctest/doctest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "io/remux.h"
#include "support/h264_black720.h"

namespace {
// The fixture is a run of access units, each starting at the SPS that precedes its picture.
std::vector<std::span<const uint8_t>> accessUnits() {
  const uint8_t* data = bc::test::kH264Black720;
  const size_t size = bc::test::kH264Black720Size;
  std::vector<size_t> starts;
  for (size_t i = 0; i + 4 <= size;) {
    size_t sc = 0;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) sc = 3;
    else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) sc = 4;
    if (sc == 0) {
      ++i;
      continue;
    }
    if ((data[i + sc] & 0x1f) == 7) starts.push_back(i);  // SPS
    i += sc;
  }
  std::vector<std::span<const uint8_t>> aus;
  for (size_t k = 0; k < starts.size(); ++k)
    aus.emplace_back(data + starts[k], (k + 1 < starts.size() ? starts[k + 1] : size) - starts[k]);
  return aus;
}

// The SPS/PPS NAL units at the head of an access unit, as extradata would carry them.
size_t spsPpsPrefix(const uint8_t* au, size_t size) {
  size_t i = 0;
  while (i + 4 <= size) {
    size_t sc = (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) ? 3
                : (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) ? 4
                                                                                    : 0;
    if (sc == 0) break;
    const int type = au[i + sc] & 0x1f;
    if (type != 7 && type != 8) break;  // the first picture NAL ends the parameter sets
    i += sc;
    while (i + 3 <= size && !(au[i] == 0 && au[i + 1] == 0 && (au[i + 2] == 1 || au[i + 2] == 0))) ++i;
  }
  return i;
}

std::shared_ptr<const AVCodecParameters> h264Params(const uint8_t* au, size_t size) {
  auto p = std::shared_ptr<AVCodecParameters>(avcodec_parameters_alloc(), [](AVCodecParameters* x) {
    avcodec_parameters_free(&x);
  });
  p->codec_type = AVMEDIA_TYPE_VIDEO;
  p->codec_id = AV_CODEC_ID_H264;
  p->width = 1280;
  p->height = 720;
  p->format = AV_PIX_FMT_YUV420P;
  const size_t n = spsPpsPrefix(au, size);
  p->extradata = static_cast<uint8_t*>(av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE));
  std::memcpy(p->extradata, au, n);
  p->extradata_size = static_cast<int>(n);
  return p;
}

std::shared_ptr<bc::MediaPacket> packet(const std::shared_ptr<const AVCodecParameters>& params, std::span<const uint8_t> au,
                                        int64_t pts, bool key) {
  auto m = std::make_shared<bc::MediaPacket>();
  m->params = params;
  m->timeBase = {1, 30};
  m->video = true;
  m->pkt = av_packet_alloc();
  if (!m->pkt || av_new_packet(m->pkt, static_cast<int>(au.size())) < 0) throw std::runtime_error("packet");
  std::memcpy(m->pkt->data, au.data(), au.size());
  m->pkt->pts = m->pkt->dts = pts;
  if (key) m->pkt->flags |= AV_PKT_FLAG_KEY;
  return m;
}

int writeCb(void* opaque, const uint8_t* buf, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(opaque);
  out->insert(out->end(), buf, buf + size);
  return size;
}

struct Reader {
  const std::vector<uint8_t>* data;
  size_t pos = 0;
};
int readCb(void* opaque, uint8_t* buf, int size) {
  auto* r = static_cast<Reader*>(opaque);
  const size_t n = std::min<size_t>(static_cast<size_t>(size), r->data->size() - r->pos);
  std::memcpy(buf, r->data->data() + r->pos, n);
  r->pos += n;
  return n ? static_cast<int>(n) : AVERROR_EOF;
}
}  // namespace

TEST_CASE("RemuxWriter: drops until the first keyframe, then writes the packets as they came") {
  const auto aus = accessUnits();
  REQUIRE(aus.size() >= 2);
  auto params = h264Params(aus[0].data(), aus[0].size());

  std::vector<uint8_t> written;
  AVFormatContext* fmt = nullptr;
  REQUIRE(avformat_alloc_output_context2(&fmt, nullptr, "mpegts", nullptr) == 0);
  std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> fmtGuard(fmt, &avformat_free_context);
  AVIOContext* wio = avio_alloc_context(static_cast<uint8_t*>(av_malloc(4096)), 4096, 1, &written, nullptr, writeCb, nullptr);
  REQUIRE(wio != nullptr);
  fmt->pb = wio;
  fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

  bc::RemuxWriter w(fmt);
  const int n = 12;
  for (int i = 1; i < n; ++i)
    w.write(*packet(params, aus[static_cast<size_t>(i) % aus.size()], i, i % 3 == 0));
  CHECK(w.dropped() == 2);  // packets 1 and 2 precede the first keyframe
  REQUIRE(w.wroteHeader());
  REQUIRE(av_write_trailer(fmt) == 0);
  avio_context_free(&wio);

  Reader reader{&written};
  AVIOContext* rio = avio_alloc_context(static_cast<uint8_t*>(av_malloc(4096)), 4096, 0, &reader, readCb, nullptr, nullptr);
  REQUIRE(rio != nullptr);
  AVFormatContext* in = avformat_alloc_context();
  REQUIRE(in != nullptr);
  in->pb = rio;
  in->flags |= AVFMT_FLAG_CUSTOM_IO;
  REQUIRE(avformat_open_input(&in, nullptr, nullptr, nullptr) == 0);
  REQUIRE(avformat_find_stream_info(in, nullptr) >= 0);
  REQUIRE(in->nb_streams == 1);
  CHECK(in->streams[0]->codecpar->codec_id == AV_CODEC_ID_H264);
  CHECK(in->streams[0]->codecpar->width == 1280);
  CHECK(in->streams[0]->codecpar->height == 720);

  AVPacket* p = av_packet_alloc();
  REQUIRE(p != nullptr);
  int video = 0;
  while (av_read_frame(in, p) >= 0) {
    if (p->stream_index == 0) ++video;
    av_packet_unref(p);
  }
  av_packet_free(&p);
  CHECK(video == n - 1 - 2);  // every packet after the first keyframe
  avformat_close_input(&in);
  avio_context_free(&rio);
}