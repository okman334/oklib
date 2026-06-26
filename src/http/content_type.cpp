#include "oklib/http/content_type.h"

#include <array>
#include <cctype>

namespace oklib::http {
namespace {

constexpr std::array<ContentTypeInfo, 65> k_content_types{{
    {ContentType::text_plain, "text/plain", "txt"},
    {ContentType::text_html, "text/html", "html"},
    {ContentType::text_css, "text/css", "css"},
    {ContentType::text_csv, "text/csv", "csv"},
    {ContentType::text_markdown, "text/markdown", "md"},
    {ContentType::text_event_stream, "text/event-stream", "sse"},

    {ContentType::application_javascript, "application/javascript", "js"},
    {ContentType::application_json, "application/json", "json"},
    {ContentType::application_xml, "application/xml", "xml"},
    {ContentType::application_urlencoded, "application/x-www-form-urlencoded", "kv"},
    {ContentType::application_octet_stream, "application/octet-stream", "bin"},
    {ContentType::application_zip, "application/zip", "zip"},
    {ContentType::application_gzip, "application/gzip", "gzip"},
    {ContentType::application_7z, "application/x-7z-compressed", "7z"},
    {ContentType::application_rar, "application/x-rar-compressed", "rar"},
    {ContentType::application_pdf, "application/pdf", "pdf"},
    {ContentType::application_rtf, "application/rtf", "rtf"},
    {ContentType::application_grpc, "application/grpc", "grpc"},
    {ContentType::application_wasm, "application/wasm", "wasm"},
    {ContentType::application_jar, "application/java-archive", "jar"},
    {ContentType::application_xhtml, "application/xhtml+xml", "xhtml"},
    {ContentType::application_atom, "application/atom+xml", "atom"},
    {ContentType::application_rss, "application/rss+xml", "rss"},
    {ContentType::application_word, "application/msword", "doc"},
    {ContentType::application_excel, "application/vnd.ms-excel", "xls"},
    {ContentType::application_powerpoint, "application/vnd.ms-powerpoint", "ppt"},
    {ContentType::application_eot, "application/vnd.ms-fontobject", "eot"},
    {ContentType::application_m3u8, "application/vnd.apple.mpegurl", "m3u8"},
    {ContentType::application_docx,
     "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
     "docx"},
    {ContentType::application_xlsx,
     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
     "xlsx"},
    {ContentType::application_pptx,
     "application/vnd.openxmlformats-officedocument.presentationml.presentation",
     "pptx"},

    {ContentType::multipart_form_data, "multipart/form-data", "mp"},

    {ContentType::image_jpeg, "image/jpeg", "jpg"},
    {ContentType::image_png, "image/png", "png"},
    {ContentType::image_gif, "image/gif", "gif"},
    {ContentType::image_ico, "image/x-icon", "ico"},
    {ContentType::image_bmp, "image/bmp", "bmp"},
    {ContentType::image_svg, "image/svg+xml", "svg"},
    {ContentType::image_tiff, "image/tiff", "tiff"},
    {ContentType::image_webp, "image/webp", "webp"},

    {ContentType::video_mp4, "video/mp4", "mp4"},
    {ContentType::video_flv, "video/x-flv", "flv"},
    {ContentType::video_m4v, "video/x-m4v", "m4v"},
    {ContentType::video_mng, "video/x-mng", "mng"},
    {ContentType::video_ts, "video/mp2t", "ts"},
    {ContentType::video_mpeg, "video/mpeg", "mpeg"},
    {ContentType::video_webm, "video/webm", "webm"},
    {ContentType::video_mov, "video/quicktime", "mov"},
    {ContentType::video_3gpp, "video/3gpp", "3gpp"},
    {ContentType::video_avi, "video/x-msvideo", "avi"},
    {ContentType::video_wmv, "video/x-ms-wmv", "wmv"},
    {ContentType::video_asf, "video/x-ms-asf", "asf"},

    {ContentType::audio_mp3, "audio/mpeg", "mp3"},
    {ContentType::audio_ogg, "audio/ogg", "ogg"},
    {ContentType::audio_m4a, "audio/x-m4a", "m4a"},
    {ContentType::audio_aac, "audio/aac", "aac"},
    {ContentType::audio_pcma, "audio/pcma", "pcma"},
    {ContentType::audio_opus, "audio/opus", "opus"},

    {ContentType::font_ttf, "font/ttf", "ttf"},
    {ContentType::font_otf, "font/otf", "otf"},
    {ContentType::font_woff, "font/woff", "woff"},
    {ContentType::font_woff2, "font/woff2", "woff2"},
}};

bool is_ows(char ch) noexcept {
  return ch == ' ' || ch == '\t';
}

std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && is_ows(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_ows(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const auto left = static_cast<unsigned char>(lhs[i]);
    const auto right = static_cast<unsigned char>(rhs[i]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

std::string_view media_type_without_parameters(std::string_view value) noexcept {
  const auto semicolon = value.find(';');
  if (semicolon != std::string_view::npos) {
    value = value.substr(0, semicolon);
  }
  return trim(value);
}

std::string_view suffix_from_path(std::string_view path) noexcept {
  const auto query = path.find_first_of("?#");
  if (query != std::string_view::npos) {
    path = path.substr(0, query);
  }

  const auto slash = path.find_last_of("/\\");
  const auto dot = path.find_last_of('.');
  if (dot == std::string_view::npos || dot + 1 >= path.size() ||
      (slash != std::string_view::npos && dot < slash)) {
    return {};
  }
  return path.substr(dot + 1);
}

}  // namespace

std::span<const ContentTypeInfo> known_content_types() noexcept {
  return k_content_types;
}

std::string_view content_type_to_string(ContentType type) noexcept {
  for (const auto& entry : k_content_types) {
    if (entry.type == type) {
      return entry.mime;
    }
  }
  return {};
}

std::string_view content_type_suffix(ContentType type) noexcept {
  for (const auto& entry : k_content_types) {
    if (entry.type == type) {
      return entry.suffix;
    }
  }
  return {};
}

ContentType content_type_from_string(std::string_view value) noexcept {
  value = media_type_without_parameters(value);
  if (value.empty()) {
    return ContentType::none;
  }

  for (const auto& entry : k_content_types) {
    if (equals_ignore_case(value, entry.mime)) {
      return entry.type;
    }
  }
  return ContentType::undefined;
}

ContentType content_type_from_suffix(std::string_view suffix) noexcept {
  suffix = trim(suffix);
  if (!suffix.empty() && suffix.front() == '.') {
    suffix.remove_prefix(1);
  }
  if (suffix.empty()) {
    return ContentType::none;
  }

  for (const auto& entry : k_content_types) {
    if (equals_ignore_case(suffix, entry.suffix)) {
      return entry.type;
    }
  }
  return ContentType::undefined;
}

ContentType content_type_from_path(std::string_view path) noexcept {
  return content_type_from_suffix(suffix_from_path(path));
}

}  // namespace oklib::http
