Fonts from https://fonts.google.com/
Icons from https://fonts.google.com/icons

## Embedded icons

All icons in `embedded/icons/` are [Material Symbols](https://fonts.google.com/icons)
(Apache License 2.0), style **Outlined**, weight 400, fill 0, optical size 48,
rendered white on a transparent background at 64x64 (the `msgbox_*` and
`image` icons at 256x256). White icons can be tinted at render time (e.g.
`Tree.icon` tints to the text color). Every session gets them at
`res/icons/<file>.png`.

File-type and dialog icons: `file`, `folder`, `audio`, `image`, `code`,
`document`, `check`, `close`, `delete`, `error`, `msgbox_info`,
`msgbox_warning`, `msgbox_error`, `msgbox_question`.

To add more icons in the same style, rasterize
`https://fonts.gstatic.com/s/i/short-term/release/materialsymbolsoutlined/<name>/default/48px.svg`
with a white fill at 64x64.

General-purpose UI icons (toolbars, buttons, menus). Set them on a
`Button` / `MenuButton` / `MenuItem` / `TreeNode` `icon` field; server-side
module code can use `common::icon_path()` / `common::action_icon()` from
`modules/bdg/common/server/ui_helpers.hpp`.

| File | Material Symbols name | File | Material Symbols name |
|------|-----------------------|------|-----------------------|
| `add.png` | add | `open_in_new.png` | open_in_new |
| `arrow_back.png` | arrow_back | `package.png` | inventory_2 |
| `arrow_forward.png` | arrow_forward | `paste.png` | content_paste |
| `arrow_up.png` | arrow_upward | `pause.png` | pause |
| `bug.png` | bug_report | `person.png` | person |
| `chevron_right.png` | chevron_right | `play.png` | play_arrow |
| `cloud.png` | cloud | `redo.png` | redo |
| `copy.png` | content_copy | `refresh.png` | refresh |
| `cut.png` | content_cut | `remove.png` | remove |
| `database.png` | database | `save.png` | save |
| `download.png` | download | `search.png` | search |
| `edit.png` | edit | `server.png` | dns |
| `expand_more.png` | expand_more | `settings.png` | settings |
| `filter.png` | filter_alt | `sort.png` | sort |
| `folder_open.png` | folder_open | `star.png` | star |
| `help.png` | help | `stop.png` | stop |
| `history.png` | history | `terminal.png` | terminal |
| `home.png` | home | `tree.png` | account_tree |
| `info.png` | info | `undo.png` | undo |
| `link.png` | link | `upload.png` | upload |
| `lock.png` | lock | `visibility.png` | visibility |
| `memory.png` | memory | `warning.png` | warning |
| `menu.png` | menu | `zip.png` | folder_zip |
| `more.png` | more_vert | `zoom_in.png` | zoom_in |
| `new_folder.png` | create_new_folder | `zoom_out.png` | zoom_out |
| `commit.png` | commit | `send.png` | send |
| `fit_screen.png` | fit_screen | `table.png` | table |
| `merge.png` | merge | | |
