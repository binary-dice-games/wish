// MIT License © 2026 Binary Dice Games
/// @file video.cpp
/// @brief Registers the Video prototype in the "wish" bison namespace.
#include "src/bison/bison_object.hpp"

#include "ui_elements.hpp"

namespace bdg::wish {

using namespace bdg::bison;

void register_video() {
  auto proto = dynamic_ptr{"Video"_rkey, {}};
  proto->addField(
      "player"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Player"),
          attr<Description>("Name of the session's media player whose video to show (the name "
                            "given to open_media()). Shows a dark placeholder until the player "
                            "has a frame, for an audio-only player, or on a server without "
                            "media support."),
          attr<Category>("Content"),
          attr<Required>()});
  proto->addField(
      "width"_rkey,
      field{
          int32_t{0},
          attr<DisplayName>("Width"),
          attr<Description>("Display width in pixels; 0 uses the video's natural width (320 "
                            "before the first frame), -1 fills the available width."),
          attr<Category>("Layout"),
          attr<Range>(-1, 16384),
          attr<Step>(1)});
  proto->addField(
      "height"_rkey,
      field{
          int32_t{0},
          attr<DisplayName>("Height"),
          attr<Description>("Display height in pixels; 0 follows the width and the video's aspect "
                            "ratio, -1 fills the available height."),
          attr<Category>("Layout"),
          attr<Range>(-1, 16384),
          attr<Step>(1)});
  proto->addField(
      "keep_aspect"_rkey,
      field{
          true,
          attr<DisplayName>("Keep Aspect Ratio"),
          attr<Description>("Letterbox the video inside width x height instead of stretching it."),
          attr<Category>("Layout")});
  proto->addField(
      "tint"_rkey,
      field{
          std::string{""},
          attr<DisplayName>("Tint"),
          attr<Description>("Optional \"#RRGGBBAA\" or \"#RRGGBB\" color multiplied over the video "
                            "(e.g. \"#FFFFFF80\" to fade it). Empty draws it unmodified."),
          attr<Category>("Appearance")});
  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Video"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Shows the current frame of a media player playing on the server. "
                        "Clicking it emits \"clicked\"."));
  dynamic::addClass(
      "wish"_key, std::move(proto), "Element"_key, dynamic::make_factory<ui_element>("wish"_key, "Video"_key));
}

} // namespace bdg::wish
