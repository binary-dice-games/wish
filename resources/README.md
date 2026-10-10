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
| `add.png` | add | `menu.png` | menu |
| `arrow_back.png` | arrow_back | `merge.png` | merge |
| `arrow_down.png` | arrow_downward | `more.png` | more_vert |
| `arrow_forward.png` | arrow_forward | `new_folder.png` | create_new_folder |
| `arrow_up.png` | arrow_upward | `notifications.png` | notifications |
| `bookmark.png` | bookmark | `open_in_new.png` | open_in_new |
| `bug.png` | bug_report | `package.png` | inventory_2 |
| `calendar.png` | calendar_month | `palette.png` | palette |
| `chart.png` | bar_chart | `paste.png` | content_paste |
| `chevron_left.png` | chevron_left | `pause.png` | pause |
| `chevron_right.png` | chevron_right | `person.png` | person |
| `cloud.png` | cloud | `play.png` | play_arrow |
| `commit.png` | commit | `print.png` | print |
| `copy.png` | content_copy | `redo.png` | redo |
| `cut.png` | content_cut | `refresh.png` | refresh |
| `dark_mode.png` | dark_mode | `remove.png` | remove |
| `database.png` | database | `save.png` | save |
| `download.png` | download | `schedule.png` | schedule |
| `drag.png` | drag_indicator | `search.png` | search |
| `edit.png` | edit | `send.png` | send |
| `expand_less.png` | expand_less | `server.png` | dns |
| `expand_more.png` | expand_more | `settings.png` | settings |
| `filter.png` | filter_alt | `share.png` | share |
| `fit_screen.png` | fit_screen | `sort.png` | sort |
| `folder_open.png` | folder_open | `star.png` | star |
| `fullscreen.png` | fullscreen | `stop.png` | stop |
| `fullscreen_exit.png` | fullscreen_exit | `sync.png` | sync |
| `help.png` | help | `table.png` | table |
| `history.png` | history | `tag.png` | label |
| `home.png` | home | `terminal.png` | terminal |
| `info.png` | info | `tree.png` | account_tree |
| `key.png` | key | `tune.png` | tune |
| `light_mode.png` | light_mode | `undo.png` | undo |
| `link.png` | link | `upload.png` | upload |
| `lock.png` | lock | `visibility.png` | visibility |
| `lock_open.png` | lock_open | `visibility_off.png` | visibility_off |
| `login.png` | login | `warning.png` | warning |
| `logout.png` | logout | `zip.png` | folder_zip |
| `mail.png` | mail | `zoom_in.png` | zoom_in |
| `memory.png` | memory | `zoom_out.png` | zoom_out |
