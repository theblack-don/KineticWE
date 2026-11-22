# Emerge Strip Layout for KineticWE 2.0 — Design & Porting Plan

Status: **Phase A + B implemented** (engine + `LayoutKind` + config/KCM wiring).
Phase C (input, tab-indicator rendering, persistence, settings UI) deferred — see § 11.

This document port-plans the niri-style **scrolling strip** from
[PlasmaZones](https://github.com/fuddlesworth/PlasmaZones) onto KineticWE's native
`LayoutEngine` system. It covers Phases **A** (model + engine) and **B** (verbs +
config) only; input (wheel-scroll gesture) is Phase C and deliberately deferred.

Scope decisions (confirmed):

1. **Per desktop AND per monitor.** Emerge is a `LayoutKind` selected per
   `(output, desktop)`, exactly like every other layout.
2. The kind is named **`Emerge`**.
3. Phases **A + B** now; Phase C (input) later.
4. This doc is the deeper porting/design write-up requested before any code.

---

## 1. What "scrolling" is

A strip is an **unbounded, ordered list of columns**. The screen (work area) is a
**viewport onto the strip** — it shows a window of the strip and scrolls along it.

The defining invariant (the whole reason it is interesting):

> **Inserting or removing a column NEVER resizes another column. Only the viewport
> moves.**

So opening a window slides the existing columns aside and scrolls the view; it does
not re-divide the screen like a tiling layout does. Each column may stack several
windows (Normal) or show one at a time with a tab indicator (Tabbed).

---

## 2. Source: how PlasmaZones implements it (what we port)

PlasmaZones is a **daemon + KWin effect + pure engine library**:

| Piece | File (in `/tmp/PlasmaZones`) | Role |
|---|---|---|
| Pure model | `libs/phosphor-scroll-engine/include/.../ScrollStrip.h` | Data structure + verbs |
| Data types | `.../ScrollTypes.h` | `Column`, `Tile`, `ColumnWidth`, `WindowHeight`, `ScrollLayoutParams` |
| Axis abstraction | `.../StripAxis.h` | main/cross (`x`/`y`) role remapping for horizontal/vertical |
| Relayout | `src/scrollstrip_relayout.cpp` | Resolves pixel rects from params + view offset |
| Sizing | `src/scrollstrip_sizing.cpp`, `_height.cpp` | Column extent, preset widths, tabbed/stack cross sizes |
| Structure | `src/scrollstrip_structure.cpp` | insert/remove/move/consume/expel verbs |
| Model | `src/scrollstrip_model.cpp` | Introspection/order |
| Daemon orchestration | `src/daemon/daemon/scrolling.cpp` | Config precedence, per-screen state, mode transitions |
| Settings keys | `src/config/.../scrolling.cpp` | Every `ScrollPerScreenKeys` and the schema |

**The key realization for porting:** PlasmaZones had to build a *daemon* + a *D-Bus
bridge* (`KWinCompositorBridge`) + a *KWin effect* because it is a third-party add-on
poking windows over IPC. KineticWE **is the compositor**. So we port the
*algorithm and data model*, not the process architecture — no daemon, no D-Bus, no
bridge. A `EmergeLayoutEngine` computes tile geometry in-process and the tile
system applies it natively.

**What we port wholesale:** the strip-viewport model, the `Column` / `Tile` intent
structures, width/height kinds (Proportion / Fixed / Preset), the
"insert/remove never resizes siblings" invariant, width-preset cycling,
consume/expel, tabbed display, and the main/cross axis abstraction.

**What we do NOT port:** the daemon, the DBus adaptors, the KWin effect's paint
pass (tab indicator *rendering*, overlays), and the effect's gesture handling.

---

## 3. KineticWE already has the scaffolding

`src/tiles/layoutengine.h` already declares the scrolling hooks — **no engine
implements them yet**:

```cpp
virtual bool supportsViewport() const { return false; }   // scrolling => true
virtual void setViewportOffset(qreal offset) { }          // the view slide
virtual qreal viewportOffset() const { return 0.0; }
// Emerge-specific column actions (no-op today):
virtual void focusColumnLeft() {}
virtual void focusColumnRight() {}
virtual void moveColumnLeft() {}
virtual void moveColumnRight() {}
virtual void switchPresetColumnWidth() {}
```

And `LayoutKind` is an open, append-only enum (`MasterStack=0 … Columns=5`), with
the rule *"append new kinds to the end and never reorder."* So `Emerge = 6` is the
intended drop-in slot.

### 3.1 Concept mapping

| PlasmaZones | KineticWE | Notes |
|---|---|---|
| `ScrollStrip` (model) | `EmergeLayoutEngine` (a `LayoutEngine`) | holds the columns + view state |
| `QVector<Column>` | `QList<Column>` | rename `Column`; names collide with the existing `ColumnsLayoutEngine::Column`, so use a distinct name (e.g. `ScrollColumn`) |
| `Column::tiles` | `QList<CustomTile *>` leaves + intent | we key intents by `KWin::Window *` |
| `Tile` (windowId, height intent, min size) | leaf + per-`Window` intent map | keep `WindowId` → intent in a side map or on the tile |
| `ScrollLayoutParams` | engine member / passed to `reflow()` | go geometry + gap + axis + presets |
| `StripAxis` (main/cross) | `Qt::Orientation` | or a tiny helper; port the main/cross role mapping |
| `viewOffset` + active-relative `viewAnchor` | `qreal m_viewOffset` + active column | **the subtle part** (§ 5) |
| `supportsViewport/setViewportOffset` | same hooks (already exist) | the engine flips these to true |
| daemon config precedence | TilingController + `kineticwe.kwe [Tiling]` | per-screen/per-context (§ 6) |

### 3.2 Geometry path (implemented)

The tile system is **bounded to the output by default**: `CustomTile::setRelativeGeometry`
(`src/tiles/customtile.cpp`) clamps to `[0,1]` and refuses out-of-bounds geometry, and
`Tile::windowGeometry()` clips to the output. An unbounded strip needs columns that sit
beyond the output edge, so the engine opts those tiles out through a **default-off** flag:

- `Tile::allowOffscreen()` / `setAllowOffscreen(bool)` (`src/tiles/tile.h`, `tile.cpp`),
  default `false`.
- `CustomTile::setRelativeGeometry` skips the `[0,1]` clamp, the out-of-bounds refusal
  and the on-screen adjacency block when the flag is set.
- `Tile::windowGeometry()` skips the `intersected(output)` clip when the flag is set,
  so an off-view window keeps its true (off-screen) rect instead of collapsing to empty.

The `EmergeLayoutEngine` sets `leaf->setAllowOffscreen(true)` on every leaf it
creates. Every other layout leaves the flag clear and keeps the historical behaviour
byte-for-byte.

**Off-view tiles are parked, not laid out beyond the edge.** Output-relative
coordinates past `[0,1]` are still *global* KWin coordinates, so a column at
`x > 1` on one monitor lands on the next monitor and KWin draws it there (there is
no per-output clipping, and the tiling controller has no output-change migration
hook). So `reflow()` distinguishes three cases per tile:

- **fully off-view** → `parkTile()` moves it to `unionBottom + 1 + 16`, a region
  below *every* output that belongs to no monitor (ported from PlasmaZones'
  `kParkMargin`). This is topology-agnostic — no neighbour can draw it on any
  monitor layout, so the parked state never bleeds onto another screen;
- **straddling** → the main extent is clamped to the visible part so the overhang
  cannot spill onto an adjacent output;
- **fully visible** → normal relative geometry.

The trade-off is animation direction: because the tile is parked below the
outputs, the `geometrychange` effect (which animates from the previous geometry)
slides it up from the bottom when it returns, rather than in from the side. A
side-park along the strip axis gives the nicer sideways slide but travels across
a neighbouring monitor during the animation on multi-monitor layouts, so the
below-union park is kept as the topology-safe default.

This is why `allowOffscreen` is still needed: the parked geometry's relative `y`
is well above `1`.

Note: `supportsResizeGravity` / screen-edge anchor tests still compare
`relativeGeometry() > 0` / `< 1`. For parked tiles those answers are mathematically
"wrong", but parked tiles are not interactively resizable, so it is harmless.

---

## 4. Phase A — data model + engine

### 4.1 New files

- `src/tiles/emergelayoutengine.h`
- `src/tiles/emergelayoutengine.cpp`

Register in `src/CMakeLists.txt` near the other tile engines (line ~220 block, e.g.
after `tiles/columnslayoutengine.cpp`).

### 4.2 Data model (port from `ScrollTypes.h` / `ScrollStrip.h`)

```cpp
namespace KWin {

enum class ScrollWidthKind { Proportion, Fixed, Preset };   // ColumnWidth::Kind
enum class ScrollHeightKind { Auto, Fixed, Preset };        // WindowHeight::Kind
enum class ScrollColumnDisplay { Normal, Tabbed };          // ColumnDisplay
enum class ScrollCenterFocused { Never, Always, OnOverflow };// CenterFocusedColumn

struct ScrollColumnWidth {
    ScrollWidthKind kind = ScrollWidthKind::Proportion;
    qreal proportion = 0.5;
    int fixedPx = 0;
    qreal presetFraction = 0.5;
};

struct ScrollWindowHeight {
    ScrollHeightKind kind = ScrollHeightKind::Auto;
    qreal weight = 1.0;
    int fixedPx = 0;
    qreal presetFraction = 0.5;
};

// One window tile in a column (an "intent" record; the actual leaf is the
// CustomTile the engine creates under the RootTile).
struct ScrollTileIntent {
    ScrollWindowHeight height;
    int minWidth = 0;
    int minHeight = 0;
};

// One column: a stack of tiles ACROSS the strip.
struct ScrollColumn {
    QList<ScrollTileIntent> tiles;   // in stack order
    int activeTileIdx = 0;
    ScrollColumnWidth width;
    ScrollColumnDisplay display = ScrollColumnDisplay::Normal;
    // Which tab owns the column's cross extent while Tabbed (id-based, so it
    // survives insert/remove/reorder):
    QString heightOwnerId;
    bool maximizedToEdges = false;
    // leaf pointer for the active window (owned by the engine, not the struct):
    // the engine maps windowId -> CustomTile* externally.
};

// Resolved output of one layout pass (mirrors ScrollStrip::ResolvedStrip).
struct ResolvedLayout {
    qreal viewOffset = 0.0;
    QList<Window *> order;           // windows in strip order
};
}
```

Column naming note: `ColumnsLayoutEngine` already has a `struct Column`; use
`ScrollColumn` to avoid a clash.

### 4.3 Engine class surface

```cpp
class EmergeLayoutEngine : public LayoutEngine {
    Q_OBJECT
public:
    LayoutKind layoutKind() const override { return LayoutKind::Emerge; }
    void attach(RootTile *root) override;

    // Window lifecycle.
    void addWindow(Window *window) override;
    void removeWindow(Window *window) override;
    void moveWindow(Window *window, int delta) override;

    // Emerge hooks (already in the base).
    bool supportsViewport() const override { return true; }
    void setViewportOffset(qreal offset) override;   // pan by pixel offset
    qreal viewportOffset() const override;

    // Emerge verbs (already in the base).
    void focusColumnLeft() override;
    void focusColumnRight() override;
    void moveColumnLeft() override;
    void moveColumnRight() override;
    void switchPresetColumnWidth() override;

    // Column ingest/expel inherited via consumeWindow/expelWindow (already in base).
    void consumeWindow(Window *window) override;
    void expelWindow(Window *window) override;

    void reflow() override;
    void pruneEmptyLeaves() override;
    QList<Window *> windows() const override;
    Window *primaryWindow() const override;
    Window *windowInDirection(Window *from, FocusDirection direction) const override;

    bool supportsPerTileResize() const override { return true; }
    void adjustTileSize(Window *window, qreal weightDelta, Qt::Orientation axis) override;
    void adjustColumnRatio(qreal delta) override;

private:
    // Strip model.
    QList<ScrollColumn> m_columns;
    int m_activeColumnIdx = -1;
    QPointer<CustomTile> m_root;
    // Window <-> tile mappings (the engine owns leaf creation).
    QHash<Window *, int> m_windowColumn;      // window -> column index
    QHash<Window *, QPointer<CustomTile>> m_windowTile; // window -> leaf
    // Config / params.
    Qt::Orientation m_axis = Qt::Horizontal;
    qreal m_gap = 0.0;
    QList<qreal> m_presetColumnWidths;   // {1/3, .5, 2/3, .75, 1}
    QList<qreal> m_presetWindowHeights;
    ScrollCenterFocused m_centerFocused = ScrollCenterFocused::Never;
    bool m_respectMinimumSize = true;
    bool m_centerShortColumns = false;
};
```

The view state (`m_viewOffset` used by `viewportOffset()`) is stored **relative to
the active column** as an anchor (§ 5), not as an absolute strip offset.

### 4.4 Geometry model (the coordinate system)

We keep the strip in **pixel coordinates** internally, then convert to **relative
`[0,1]`** when calling `setRelativeGeometry` (or `setGeometryFromAbsolute`). This
keeps the PlasmaZones pixel math intact and lets the existing tile machinery move
windows.

Strip coordinates (role-based, from `StripAxis.h`):
- **main** axis = along the strip (columns laid out left→right if horizontal, top→bottom if vertical).
- **cross** axis = within a column (vertical stack if horizontal, horizontal stack if vertical).

A helper `ScrollAxis` mirrors `StripAxis`:

```cpp
struct ScrollAxis {
    Qt::Orientation o = Qt::Horizontal;
    int mainPos(const QRectF &r) const;   // x for H, y for V
    int crossPos(const QRectF &r) const;
    int mainSize(const QSizeF &s) const;
    int crossSize(const QSizeF &s) const;
    QRectF makeRect(main, cross, mainExtent, crossExtent) const;
};
```

### 4.5 `reflow()` (the heart — port from `scrollstrip_relayout.cpp`)

Follows PlasmaZones `ScrollStrip::relayout`:

1. Resolve params: `workArea` (output work rect), `gap`, `axis`, presets.
2. `viewOffset = columnStripPos(active) - anchor` (no active column ⇒ 0).
3. `stripStart = axis.mainLow(workArea) - viewOffset`; `mainCursor = stripStart`.
4. For each column in order:
   - `colW = columnExtentPx(col)` — resolve width (Proportion/Fixed/Preset), apply
     min-extent floor and a work-area cap; **maximized-to-edges ⇒ raw work area.**
   - Skip fully-minimized / empty columns (they occupy no main extent).
   - Resolve the column rect:
     - Normal: spans the full **cross** extent (minus tab-indicator reservation).
     - Tabbed: cross extent = owned tab's height intent (one tab decides, so a tab
       switch doesn't resize the column), centered per `centerShortColumns`.
   - Lay out tiles across the cross axis: Fixed/Preset take their px; Auto (weighted)
     share the remainder. For a tabbed column only the active tile is laid out; the
     rest share its hidden rect.
   - Straddler handling: if `cropStraddlers` is false, clamp on-screen columns to the
     work area; if true, keep their true rect (the compositor clips them).
5. Convert each leaf's absolute pixel rect to relative geometry and
   `setRelativeGeometry` it.

The **insert/remove invariant** is enforced by the verbs (§ below): inserting a
column only changes that column's position *relative to the view*; existing sibling
widths are untouched.

### 4.6 The anchor/model math (the subtle part)

Port these from `ScrollStrip` (relayout/sizing):

- `columnStripPos(i)` — cumulative main position of column `i` (sum of preceding
  extents + gaps).
- `stripExtentPx(params)` — total strip length.
- `viewOffsetFor(params) = columnStripPos(active) - m_viewAnchor`.
- **`m_viewAnchor` is stored relative to the active column.** This is what makes a
  window opened *before* the focused column not drift: structural changes shift the
  anchor with the column, so the derived `viewOffset` is unchanged.
- Focus/centering policies (port `focusAnchorFor` + `centeredAnchorFor` +
  `clampedAnchorFor`):
  - **Never** — scroll the minimum so the focused column is fully visible, pinning
    to the edge it entered from.
  - **Always** — center the focused column.
  - **OnOverflow** — center only when focused + previous can't both fit.
- **View detachment:** `scrollViewBy` (a pan) sets a latch so the centering policy
  doesn't immediately undo it; a focus change or a centering verb clears it and takes
  the view back.

This anchor/detach/counter logic is the most error-prone part of the whole port. It
should be the first thing unit-tested (see § 7).

### 4.7 Verbs (Phase A minimum)

- `addWindow`: insert a new single-tile column. Default position: immediately after
  the active column (or index 0 on empty). Respect `InsertPosition` (LeftOfActive /
  RightOfActive / ActiveColumn). Focus it. Leaves sibling widths untouched.
- `removeWindow`: remove the tile; if the column empties, close it and reflow. Keep
  the view anchored so survivors don't jump; select a sensible new focus.
- `moveWindow(window, delta)` / `moveColumnLeft/Right`: reorder the column, reanchor.
- `consumeWindow` / `expelWindow`: move a window into / out of the neighbor column.
- `reflow()`: § 4.5.

---

## 5. Phase B — integration, config, verbs

### 5.1 Add the `LayoutKind`

1. `src/tiles/layoutengine.h`: add `Emerge = 6` to `LayoutKind`
   (append, never reorder).
2. `src/tiles/layoutengine.cpp`:
   - `layoutKindToString`: `case Emerge: return "Emerge";`
   - `layoutDisplayName`: `case Emerge: return "Emerge";`
   - `layoutKindFromString`: `if (name.compare("Emerge", Ct::CaseInsensitive) == 0) return Emerge;`
3. `src/tiling/tilingcontroller.cpp`, `createLayoutEngine`:
   `case Emerge: return std::make_unique<EmergeLayoutEngine>(parent);`

### 5.2 Config surface (Phase B)

Add per-layout config to the `[Tiling]` group (read by `TilingController` and pushed
into the engine). Proposed keys (names are a suggestion — keep them `Emerge*`):

| Key | Type | Default | Meaning |
|---|---|---|---|
| `EmergePresetColumnWidths` | `StringList` | `1/3,1/2,2/3,3/4,1` (or decimals) | preset width vocabulary |
| `EmergePresetWindowHeights` | `StringList` | same | preset height vocabulary |
| `EmergeAppWidths` | `StringList` | empty | per-app column width, `windowClass=fraction` (e.g. `firefox=0.333`); exact, case-insensitive class match |
| `EmergeTabbed` | `bool` | `false` | default display mode |
| `EmergeFillGaps` / `EmergeGap` | `int` | existing tiling gap | inner gap |

These map onto `ColumnsMaxColumns`-style plumbing in `TilingController` (it already
pushes `m_columnsMaxColumns` to every engine via `setMaxColumns`).

### 5.3 Per-desktop + per-monitor

Emerge is a `LayoutKind` selected per `(LogicalOutput, VirtualDesktop)`, so it is
per-desktop and per-monitor **by construction** — the same `TileManager` /
`RootTile` mechanism every other layout uses. Confirmed in `tilingcontroller.cpp`:

```cpp
for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
    const LayoutEngine::LayoutKind kind = resolveLayoutKind(output, desktop); // <-- per output+desktop
    setupLayoutEngine(manager, desktop, kind);
}
```

**Per-screen behavioral config (axis / presets) needs a decision.** PlasmaZones
resolves axis + presets per screen via a global config + per-screen override map +
per-context rules. For KineticWE, the minimal Phase-B approach is:

1. **Axis:** a single global default in `[Tiling]`, resolved in `reflow()` from the
   output's aspect ratio (auto) unless overridden. A per-`(output,desktop)` override
   can be added later through the (currently single-value) per-screen config group
   in `kineticwe.kwe`.
2. **Presets:** a single global vocabulary for now; per-monitor override deferred.

> **Open question (§ 8):** does the tiling config already support
> per-`(output,desktop)` values today (like the per-desktop layout kind), or would
> per-monitor axis/presets need a new per-screen keyed group in `kineticwe.kwe`?

### 5.4 KCM / settings

- `src/kcms/tiling/tilingsettings.kcfg`: add the `Emerge*` entries so the tiling
  KCM exposes them.
- Add a scrolling section to the tiling settings UI (Phase B UI work).
- `EnabledLayouts` default list: include `Emerge` in the shipped default or leave
  it opt-in. Confirm which the user wants (§ 8).

### 5.5 Verbs for Phase B (the ones already declared in the base)

These are the "no-op for non-scrolling" hooks the base already declares — implement
them here:

- `focusColumnLeft/Right` — focus the adjacent column; re-anchor per the centering policy.
- `moveColumnLeft/Right` — reorder the active column with its neighbor; re-anchor.
- `switchPresetColumnWidth` — cycle the active column's width to the next preset
  (niri `toggle_width` semantics: step strictly wider/taller, wrap at the ends).
- `adjustTileSize(window, weightDelta, axis)` — per-tile resize: delta on the cross
  (stack) weight for a Normal column, or the owned height for a Tabbed column; delta
  on the main axis adjusts the column width toward a neighbor. Port the
  push/pull + screen-edge no-op logic from the columns engine.
- `adjustColumnRatio(delta)` — width/ratio of the active column.
- `interactiveResizeEnded` — reuse the default `LayoutEngine` implementation (it
  converts drag delta to `adjustTileSize`).

---

## 6. Risks & how to de-risk them

1. **Off-screen columns.** Handled by the gated `allowOffscreen` flag (§ 3.2) plus
   the **park-below-the-union** placement: an off-view window is moved to a region
   below every output so it cannot render on a neighbouring monitor. Straddlers are
   clamped to the visible part. Concern: moving windows on every relayout. Mitigation:
   the tile early-returns when `m_relativeGeometry == constrainedGeom`, and the park
   rect is stable (not view-dependent), so a parked window is not re-committed as the
   view scrolls. Keep this as a performance test.

2. **Anchor math drift.** Port `focusAnchorFor` / `centeredAnchorFor` /
   `clampedAnchorFor` / view-detach faithfully and unit-test them first (§ 7).

3. **Screen-edge detection.** `supportsResizeGravity` / `Tile::anchors` assume
   `[0,1]`. Tiles partially off-screen will report odd anchors during interactive
   resize. Since off-screen tiles aren't resized interactively, this is acceptable;
   document it.

4. **Tab indicator.** Phase A/B can ship **without** painting the tab indicator (a
   Tabbed column just shows the active window). Rendering the indicator is the KWin
   effect's job in PlasmaZones; in KineticWE this is a compositor-side paint concern
   that should land **after** the core layout works. Defer rendering; keep the
   geometry reservation logic.

5. **Crop straddlers.** Clamping vs. cropping a column that straddles the output edge
   is a behavior distinction. Phase A: clamp (simpler, matches a "window clipped by
   the output" look). `cropStraddlers = true` is a later refinement.

6. **Persistence.** PlasmaZones persists strip state per context. KineticWE's other
   layouts rebuild from the window set on attach — no state to persist for a simple
   first cut (width intents reset to defaults on re-attach). Defer real persistence.

---

## 7. Testing & static verification

- **Unit tests (autotests/):** the anchor/centering math is the highest-risk code and
  is testable without a running compositor. Add tests mirroring PlasmaZones'
  `scrollstriptestutils` / `test_scrollstrip_core`:
  - insert before active doesn't shift the focused column's view;
  - insert/remove never resizes a sibling;
  - preset cycling steps strictly wider/taller and wraps;
  - `Never`/`Always`/`OnOverflow` centering;
  - consume/expel; tabbed extent owned by one tab.
- **Static checks I can run during development:** `clang-format --dry-run -Werror`,
  `shellcheck`/`bash -n` (not relevant here), and a **configure-only** `cmake -S . -B
  <fresh-tmp> -G Ninja` to catch CMake/registration mistakes. **No compile, no build.**
- **You build/run:** `./build-rpm-dev.sh --kwin-only` (or `--binary` / `--full`) and
  exercise the layout manually.

---

## 8. Resolved decisions

1. **Per-monitor axis & presets:** *only which layout* is per `(output, desktop)` for
   now (already free). Axis + presets are **single global** `[Tiling]` config — no
   per-screen behavior group in this phase.
2. **Shipped default:** `Emerge` **is included** in the default `EnabledLayouts`
   list (opt-out).
3. **InsertPosition default:** right of active (niri) — confirmed.
4. **Tabbed indicator rendering:** deferred to a later phase (Phase C+). Phase A/B
   ship a Tabbed column showing the active window only; the geometry reservation is
   kept.
5. **`cropStraddlers`:** Phase A **clamps** straddling columns to the output; revisit
   after testing.

---

## 9. Suggested implementation order (Phase A, then B)

A1. Add `ScrollAxis` + data structs (`scroll_types.h`-like, can live in
    `emergelayoutengine.h`).
A2. Add `EmergeLayoutEngine` skeleton, wire it into factory + `LayoutKind` +
    CMake.
A3. Port `reflow()` (column extent → pixel rect → relative geometry) and the
    insert/remove verbs. Hand-test a static strip first.
A4. Port the anchor/centering math; unit-test it.
A5. B1: config keys + KCM + per-desktop/per-output selection; `EnabledLayouts`.
A6. B2: the base-declared scrolling verbs (focus/move column, preset cycling,
    per-tile resize).

Phase C (deferred): wheel-scroll/`Meta+wheel` input, tab-indicator rendering,
persistence.

---

## 10. Reference: exact PlasmaZones sources to port from

| What | File to read |
|---|---|
| Model + verbs | `libs/phosphor-scroll-engine/include/PhosphorScrollEngine/ScrollStrip.h` |
| Types (`Column`,`Tile`,`ColumnWidth`,`WindowHeight`,`ScrollLayoutParams`) | `.../ScrollTypes.h` |
| Axis roles | `.../StripAxis.h` |
| Relayout / geometry | `src/scrollstrip_relayout.cpp` |
| Column/tabbed sizing | `src/scrollstrip_sizing.cpp`, `src/scrollstrip_sizing_height.cpp` |
| Insert/remove/consume/expel | `src/scrollstrip_structure.cpp` |
| Anchor/centering math | `scrollstrip_relayout.cpp` (`focusAnchorFor`, `centeredAnchorFor`, `clampedAnchorFor`, `scrollViewBy`, `updateViewForFocus`) |
| Config keys/schema | `src/config/settings/scrolling.cpp` |

*(PlasmaZones repo is at `/tmp/PlasmaZones` if you want to inspect it locally.)*

---

## 11. Implementation status (Phase A + B)

Implemented:

- **`src/tiles/emergelayoutengine.h` / `.cpp`** — the strip model
  (`EmergeColumn` / `EmergeTile` / width & height intents / display mode),
  the pixel-free relative-coordinate layout, the active-relative view anchor with
  `Never` / `Always` / `OnOverflow` centering, and the verbs: add/remove/move,
  consume/expel, focus column, move column, preset cycling, per-tile resize,
  viewport offset.
- **`src/tiles/tile.h` / `.cpp`** and **`src/tiles/customtile.cpp`** — the gated
  `Tile::allowOffscreen` flag (§ 3.2), default off; every other layout unchanged.
- **Multi-monitor parking** — off-view tiles are moved below the union of all
  outputs, straddlers clamped to the edge (§ 3.2), so a column scrolled off one
  monitor cannot appear on the next.
- **`src/tiles/layoutengine.h` / `.cpp`** — `LayoutKind::Emerge = 6` plus the
  string / display-name / from-string cases.
- **`src/tiling/tilingcontroller.{h,cpp}`** — factory case, `[Tiling]` config read,
  `configureEmergeEngine()` / `applyEmergeSettingsToEngines()`, `Emerge`
  added to the default `EnabledLayouts`, and focus-follows-view wired onto
  `Workspace::windowActivated` (via `EmergeLayoutEngine::focusWindow`).
- **`src/CMakeLists.txt`** — source registered.
- **`src/kcms/tiling/tilingsettings.kcfg`** — `Emerge*` entries + updated
  `EnabledLayouts` / `DefaultLayout` docs.
- **`src/kcms/tiling/ui/main.qml`** — `Emerge` checkbox, default-layout entry,
  and both layout whitelists.
- **`src/tiling/tilingdbusinterface.h`** — doc comment updated.

Config keys (global; `[Tiling]` group of `kineticwe.kwe`):

| Key | Type | Default |
|---|---|---|
| `EmergeAxis` | int (0 auto / 1 horizontal / 2 vertical) | 0 |
| `EmergePresetColumnWidths` | StringList | 0.333,0.5,0.667,0.75,1 |
| `EmergePresetWindowHeights` | StringList | 0.333,0.5,0.667,0.75,1 |
| `EmergePeek` | Bool | false |
| `EmergePeekHeight` | int (px of the bottom band) | 48 |
| `EmergeAppWidths` | StringList (`windowClass=fraction`) | empty |

Deferred (Phase C+, needs your go-ahead):

- **Settings UI** for the `Emerge*` keys (they're in the kcfg but have no widgets
  on the tiling page yet).
- **Tab-indicator rendering** (Tabbed columns show the active window; the visual
  **indicator strip** beside it is not painted).
- **Strip persistence** (order/focus not saved across restarts).
- **`cropStraddlers` / `respectMinimumSize`**: minimum size is only a soft floor
  today; straddlers are clamped by the compositor's own clipping.

## 12. Revision history

**Revision 21 (this session):**
- **Per-app predefined column widths.** New `[Tiling]` `EmergeAppWidths`
  StringList of `windowClass=fraction` entries (e.g. `firefox=0.333`), edited
  on the Kinetic Settings Tiling page with the existing interactive window
  detect. A matched window opens its column at the configured fraction of the
  work area (0.333 / 0.5 / 0.667 / 1 on the page; hand-edited values keep a
  custom option). Matching is exact and case-insensitive against
  `Window::resourceClass()`; `=` in a class name is tolerated because the entry
  splits on the last `=`, and the last duplicate wins.
- **The width survives normalization.** `EmergeColumnWidth` gained `pinned`
  (explicit width normalisation must not rescale) and `ruleOwned` (the width is
  still the app rule's, which is what lets config edits live-apply).
  `normalizeWidthsToFill()` leaves pinned columns alone and scales only the
  unpinned remainder; when every column is pinned (or no space is left) the gap
  stays, intentionally.
- **Live reapply, never over a manual resize.** Pushing the rules again
  (`setEmergeWindowWidthRules()`) updates columns that still report
  `ruleOwned`, resets columns whose rule was removed, and adopts rules for
  already-open windows that never had a manual width. Preset cycling, ratio
  keys, resize keys and edge drags all clear `ruleOwned` (while preserving
  `pinned`), after which config edits leave that column alone.
- **Late WM_CLASS.** `TilingController::onWindowClassChanged()` now refreshes
  an Emerge column's rule before the ignore early-out, so a window that
  arrived classless still gets its configured width.
- **Lone pinned columns can be dragged.** A lone ordinary column still fills
  the viewport and its edge drag remains a no-op, but a lone app-rule column
  accepts the drag delta (`MinColumnExtent..1.0`) and takes manual ownership.

**Revision 20 (this session):**
- **Peeked windows no longer steal focus on hover.** `Tile` gained a
  default-off `peeked()` / `setPeeked()` flag (mirroring `allowOffscreen`), set
  by `EmergeLayoutEngine` on the leaves it lays out in the bottom peek band
  (`peekColumn()` sets it true; `setTileRect()` and `parkTile()` clear it), so a
  window that scrolls into view stops being "peeked" on the next reflow.
  `Window::pointerEnterEvent()` now returns early for a window whose tile is
  peeked, so `FocusFollowsMouse` / `FocusUnderMouse` do not pull focus (and do
  not autoraise) when the pointer merely passes over the band. Click-to-focus is
  a separate path (`Workspace::activateWindow` / `Window::takeFocus`), so a
  click still focuses the peeked window and the existing focus-follows-view
  scrolls it into view.

**Revision 19 (this session):**
- **Bottom peek band for off-view columns.** New `EmergePeek` (Bool, default
  **false**) and `EmergePeekHeight` (int px, default 48) config keys, pushed to
  every live engine via `configureEmergeEngine()` and exposed on the Kinetic
  Settings Emerge page (toggle + slider). When enabled and at least one column is
  scrolled fully off the strip, `reflow()` carves a band off the bottom of the
  work area: the visible columns shrink to `1 - peek`, and up to two off-view
  columns (the nearest to the viewport, `MaxPeekedColumns`) are laid out in the
  band in strip order, each in an equal-width slot (`peekColumn()`), so a left
  column stays left and a right column stays right and the existing half-gaps
  separate the slots. Columns further down the strip stay parked, so the band
  cannot crowd into unreadable slivers. The peeked column keeps its full
  cross layout but is shifted down, showing only its top `peek` (no squash). The
  band is suppressed (falling back to `parkTile()`) when another output sits
  directly below — `outputHasMonitorBelow()` checks for an output that overlaps
  the strip's horizontal span and starts at/below its bottom edge.

**Revision 18 (this session):**
- **Drops now land where released — in every layout, on every output.** A
  cross-output/desktop drag used to always append the window to the end of the
  destination layout (`addWindowToLayout()` → `addWindow()`), and only
  Columns/Emerge honoured a drop zone on the same output. Added
  `LayoutEngine::insertWindow(window, target, zone)` with implementations in all
  seven engines; the base `endMoveWindowOnZone()` now tries it first and falls
  back to `endMoveWindow()`. This applies on the same output too, so every
  layout now supports position-aware drops.
- **Directional drop zones.** `DropZone` gained `InsertLeft` / `InsertRight`, and
  `TilingController::dropZoneForTarget()` now uses the cursor's nearest edge
  (2D, with a central dead-zone for `Swap`) instead of Y-only top/bottom. A
  horizontal strip therefore reads left/right naturally.
- **Per-layout meaning.** Order-based layouts (Monocle, AutoGrid, Stacked,
  MasterStack, Center Tile) read Left/Above as "before the target" and
  Right/Below as "after", shifting the other windows. Columns/Emerge keep
  stacking into the target's column for Above/Below and add a new adjacent
  column for Left/Right.
- The cross-output/desktop branch resolves the drop target on the destination
  engine *before* adopting the window (while it is detached it cannot shadow the
  target), then calls `insertWindow()` after `addWindowToLayout()`.

**Revision 17 (this session):**
- **User-set column widths now persist across window open/close and focus
  changes.** `rebalanceColumnWidths()` unconditionally reset every column to the
  default whenever a window was added or removed, so opening a new window
  clobbered a width the user had cycled/dragged (e.g. a full-width Marvel Rivals
  snapped back to `0.5`). `EmergeColumnWidth` gained a `userSet` flag, set by the
  explicit paths — preset cycle, `adjustColumnRatio()`, the main-axis
  `adjustTileSize()`, and `interactiveResizeEnded()` (both dragged column and the
  neighbour it trades width with). `rebalanceColumnWidths()` now only re-defaults
  columns that were never touched, and `normalizeWidthsToFill()` preserves the
  flag across its rescale. A lone column is never flagged, since it always fills
  the viewport (cycling it is a visual no-op); this keeps the first-column-full /
  subsequent-`0.5` default when the second window opens. (Exception, Revision 21:
  a lone column pinned by an app width rule keeps its explicit width and is
  flagged.) This restores the
  documented invariant ("inserting or removing a column NEVER resizes another
  column"), which `rebalanceColumnWidths()` had been violating.

**Revision 16 (this session):**
- **Expelled columns are `0.5` wide.** `expelWindow()` created its new
  `EmergeColumn` with the struct default (`proportion = 1.0`), so the expelled
  window's column came out full-width and dwarfed its neighbours. It now uses
  the shared `DefaultColumnExtent = 0.5`, matching `rebalanceColumnWidths()`'s
  multi-column default. (A lone full-width source column is left at `1.0`; only
  the new column is normalised.)

**Revision 15 (this session):**
- **Drop-into-column + working consume/expel.** `EmergeLayoutEngine` now
  overrides `endMoveWindowOnZone()`: dropping a dragged window on the top or
  bottom quarter of a target inserts it into the target's column above/below
  (same-column drops reorder), while the middle keeps the existing swap. The
  inserted `EmergeTile` keeps its leaf and height intent, the source column is
  closed if it empties, and the detached dragged window is re-adopted into its
  new slot. `beginMoveWindow()` now records the source column/tile alongside the
  leaf (like `ColumnsLayoutEngine`) so the drop can locate the source tile
  without searching.
- **Consume/expel follow the view.** Both verbs moved the window structurally
  but left `m_activeColumnIdx`/`m_viewAnchor` untouched, so the consumed/expelled
  window could land off-screen while the view stayed put (and, because the
  focused window did not change, `focusWindow()` was never called to scroll to
  it) — looking like a no-op or a vanishing window. Both now set the target/new
  column active, update its `activeTileIdx`, and re-anchor via
  `focusAnchorFor()` so the view follows the moved window. Expel remains
  uncapped (no `maxColumns` limit in Emerge).

**Phase A + B (initial):**
- Engine, `LayoutKind`, config wiring, KCM, off-screen parking below union of outputs.

**Revision 1 (earlier this session):**
- **Multi-monitor parking**: off-view tiles moved below the union of all outputs
  (§ 3.2); straddlers clamped to the visible part (no neighbor bleed).
- **First window full width**: `rebalanceColumnWidths()` — one column at `1.0`,
  two at `0.5` each, three or more at `0.5` each so the strip scrolls.
- **Swap verb**: `moveWindow(window, delta)` swaps the window `delta` positions
  away in strip order (same column = tile swap; different column = column swap),
  making the `Meta+Shift+Arrow` swap binds work.
- **Shift+Meta+scroll** panned the strip (avoids the VDM's `Meta+scroll`);
  replaced by focus stepping in Revision 15.

**Revision 15 (this session):**
- **Shift+Meta+scroll now steps column focus.** It used to pan the viewport by
  one column width plus a fixed 5% offset while leaving `m_activeColumnIdx`
  untouched, so the strip stopped mid-column (windows clipped at the viewport
  edge, outer gaps applied to the cut edges) and the focused window could sit
  off-screen. Wheel input now delegates to
  `TilingController::focusLeft/Right` (or `focusUp/Down` for a vertical strip),
  i.e. the same path as `Meta+Left/Right`, resolved against the active output's
  Emerge engine. That keeps the target column pinned to the viewport edge, moves
  keyboard focus, targets the visible strip even when another engine registered
  the axis shortcut first, and avoids a reflow-before-activation race with
  FocusFollowsMouse.

**Revision 14 (this session):**
- **Main-axis mouse resize is now a divider.** New `interactiveResizeEnded()`
  override trades width between the dragged column and the neighbour on the
  dragged side, keeping the strip's total extent constant. Previously a
  narrower drag shrank the total, the view clamp scrolled left and an off-screen
  column peeked in (three visible at once); now the visible set stays at two and
  off-screen columns stay off-screen. The cross axis still delegates to the base
  per-tile stack behaviour.

**Revision 13 (this session):**
- **Trimmed the Emerge settings.** Removed the center-focused-column and
  center-short-columns options (niri-isms that do not apply here) and the
  respect-minimum-size toggle — the engine now always honours minimum sizes.
  Dropped the corresponding config keys (`EmergeCenterFocusedColumn`,
  `EmergeCenterShortColumns`, `EmergeRespectMinimumSize`) from the kcfg, the
  compositor, and the Kinetic Settings page, and removed the centering policy and
  cross-axis centering from the engine itself (focus now always scrolls the
  minimum to keep the focused column visible). The Emerge section now exposes
  only strip direction and the two preset lists.

**Revision 12 (this session):**
- **Columns keep their own width; the view scrolls (no neighbour resizing).**
  Removed the `baseWidth` split and the remainder-fill: a cycle/resize now changes
  only that column's `width`, and a full-width (1.0) column fills the work area
  **within the outer gaps** with the other column pushed off-screen. Focusing
  toward that other column scrolls it back into view. Out-of-focus columns are
  clipped at the viewport edge, never resized. Added `normalizeWidthsToFill()` so
  a strip narrower than the viewport is scaled up (never empty space).

**Revision 11 (this session):**
- **Cycled size is now durable; only the out-of-focus shrink is temporary.**
  `EmergeColumn::baseWidth` is the column's *held* size. Cycling
  (`switchPresetColumnWidth`), `adjustColumnRatio()` and the main-axis
  `adjustTileSize()` now write `baseWidth`, so the new size is what the column
  keeps when focus returns. `applyFocusedWidths()` derives the live `width`:
  the focused column shows its held width, and a one/two-column strip fills the
  viewport by giving the *other* column the remainder — a temporary shrink,
  recomputed on every focus change and on every reflow. With three or more
  columns the strip overflows and scrolls, so the held widths stand. Replaced
  `resizeColumnWithNeighbour()`.

**Revision 10 (this session):**
- **Resize/cycle is now temporary (Option 2).** Each `EmergeColumn` gained a
  `baseWidth` (the balanced default) alongside the live `width`.
  `rebalanceColumnWidths()` sets both; `resizeColumnWithNeighbour()` moves only
  `width`; and `focusWindow()` resets every column's `width` back to `baseWidth`
  whenever the active column changes. So cycling a window resizes it and its
  neighbour compensates to keep the screen full, but the moment focus moves the
  layout returns to its defined sizes. Dropped the now-redundant under-fill
  normalizer.

**Revision 9 (this session):**
- **Width changes now keep the viewport filled (Option B).**
  `EmergeLayoutEngine::resizeColumnWithNeighbour()` resizes the focused column
  and absorbs the opposite change in a neighbour (right, else left), so the
  strip's total extent is preserved and the screen never shows empty space. The
  preset cycle (`switchPresetColumnWidth`), `adjustColumnRatio()` and the
  main-axis `adjustTileSize()` (mouse resize) all route through it. It also
  re-anchors the view so the resized column stays fully visible.
- **`reflow()` clamps the view offset** to `[0, stripExtent-1]`, so no
  out-of-range anchor can reveal empty space past the end of the strip.
- **`rebalanceColumnWidths()` grows the strip** proportionally when the total
  would fall below one viewport (e.g. after removing a column), so the screen
  stays full.
- A lone column always fills the viewport, so cycling it is a no-op.

**Revision 8 (this session):**
- **Same-screen drag swap fix**: `EmergeLayoutEngine` now remembers the source
  **leaf** in `beginMoveWindow()` (KWin detaches a dragged tiled window from its
  tile on the first movement, so `columnOfWindow(window)` is empty by drop time),
  and `endMoveWindow()` exchanges the two windows between their leaves and
  re-adopts the detached dragged window. Previously the swap was skipped, leaving
  a ghost and a homeless window. Removed the now-dead `swapWindows()` helper.

**Revision 7 (this session):**
- **Cross-monitor drag ghost fix**: `EmergeLayoutEngine::cancelMoveWindow()` now
  removes the dragged window (via `removeWindow()`) when it is still tracked,
  instead of only pruning empty leaves. `TilingController::onWindowMoveFinished()`
  calls `cancelMoveWindow()` *before* the destination engine adopts the window, so
  the source leaf was still occupied at prune time and survived as a ghost column.

**Revision 6 (this session):**
- **Renamed the layout `Scrolling` → `Emerge`** end to end: `LayoutKind::Emerge = 6`,
  `EmergeLayoutEngine`, `src/tiles/emergelayoutengine.{h,cpp}`, and the six config
  keys `Scrolling*` → `Emerge*`. Back-compat: `layoutKindFromString()` and
  `enabledLayoutKinds()` still accept the old `"Scrolling"` name, and
  `TilingController::reconfigure()` migrates the old keys and the persisted
  layout-name values (`EnabledLayouts`, `DefaultLayout`, per-(desktop, output)
  overrides) on first load. This document is now `docs/emerge.md`.

**Revision 5 (this session):**
- **Swap now holds the viewport**: `EmergeLayoutEngine::moveWindow()` captures the
  view offset before the swap and re-derives the anchor afterwards, so the two
  windows visibly exchange slots (and the geometry animation shows it) instead of
  the view scrolling with the active window and making the swap look like a no-op.

**Revision 4 (this session):**
- **Reverted the park position** to just below the union of all outputs. The
  side-park gave the nicer sideways slide but crossed a neighbouring monitor
  during the animation on multi-monitor setups; the below-union park is
  topology-safe and never bleeds. (Same mechanical fix in either case; the
  `geometrychange` effect animates from the previous geometry, so the return
  slides up from the bottom.)

**Revision 3 (this session):**
- **Scroll-direction animation**: off-view tiles are now parked just beyond the
  union of all outputs **along the strip's main axis** at their normal cross
  position (instead of below the union). The `geometrychange` effect animates
  from the previous geometry, so the tile now slides in from the side it left by
  rather than from the bottom. Multi-monitor correctness is preserved for the
  parked state; an inner monitor's tile briefly crosses its neighbour during the
  animation.

**Revision 2 (this session):**
- **Close-left gap fix**: after removing a column, the view anchor is re-clamped
  (`clampedAnchorFor`, or re-centered under `Always`) so the survivors slide over
  to fill the gap instead of leaving the derived view offset negative.
- **Preset-width shortcut**: `TilingController::switchPresetColumnWidth()` plus the
  KGlobalAccel action `Tiling Emerge: Switch Column Width Preset` (no default
  binding).
- **Kinetic Settings**: `Emerge` added to the layout lists; a new **Emerge**
  section on the tiling page exposes strip direction, centering, preset widths /
  heights, respect-minimum-size and center-short-columns. The compositor now reads
  the two enum keys by choice name *or* int, so both UIs round-trip.
- Known limitation: a vertical stack of more than ~6 tiles can hit the tile
  system's relative minimum size (0.15), like the other stacking layouts.
- Known limitation: Shift+Meta+scroll steps one column per notch; a smooth
  kinetic pan is not implemented.
