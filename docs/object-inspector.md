# Object Inspector

`wish::object_inspector` (registered as `"ObjectInspector"` in the `"wish"`
bison namespace, `src/ui/ui_elements/object_inspector.{hpp,cpp}`) is a
reflection-driven property inspector, modeled on Unity's/Visual Studio's:
point it at a `bison::dynamic` object and it builds a two-column table (field
name | a type-appropriate editable widget) plus a description panel below
that shows the currently-selected row's field description — instead of an
app hand-rolling one `Label` + editor widget pair per field.

## Why `set_target()`, not just a `target` field

`ObjectInspector` is a real, RMI-instantiable `"wish"`-namespace class —
`instantiate("ObjectInspector", "wish", {...})` and nest it in a tree exactly
like `Button`/`Table`/any other element. But unlike a plain leaf widget
(whose whole behavior lives in its render function), building its table means
*creating new child elements* — something no render function may do: the
render loop only ever holds a session's **read** lock (see this repo's
`CLAUDE.md`, "Session threading model"); every other place in this codebase
that constructs child elements at runtime (`ui_template::instantiate_prototype()`,
`message_box::rebuild()`) does so under the **write** lock instead.

`ObjectInspector` follows that same precedent: its table is built by
`set_target()`, which only ever runs either as the `"__construct"`/
`"set_target"` RMI methods (both dispatched under the write lock
automatically) or from server-side C++ code that already holds
`context_wlock{*sync_ctx_}` explicitly (the same requirement
`stamp_widget()`-style direct-construction code elsewhere in this codebase
already follows). A plain `set({target: ...})` call deliberately does **not**
trigger a rebuild — call `set_target()` (or supply `target` in the
`instantiate()` construct params, which reaches the same code path via
`"__construct"`).

## Usage

**From a remote/RMI client** (only meaningful when `target`'s class is also
registered server-side — see "Reflection is process-local" below):

```cpp
dynamic params;
params["target"_key] = my_target_dynamic_ptr;
auto proxy = client.instantiate("wish"_key, "ObjectInspector"_key, std::move(params)).get();
// ... later, to point it at a different object:
dynamic retarget;
retarget["target"_key] = a_different_target;
proxy.call("set_target"_key, std::move(retarget)).get();
```

