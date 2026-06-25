#include "websocket_compression.h"

#if OKLIB_ENABLE_WEBSOCKET_COMPRESSION
#include <zlib.h>
#endif

namespace oklib::websocket {

bool websocket_deflate(std::string_view input, std::string* output) {
#if OKLIB_ENABLE_WEBSOCKET_COMPRESSION
  z_stream stream{};
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }
  output->clear();
  unsigned char buffer[4096];
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  int ret = Z_OK;
  do {
    stream.next_out = buffer;
    stream.avail_out = sizeof(buffer);
    ret = deflate(&stream, Z_SYNC_FLUSH);
    if (ret != Z_OK) {
      deflateEnd(&stream);
      return false;
    }
    output->append(reinterpret_cast<const char*>(buffer), sizeof(buffer) - stream.avail_out);
  } while (stream.avail_out == 0);
  deflateEnd(&stream);
  if (output->size() >= 4 && output->substr(output->size() - 4) == "\x00\x00\xff\xff") {
    output->resize(output->size() - 4);
  }
  return true;
#else
  (void)input;
  (void)output;
  return false;
#endif
}

bool websocket_inflate(std::string_view input, std::string* output) {
#if OKLIB_ENABLE_WEBSOCKET_COMPRESSION
  std::string wire(input);
  wire.append("\x00\x00\xff\xff", 4);
  z_stream stream{};
  if (inflateInit2(&stream, -15) != Z_OK) {
    return false;
  }
  output->clear();
  unsigned char buffer[4096];
  stream.next_in = reinterpret_cast<Bytef*>(wire.data());
  stream.avail_in = static_cast<uInt>(wire.size());
  int ret = Z_OK;
  do {
    stream.next_out = buffer;
    stream.avail_out = sizeof(buffer);
    ret = inflate(&stream, Z_SYNC_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&stream);
      return false;
    }
    output->append(reinterpret_cast<const char*>(buffer), sizeof(buffer) - stream.avail_out);
  } while (stream.avail_out == 0 && ret != Z_STREAM_END);
  inflateEnd(&stream);
  return true;
#else
  (void)input;
  (void)output;
  return false;
#endif
}

}  // namespace oklib::websocket
