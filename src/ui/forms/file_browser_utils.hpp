// MIT License © 2025 Binary Dice Games
/// @file file_browser_utils.hpp
/// @brief Shared helpers for forms that render a file/directory listing
/// (FileDialog, FileExplorer): the type-icon lookup and the icon+label Name
/// cell it builds, the small `__wish_id` accessor both forms use when
/// caching widget pointers during on_init(), and the comparison primitives
/// both forms' click-to-sort-column handling is built on.
#pragma once

#include <ui/ui_element.hpp>

#include "src/bison/bison_object.hpp"

#include <string>

namespace bdg::wish {

/// @brief Reads back the `__wish_id` field stamped onto every imported
/// element during on_init() (see file_dialog.cpp/file_explorer.cpp), so it
/// can be cached alongside a widget pointer for on_event() dispatch.
template <typename Element>
bison::key_t wish_id_of(const Element& element) {
  return element->template as<bison::key_t>(bison::key_t{"__wish_id"});
}

/// @brief Maps a file/directory entry to the embedded icon (under
/// resources/embedded/icons/) shown to its left in a file listing's Name
/// column, mirroring the file-type icons in Windows' Open File dialog.
/// Falls back to the generic "file" icon for extensions not called out
/// below.
///
/// @param name  Entry's file/directory name (extension is read from this).
/// @param type  "dir" or "file".
/// @return  Icon basename, e.g. "folder", "image", "code" -- append to
///          "res/icons/" + result + ".png" for the Image widget's `src`.
std::string icon_for_entry(const std::string& name, const std::string& type);

/// @brief Builds the Name column cell used by both FileDialog and
/// FileExplorer's file tables: a HorizontalLayout containing a type icon
/// (via icon_for_entry()) followed by a Label showing @p display_name.
///
/// The icon deliberately does NOT set an explicit "width"/"height" -- see
/// render_image()'s "__auto_size_to_font__" handling (imgui_ui_renderer.cpp)
/// for why: it sizes itself to the current font's line height at render
/// time instead, both so the icon tracks --font_size automatically and so
/// it doesn't trip render_horizontal_layout()'s width-hint BeginChild
/// wrapping (which, for these row-generated icons with no per-row
/// __path__/__wish_id of their own, would collide every row onto the same
/// BeginChild ID).
///
/// @param name          Entry's file/directory name, passed to icon_for_entry().
/// @param type          "dir" or "file", passed to icon_for_entry().
/// @param display_name  Text shown in the Label (may differ from @p name,
///                      e.g. FileExplorer's "[dirname]" / ".. [Up]" framing).
ui_element_ptr make_name_cell(const std::string& name, const std::string& type, const std::string& display_name);

/// @brief Case-insensitive (ASCII) less-than, for sorting a file listing's
/// Name/Type/Modified columns the way a case-sensitive `operator<` on
/// std::string would get wrong (e.g. "Zeta" sorting before "apple").
bool ascii_ci_less(const std::string& a, const std::string& b);

/// @brief Approximates the byte count behind a human-formatted size string
/// (e.g. "12.3 KB", "1 GB", as produced by format_bytes() in
/// file_explorer.cpp), for numeric Size-column sorting.
///
/// Both the sandbox listing (server-formatted) and the local listing
/// (client-formatted, see update_local_listing()'s `size` field) only ever
/// carry the already-formatted display string -- not the raw byte count --
/// so an exact sort is not possible; this recovers enough precision (value
/// * unit multiplier) to order entries the way a user expects (all KB
/// before all MB, etc.), which plain lexicographic string comparison does
/// not ("1 GB" < "10 KB" alphabetically).
///
/// @return  Parsed byte estimate, or -1.0 for an empty/unparseable string
///          (e.g. a directory row's blank size) so such rows always sort
///          first in ascending order.
double parse_display_size(const std::string& text);

} // namespace bdg::wish
