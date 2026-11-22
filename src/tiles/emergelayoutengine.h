/*
    KWin - the KDE window manager
    SPDX-FileCopyrightText: 2026 KWin Tiling Fork

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "layoutengine.h"
#include "tile.h"

#include <QHash>
#include <QList>
#include <QPointer>

#include <optional>

namespace KWin
{

class CustomTile;
class RootTile;
class Window;

/**
 * The Emerge layout: an endless strip of columns, modelled on the niri
 * compositor (and ported from PlasmaZones' scrolling engine).
 *
 * Windows form an ordered list of COLUMNS on an unbounded strip; the screen is
 * a viewport that slides along it. The one invariant everything here serves:
 * inserting or removing a column NEVER resizes another column — only the
 * viewport moves. Opening a window therefore does not re-divide the output the
 * way a bounded tiling layout does.
 *
 * A column holds a stack of tiles; with EmergeColumnDisplay::Tabbed only the
 * active tile is shown and the rest are parked off-view.
 *
 * Coordinates are relative to the output's client area: the main axis (along
 * the strip) and the cross axis (within a column) each span [0,1]. The view is
 * expressed as an offset along the main axis, kept through an anchor stored
 * RELATIVE to the active column so that structural changes before the focus do
 * not make the focused window drift.
 */

/** How a column resolves its main-axis extent. */
enum class EmergeWidthKind {
    Proportion, ///< fraction of the viewport
    Fixed,      ///< absolute pixels
    Preset,     ///< fraction snapped to the nearest preset-list entry
};

/** How a tile resolves its cross-axis extent within a column. */
enum class EmergeHeightKind {
    Auto,   ///< share of the leftover cross extent, by weight
    Fixed,  ///< absolute pixels
    Preset, ///< fraction snapped to the nearest preset-list entry
};

/** How a column presents its tiles. */
enum class EmergeColumnDisplay {
    Normal, ///< stacked across the cross axis; all tiles visible
    Tabbed, ///< only the active tile is shown
};

struct EmergeColumnWidth
{
    EmergeWidthKind kind = EmergeWidthKind::Proportion;
    qreal proportion = 1.0;
    int fixedPx = 0;
    qreal presetFraction = 0.5;
    /// True once the user explicitly set this width (preset cycle, ratio key or
    /// gesture, or an edge drag). A user-set width survives window add/remove
    /// and focus changes; only never-touched ("default") columns are
    /// re-defaulted when the column count changes.
    bool userSet = false;
    /// Explicit width that normalisation must not rescale (app rules and
    /// manual widths).
    bool pinned = false;
    /// True while an app width rule still owns this width. Any manual width
    /// change clears it, so later config edits no longer live-apply.
    bool ruleOwned = false;
};

struct EmergeWindowHeight
{
    EmergeHeightKind kind = EmergeHeightKind::Auto;
    qreal weight = 1.0;
    int fixedPx = 0;
    qreal presetFraction = 0.5;
};

/** One window in a column, together with the leaf that carries it. */
struct EmergeTile
{
    QPointer<CustomTile> leaf;
    EmergeWindowHeight height;
};

/** One column: a stack of tiles across the strip. */
struct EmergeColumn
{
    QList<EmergeTile> tiles;
    int activeTileIdx = 0;
    /// The column's width. It only changes when the user cycles/resizes the
    /// column; the view scrolls to keep the focused column visible, so an
    /// out-of-focus column is pushed off-screen rather than resized.
    EmergeColumnWidth width;
    EmergeColumnDisplay display = EmergeColumnDisplay::Normal;
};

/**
 * Read-only view of where the strip's viewport currently sits, for external
 * consumers (the D-Bus bridge and, through it, the Noctalia bar widget).
 * Columns that are only partly visible are in neither list.
 */