**From server-side C++** (the common case for an in-process/standalone app,
e.g. genie's editor) — construct directly and inject session context
yourself, mirroring `stamp_widget()`'s existing direct-construction
convention:

```cpp
auto elem = ui_element_ptr{dynamic::instantiate<object_inspector>("wish"_key, "ObjectInspector"_key)};
auto* oi = static_cast<object_inspector*>(elem.get());
oi->init(ctx(), sync_ctx_);                 // only needed for direct construction --
                                             // an RMI-instantiated instance gets this
                                             // automatically from on_create_object()
oi->set_target(sess(), target_dynamic_ptr); // must already hold the write lock
```

Route its events back through your own app: forward every event your owning
form receives to `handle_row_event()` (self-updates the description panel;
always safe to call, it's a no-op for events that aren't this instance's own
`Table`) and to `handle_changed()`/`handle_dropped()`, which return
`std::optional<field_edit>`/`std::optional<field_drop>` you can hand to your
own field-commit/asset-loading logic:

```cpp
void MyForm::on_event(key_t widget_id, key_t event_name, const dynamic& payload) {
  inspector_->handle_row_event(widget_id, event_name, payload);
  if (event_name == "changed"_key) {
    if (auto edit = inspector_->handle_changed(widget_id, payload))
      apply_edit(edit->field_name, edit->new_value);
  } else if (event_name == "dropped"_key) {
    if (auto drop = inspector_->handle_dropped(widget_id, payload))
      resolve_drop(drop->field_name, drop->payload);
  }
}
```

**Tearing down:** call `release(sess())` before dropping your own reference
to an instance you built directly (or before erasing an RMI-instantiated one
via `client.destroy()`) — this is *not* automatic on destruction. An
`ObjectInspector`'s own children can be destroyed as a direct side effect of
whole-session teardown (`ctx.objects.clear()`), and erasing further entries
from that same map while it's being cleared is undefined behavior — so,
like every other wish class that owns dynamically-created children, cleanup
is explicit, not destructor-driven. Skip the call when you know the whole
session is being torn down anyway.

## Field → widget dispatch

A field is skipped entirely if it carries a `Hidden` or `Obsolete` attribute
(see below), or is one of the reserved `CLASS`/`PARENT`/`NAMESPACE` keys. The
remaining visible fields are sorted ascending by `Order` where present
(untagged fields default to priority `0` and keep their declaration/hash
order relative to each other and to any explicit `Order` values around
them).

| Field type | Attribute | Widget |
|---|---|---|
| `bool` | — | `Checkbox` |
| `int32_t` | `Enum` | `Combo` (items = `Enum::entries()`; commits the selected entry's *name*, via `Enum`'s existing string coercion) |
| `int32_t` | `EnumFlags` | `InputText` (`"FlagA \| FlagB"` syntax, via `EnumFlags::format`/`parse`) |
| `int32_t` | `Range` | `SliderInt` (`min`/`max` from `Range`) |
| `int32_t` | — | `InputInt` |
| `float` | `Range` | `SliderFloat` |
| `float` | — | `InputFloat` |
| `std::string` | `Multiline` | `InputText` with `multiline=true` |
| `std::string` | — | `InputText` |
| `std::vector<float>` | `ColorField` | `ColorEdit` |
| `std::vector<float>` | — | `InputText`, comma-separated (component count preserved on commit) |
| `dynamic_ptr` | `DropTarget` | read-only reference `Button`, also a drop target (`handle_dropped()` surfaces the drop) |
| `dynamic_ptr` | — | read-only reference `Button` (not a drop target) |
| anything else (`key_t`/`hash_t`) | — | read-only `Label` |

A field needing a genuinely bespoke widget (its own populate/commit
contract that doesn't fit "one value in, one value out" — e.g. a curve
editor keyed by several attribute-supplied parameters) is out of scope for
generic dispatch: tag it `Hidden` and have your app append its own widget as
an extra table row after `ObjectInspector`'s own, exactly as it would have
built the whole row by hand before this widget existed.

## The new reflection attributes

Five new bison attributes (`src/bison/bison_object.hpp` in the `bison` repo)
back the dispatch table above — all advisory, UI-agnostic hints, same shape
as the pre-existing `Range`/`Step`/`Enum`/`EnumFlags`:

- **`Hidden`** — exclude a field from generic reflection-driven UIs.
- **`Order(int32_t priority)`** — explicit ascending sort priority.
- **`ColorField()`** — a `std::vector<float>` (size 3 or 4) is an RGB/RGBA color.
- **`Multiline(int32_t lines = 4)`** — a `std::string` wants a multi-line box.
- **`DropTarget(std::string drop_type)`** — a `dynamic_ptr` field accepts
  drag-and-drop of the given type (wish's existing generic `drag_type`/
  `drop_type`/`"dropped"` primitive).

## Reflection is process-local

Bison attributes (`DisplayName`, `Description`, `Range`, ...) are C++
metadata, never serialized over RMI (see `bison::attribute`'s doc comment).
`ObjectInspector` can therefore only produce a meaningful table when
`target`'s class is registered in the **same process** that renders it —
always true for an in-process/standalone app (e.g. genie), not guaranteed for
an arbitrary split wish client/server deployment unless the client's domain
classes are also registered server-side.

## See also

- [docs/ui-elements.md](ui-elements.md#objectinspector) — the widget
  catalog entry, alongside the new `ColorEdit` widget and `InputText`'s
  `multiline`/`height` fields.
- [DESIGN.md](../DESIGN.md) — `object_inspector`'s place among wish's other
  "Key Abstractions".
