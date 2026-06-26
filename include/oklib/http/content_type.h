#pragma once

#include <span>
#include <string_view>

namespace oklib::http {

enum class ContentType {
  none = 0,

  text_plain = 100,
  text_html,
  text_css,
  text_csv,
  text_markdown,
  text_event_stream,

  application_javascript = 200,
  application_json,
  application_xml,
  application_urlencoded,
  application_octet_stream,
  application_zip,
  application_gzip,
  application_7z,
  application_rar,
  application_pdf,
  application_rtf,
  application_grpc,
  application_wasm,
  application_jar,
  application_xhtml,
  application_atom,
  application_rss,
  application_word,
  application_excel,
  application_powerpoint,
  application_eot,
  application_m3u8,
  application_docx,
  application_xlsx,
  application_pptx,

  multipart_form_data = 300,

  image_jpeg = 400,
  image_png,
  image_gif,
  image_ico,
  image_bmp,
  image_svg,
  image_tiff,
  image_webp,

  video_mp4 = 500,
  video_flv,
  video_m4v,
  video_mng,
  video_ts,
  video_mpeg,
  video_webm,
  video_mov,
  video_3gpp,
  video_avi,
  video_wmv,
  video_asf,

  audio_mp3 = 600,
  audio_ogg,
  audio_m4a,
  audio_aac,
  audio_pcma,
  audio_opus,

  font_ttf = 700,
  font_otf,
  font_woff,
  font_woff2,

  undefined = 1000,
};

struct ContentTypeInfo {
  ContentType type;
  std::string_view mime;
  std::string_view suffix;
};

[[nodiscard]] std::span<const ContentTypeInfo> known_content_types() noexcept;
[[nodiscard]] std::string_view content_type_to_string(ContentType type) noexcept;
[[nodiscard]] std::string_view content_type_suffix(ContentType type) noexcept;
[[nodiscard]] ContentType content_type_from_string(std::string_view value) noexcept;
[[nodiscard]] ContentType content_type_from_suffix(std::string_view suffix) noexcept;
[[nodiscard]] ContentType content_type_from_path(std::string_view path) noexcept;

}  // namespace oklib::http