struct EmergeViewportSnapshot
{
    bool valid = false;      ///< false when there is no active column
    int activeIndex = -1;    ///< focused column, -1 when the strip is empty
    qreal viewOffset = 0.0;  ///< main-axis offset of the viewport's leading edge
    QList<int> columnsLeft;  ///< columns fully before the viewport, farthest first
    QList<int> columnsRight; ///< columns fully past the viewport, nearest first
};

class KWIN_EXPORT EmergeLayoutEngine : public LayoutEngine
{
    Q_OBJECT

public:
    explicit EmergeLayoutEngine(QObject *parent = nullptr);
    ~EmergeLayoutEngine() override;

    LayoutKind layoutKind() const override
    {
        return LayoutKind::Emerge;
    }

    void attach(RootTile *root) override;
    void addWindow(Window *window) override;
    void removeWindow(Window *window) override;
    void moveWindow(Window *window, int delta) override;
    void beginMoveWindow(Window *window) override;
    bool endMoveWindow(Window *window, Window *target) override;
    bool insertWindow(Window *window, Window *target, DropZone zone) override;
    void cancelMoveWindow(Window *window) override;
    void consumeWindow(Window *window) override;
    void expelWindow(Window *window) override;
    void pruneEmptyLeaves() override;
    void reflow() override;

    QList<Window *> windows() const override;
    Window *primaryWindow() const override;
    Window *windowInDirection(Window *from, FocusDirection direction) const override;

    // Emerge surface (the hooks the base class already declares).
    bool supportsViewport() const override
    {
        return true;
    }
    void setViewportOffset(qreal offset) override;
    qreal viewportOffset() const override;

    /// Orientation the strip currently runs along, as resolved by the last reflow.
    Qt::Orientation axis() const
    {
        return m_axis;
    }

    void focusColumnLeft() override;
    void focusColumnRight() override;
    void moveColumnLeft() override;
    void moveColumnRight() override;
    void switchPresetColumnWidth() override;

    /// Scroll the strip to the column holding @p window, if it is a strip
    /// tile. Called by TilingController when compositor focus changes, so the
    /// view follows focus the way niri does.
    void focusWindow(Window *window);

    /// Snapshot of which columns sit fully off each edge of the viewport,
    /// for the D-Bus bridge.
    EmergeViewportSnapshot viewportSnapshot() const;

    /// Scroll the strip to the column at @p index and focus its window.
    /// No-op for an out-of-range index.
    void focusColumn(int index);

    /// Per-app column widths, keyed by lowercased window class. Pushed from
    /// TilingController; also live-applies to columns the rule still owns.
    void setEmergeWindowWidthRules(const QHash<QString, qreal> &rules);
    /// Re-check the width rule for a window whose class arrived after tiling.
    /// No-op unless the window still sits alone in a never-touched column.
    void refreshWindowWidthRule(Window *window);

    bool supportsPerTileResize() const override
    {
        return true;
    }
    void adjustTileSize(Window *window, qreal weightDelta, Qt::Orientation axis) override;
    void adjustColumnRatio(qreal delta) override;
    void interactiveResizeEnded(Window *window, const RectF &before, const RectF &after, Qt::Edge edge,
                                const QSizeF &outputSize) override;

    // ── Config, pushed from TilingController ────────────────────────────────
    void setEmergePresetColumnWidths(const QList<qreal> &presets);
    void setEmergePresetWindowHeights(const QList<qreal> &presets);
    /** 0 = auto (from output aspect), 1 = horizontal, 2 = vertical. */
    void setEmergeAxis(int axis);

