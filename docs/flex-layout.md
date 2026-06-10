# Flex Layout — composing UI screens

CrossPoint screens are laid out with the **flex primitives** in
[src/util/Flex.h](../src/util/Flex.h): a header-only, stack-allocated, allocator-free layout
system. Use it for **all** new UI — page structure, grids, rows, and positioning intrinsically
sized content. Prefer it over hand-computed `x / y / width / height` arithmetic: the math is
declarative, deterministic, and reads like the layout it produces.

Worked examples to copy:
- `LibraryActivity::renderPasses` ([src/activities/home/LibraryActivity.cpp](../src/activities/home/LibraryActivity.cpp)) — page + side rail + tile grid.
- `CollectionsActivity::render` / `CollectionGroupActivity::render` ([src/activities/home/](../src/activities/home/)) — page + list.

---

## When to use it

- **Always** for a new activity's page layout (header / body / footer).
- For any container that splits a region into rows, columns, or a grid.
- For centering or anchoring fixed-size content (text, icons, widgets) inside a slot.

If you find yourself writing `const int contentTop = ... + ... + ...;` you almost certainly want
a `flex::Vstack` instead.

## Vocabulary

| Concept | API |
|---|---|
| Vertical stack | `flex::Vstack(parent, {sizes...}, gap=0, pad={})` |
| Horizontal stack | `flex::Hstack(parent, {sizes...}, gap=0, pad={})` |
| Uniform grid | `flex::Grid(parent, rows, cols, rowGap=0, colGap=0, pad={})` |
| Main-axis sizes | `flex::fixed(px)` · `flex::grow(weight=1)` · `flex::percent(pct)` |
| Padding | `flex::Padding{top,right,bottom,left}` · `flex::all(n)` · `flex::xy(x,y)` |
| In-slot placement | `flex::align(parent, w, h, hAlign, vAlign)` · `flex::center(parent, w, h)` |

Access children with `stack[i]` (or `grid.at(row, col)`). Containers are plain stack objects —
their lifetime is the enclosing C++ scope; read the child rects, draw, done.

Sizing is **main-axis only** (vertical for `Vstack`, horizontal for `Hstack`); the cross axis always
stretches to the inner span. Distribution order: `fixed` + `percent` + gaps are consumed first, then
the remainder is split across `grow` children by weight.

---

## Canonical patterns

### 1. Full-screen page (header / body / footer)

Source every size from theme metrics via `GUI.getData()` — never hardcode `480`/`800` — so the
layout is correct in all orientations. `ButtonHints::render` positions itself at the screen bottom,
so the footer slot only needs to *reserve* its height.

```cpp
const auto& td = *GUI.getData();
const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};

flex::Vstack page(screen,
                  {flex::fixed(td.header.height), flex::grow(), flex::fixed(td.buttonHints.height)},
                  /*gap=*/td.layout.verticalSpacing,
                  flex::Padding{static_cast<int16_t>(td.layout.topPadding), 0,
                                static_cast<int16_t>(td.layout.verticalSpacing), 0});

GUI.drawHeader(renderer, page[0], title);
GUI.drawList(renderer, page[1], /* … */);          // body
ButtonHints::render(renderer, b1, b2, b3, b4);     // self-positions; footer slot just reserves space
```

> Note: `flex::Padding` fields are `int16_t`. Constant sizing literals (`flex::fixed(40)`) are fine,
> but **runtime** `int` values used directly in a `Padding{...}` brace must be cast
> (`static_cast<int16_t>(td.layout.topPadding)`) to avoid a narrowing error.

### 2. Body split into content + side rail

```cpp
flex::Hstack body(page[1], {flex::grow(), flex::fixed(RAIL_WIDTH)}, /*gap=*/RAIL_GAP,
                  flex::xy(CONTENT_PAD_X, CONTENT_PAD_Y));
renderShelf(body[0]);
renderRail(body[1]);
```

### 3. Tile grid

```cpp
flex::Grid cells(shelfArea, /*rows=*/3, /*cols=*/3, /*rowGap=*/14, /*colGap=*/14);
for (int slot = 0; slot < 9; ++slot) renderTile(cells[slot], ...);
```

### 4. Centering intrinsic content in a slot

```cpp
const Rect at = flex::center(body, textW, textH);
renderer.drawText(font, at.x, at.y, msg);
// or anchor: flex::align(slot, w, h, flex::HAlign::End, flex::VAlign::Start)
```

---

## Padding guidance

- Give a **custom-drawn** body uniform inset with `flex::Padding` / `flex::all(n)` on its container.
- **`GUI.drawList()` already insets each row horizontally** by `td.layout.contentSidePadding`. Pass it
  the full body rect (e.g. `page[1]`) — do **not** also pad the body horizontally, or rows get
  double-indented.

## Constraints & characteristics

- `Vstack`/`Hstack` hold ≤ 16 children; `Grid` ≤ 16 cells (4×4). Exceeding asserts in debug builds.
- Integer math truncates; a few pixels may be left unspoken-for on the trailing edge (invisible — the
  cross axis covers the full span).
- ~280 B of stack per container, no heap, no per-frame allocation; results are byte-identical across
  runs and builds.
- Orientation safety: always build the root `Rect` from `renderer.getScreenWidth()/getScreenHeight()`
  and size from theme metrics (`td.layout.*`, `td.header.height`, `td.buttonHints.height`).