    /// Bottom peek band. When enabled, columns scrolled fully off the strip
    /// show a sliver of themselves in a band along the bottom of the work area
    /// instead of being parked out of sight. Suppressed automatically when
    /// another output sits directly below (the peek would draw onto it).
    void setEmergePeek(bool enabled);
    /// Height of the peek band in pixels (0 disables peeking).
    void setEmergePeekHeightPx(int heightPx);

private:
    // Strip geometry (all in relative [0,1] units of the client area).
    qreal resolveColumnExtentRel(const EmergeColumn &col) const;
    qreal resolveColumnExtentRel(const EmergeColumnWidth &width) const;
    void resolveCrossExtents(const EmergeColumn &col, QList<qreal> &out) const;
    qreal columnStripPos(int index) const;
    qreal stripExtent() const;
    qreal viewOffset() const;
    qreal clampedAnchorFor(int columnIndex, qreal anchor) const;
    qreal focusAnchorFor(int targetIdx, int prevIdx, qreal oldViewOffset) const;
    void setActiveColumn(int newIndex);
    void focusAdjacentColumn(int delta);
    void stepWheelFocus(int delta);

    RectF makeRect(qreal mainPos, qreal crossPos, qreal mainExtent, qreal crossExtent) const;
    Qt::Orientation effectiveAxis() const;
    qreal outputMainPx() const;
    qreal outputCrossPx() const;

    /// Place a tile: park it below every output when it is fully off-view,
    /// clamp a straddler to the output edge, or lay it out normally.
    void setTileRect(CustomTile *leaf, qreal mainPos, qreal crossPos, qreal mainExtent, qreal crossExtent,
                     int unionBottom);
    /// Move a tile to the region just below the union of all outputs, where no
    /// monitor can draw it (no neighbour bleed on any multi-monitor layout).
    void parkTile(CustomTile *leaf, qreal mainExtent, qreal crossExtent, int unionBottom);
    /// Lay a fully off-view column into the bottom peek band at @p mainPos with
    /// a slot @p mainExtent wide. The column keeps its full cross layout but is
    /// shifted down so only its top @p peekFrac sits inside the work area.
    void peekColumn(int columnIndex, qreal mainPos, qreal mainExtent, qreal peekFrac, int unionBottom);
    /// True when another output overlaps this one's horizontal span and starts
    /// at or below its bottom edge, so a peek would be drawn onto that monitor.
    bool outputHasMonitorBelow() const;
    /// Bottom edge (global) of the union of every output's geometry.
    int unionBottomPx() const;
    void ensureAxisShortcutRegistered();
    QPointer<QAction> m_wheelForwardAction;

    int columnOfWindow(Window *window) const;
    int tileIndexOfWindow(Window *window) const;
    int totalLeafCount() const;
    /// Rule fraction for the window's class, when one is configured.
    std::optional<qreal> appWidthFraction(const Window *window) const;
    void rebalanceColumnWidths();
    /// Scale the unpinned columns up when the strip's total width would fall
    /// below one viewport, so the screen is never left partly empty. Pinned
    /// columns keep their explicit width.
    void normalizeWidthsToFill();

    QPointer<RootTile> m_root;
    QList<EmergeColumn> m_columns;
    int m_activeColumnIdx = -1;

    /// Active column's leading edge within the viewport (relative main units).
    /// Stored active-relative so an insert/remove before the focus does not
    /// move the focused column. The view offset is derived from it.
    qreal m_viewAnchor = 0.0;

    Qt::Orientation m_axis = Qt::Horizontal;

    // Config (pushed from kwinrc via TilingController).
    QList<qreal> m_presetColumnWidths{1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 1.0};
    QList<qreal> m_presetWindowHeights{1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 1.0};
    QHash<QString, qreal> m_appWidthRules;
    int m_axisSetting = 0; // 0 auto, 1 horizontal, 2 vertical
    bool m_peekEnabled = false;
    int m_peekHeightPx = 48;

    // Drag-move bookkeeping. The source column/tile indices are captured at
    // drag start (like ColumnsLayoutEngine) so insertWindow() can locate the
    // source tile after KWin has detached the dragged window.
    QPointer<CustomTile> m_moveSourceLeaf;
    int m_moveSourceColumn = -1;
    int m_moveSourceTile = -1;
};

} // namespace KWin
