/*
    KWin - the KDE window manager
    SPDX-FileCopyrightText: 2026 KWin Tiling Fork

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "emergelayoutengine.h"

#include "customtile.h"
#include "core/output.h"
#include "tilemanager.h"
#include "tiling/tilingcontroller.h"
#include "window.h"
#include "workspace.h"

#include "input.h"

#include <QtMath>

#include <QDebug>
#include <QAction>

#include <algorithm>
#include <utility>

namespace KWin
{

namespace
{
constexpr qreal MinColumnExtent = 0.05;
constexpr qreal MinCrossShare = 0.02;
constexpr qreal MinWeight = 0.1;
constexpr qreal MaxWeight = 10.0;
constexpr qreal DefaultWeight = 1.0;
/// Normal main-axis width of a column once there is more than one (a lone
/// column fills the viewport at 1.0). Shared by rebalanceColumnWidths() and
/// expelWindow() so an expelled window's new column matches its neighbours.
constexpr qreal DefaultColumnExtent = 0.5;

/// At most this many off-view columns show in the bottom peek band. The nearest
/// ones to the viewport are chosen; any further down the strip stay parked so
/// the band can never crowd into unreadable slivers.
constexpr int MaxPeekedColumns = 2;

/// One-pixel tolerance expressed in relative units, matching PlasmaZones'
/// preset-cycle allowance for fractional-scale rounding.
constexpr qreal PresetEpsilon = 0.001;

/// Distance a parked window sits below the union of all outputs. Small on
/// purpose: just past the bottom-most monitor keeps coordinates sane for KWin
/// while still landing in a region that belongs to no monitor.
constexpr qreal ParkMargin = 16.0;

/// The nearest entry of @p presets to @p fraction, or @p fallback when empty.
qreal nearestPreset(const QList<qreal> &presets, qreal fraction, qreal fallback = 0.5)
{
    if (presets.isEmpty()) {
        return fallback;
    }
    int best = 0;
    for (int i = 1; i < presets.size(); ++i) {
        if (qAbs(presets.at(i) - fraction) < qAbs(presets.at(best) - fraction)) {
            best = i;
        }
    }
    return presets.at(best);
}

/// The preset index a cycle press lands on: strictly wider (forward) or
/// narrower (backward) than @p current, wrapping at the ends by EXTENT rather
/// than by list position (a user-typed list need not be in size order).
int cyclePresetIndexByExtent(const QList<qreal> &presets, qreal current, int delta)
{
    if (presets.isEmpty()) {
        return -1;
    }
    const bool forward = delta >= 0;
    int best = -1;
    qreal bestValue = 0;
    int wrap = -1;
    qreal wrapValue = 0;
    for (int i = 0; i < presets.size(); ++i) {
        const qreal value = presets.at(i);
        if (forward) {
            if (current + PresetEpsilon < value && (best < 0 || value < bestValue)) {
                best = i;
                bestValue = value;
            }
            if (wrap < 0 || value < wrapValue) {
                wrap = i;
                wrapValue = value;
            }
        } else {
            if (value + PresetEpsilon < current && (best < 0 || value > bestValue)) {
                best = i;
                bestValue = value;
            }
            if (wrap < 0 || value > wrapValue) {
                wrap = i;
                wrapValue = value;
            }
        }
    }
    return best >= 0 ? best : wrap;
}
} // namespace

EmergeLayoutEngine::EmergeLayoutEngine(QObject *parent)
    : LayoutEngine(parent)
{
}

EmergeLayoutEngine::~EmergeLayoutEngine() = default;

void EmergeLayoutEngine::attach(RootTile *root)
{
    m_root = root;
    if (!m_root) {
        return;
    }

    // Take full ownership of the root tile: remove any pre-existing default
    // layout children so they cannot fight the strip's geometry, and make the
    // root a plain floating container the engine drives directly.
    const QList<Tile *> existingChildren = m_root->childTiles();
    for (Tile *child : existingChildren) {
        if (CustomTile *custom = qobject_cast<CustomTile *>(child)) {
            m_root->destroyChild(custom);
        }
    }
    m_root->setLayoutDirection(Tile::LayoutDirection::Floating);
    m_root->setRelativeGeometry(RectF(0, 0, 1, 1));

    // Resolve the strip direction now so the first window's anchor maths uses
    // the right orientation (reflow would otherwise only correct it later).
    m_axis = effectiveAxis();

    m_columns.clear();
    m_activeColumnIdx = -1;
    m_viewAnchor = 0.0;
}

// ── Geometry helpers ────────────────────────────────────────────────────────

Qt::Orientation EmergeLayoutEngine::effectiveAxis() const
{
    if (m_axisSetting == 1) {
        return Qt::Horizontal;
    }
    if (m_axisSetting == 2) {
        return Qt::Vertical;
    }
    if (!m_root) {
        return Qt::Horizontal;
    }
    const RectF geom = m_root->manager()->output()->geometryF();
    return geom.width() >= geom.height() ? Qt::Horizontal : Qt::Vertical;
}

qreal EmergeLayoutEngine::outputMainPx() const
{
    if (!m_root) {
        return 0.0;
    }
    const RectF geom = m_root->manager()->output()->geometryF();
    return m_axis == Qt::Horizontal ? geom.width() : geom.height();
}

qreal EmergeLayoutEngine::outputCrossPx() const
{
    if (!m_root) {
        return 0.0;
    }
    const RectF geom = m_root->manager()->output()->geometryF();
    return m_axis == Qt::Horizontal ? geom.height() : geom.width();
}

RectF EmergeLayoutEngine::makeRect(qreal mainPos, qreal crossPos, qreal mainExtent, qreal crossExtent) const
{
    return m_axis == Qt::Horizontal ? RectF(mainPos, crossPos, mainExtent, crossExtent)
                                    : RectF(crossPos, mainPos, crossExtent, mainExtent);
}

int EmergeLayoutEngine::unionBottomPx() const
{
    if (!m_root) {
        return 0;
    }
    // The strip's own client area is the floor, so even a single-output session
    // (or a degenerate output list) still parks below the working screen.
    const RectF own = workspace()->clientArea(MaximizeArea, m_root->manager()->output());
    int bottom = qRound(own.y() + own.height());
    const auto outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        if (!output) {
            continue;
        }
        const RectF geometry = output->geometryF();
        bottom = qMax(bottom, qRound(geometry.y() + geometry.height()));
    }
    return bottom;
}

void EmergeLayoutEngine::parkTile(CustomTile *leaf, qreal mainExtent, qreal crossExtent, int unionBottom)
{
    if (!leaf || !m_root) {
        return;
    }
    // A parked tile is off every output and must never take hover focus.
    leaf->setPeeked(false);
    const RectF area = workspace()->clientArea(MaximizeArea, m_root->manager()->output());
    if (area.width() <= 0.0 || area.height() <= 0.0) {
        return;
    }

    const qreal width = (m_axis == Qt::Horizontal ? mainExtent : crossExtent) * area.width();
    const qreal height = (m_axis == Qt::Horizontal ? crossExtent : mainExtent) * area.height();

    // Just below the union of every output — a region that belongs to no monitor,
    // so KWin cannot draw the window on a neighbour regardless of the monitor
    // topology. windowGeometry() adds half the between-tile gap to the top edge,
    // so fold that back out here: otherwise a large configured gap could pull the
    // parked rect onto a screen.
    const qreal parkTop = unionBottom + 1.0 + ParkMargin - m_root->gapBetween() / 2.0;
    const qreal relY = (parkTop - area.y()) / area.height();

    leaf->setRelativeGeometry(RectF(0.0, relY, width / area.width(), height / area.height()));
}

bool EmergeLayoutEngine::outputHasMonitorBelow() const
{
    if (!m_root) {
        return false;
    }
    LogicalOutput *self = m_root->manager()->output();
    if (!self) {
        return false;
    }
    const RectF own = self->geometryF();
    const qreal ownBottom = own.y() + own.height();
    const auto outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        if (!output || output == self) {
            continue;
        }
        const RectF geom = output->geometryF();
        // A monitor whose top edge is at/under our bottom and which overlaps our
        // horizontal span would render the part of the peek that hangs below.
        if (geom.y() + 1.0 >= ownBottom
            && geom.x() < own.x() + own.width()
            && geom.x() + geom.width() > own.x()) {
            return true;
        }
    }
    return false;
}

void EmergeLayoutEngine::peekColumn(int columnIndex, qreal mainPos, qreal mainExtent, qreal peekFrac,
                                    int unionBottom)
{
    if (columnIndex < 0 || columnIndex >= m_columns.size() || peekFrac <= 0.0) {
        return;
    }
    EmergeColumn &col = m_columns[columnIndex];
    if (col.tiles.isEmpty()) {
        return;
    }

    // The column keeps its full cross layout; the whole stack is shifted down so
    // only its top `peekFrac` sits inside the work area and the rest runs off the
    // bottom edge. The leaves are off-screen tiles, so the below-output part is
    // allowed; on a monitor directly below peeking is suppressed entirely.
    const qreal top = 1.0 - peekFrac;
    if (col.display == EmergeColumnDisplay::Tabbed) {
        const int active = qBound(0, col.activeTileIdx, int(col.tiles.size()) - 1);
        for (int ti = 0; ti < col.tiles.size(); ++ti) {
            CustomTile *leaf = col.tiles.at(ti).leaf;
            if (!leaf) {
                continue;
            }
            if (ti == active) {
                leaf->setPeeked(true);
                leaf->setRelativeGeometry(makeRect(mainPos, top, mainExtent, 1.0));
            } else {
                parkTile(leaf, mainExtent, 1.0, unionBottom);
            }
        }
        return;
    }

    QList<qreal> heights;
    resolveCrossExtents(col, heights);
    qreal cross = top;
    for (int ti = 0; ti < col.tiles.size(); ++ti) {
        CustomTile *leaf = col.tiles.at(ti).leaf;
        const qreal height = heights.value(ti, 1.0);
        if (leaf) {
            leaf->setPeeked(true);
            leaf->setRelativeGeometry(makeRect(mainPos, cross, mainExtent, height));
        }
        cross += height;
    }
}

void EmergeLayoutEngine::setTileRect(CustomTile *leaf, qreal mainPos, qreal crossPos, qreal mainExtent,
                                        qreal crossExtent, int unionBottom)
{
    if (!leaf) {
        return;
    }

    // A normally-visible tile is never a peek.
    leaf->setPeeked(false);

    // Fully outside the viewport: park below every output. A rect left at
    // main >= 1 (or entirely below 0) would land on the neighbouring monitor,
    // which is exactly the multi-monitor bleed this avoids.
    if (mainPos + mainExtent <= 0.0 || mainPos >= 1.0) {
        parkTile(leaf, mainExtent, crossExtent, unionBottom);
        return;
    }

    // Partially visible: keep the tile inside the viewport so its overhang
    // cannot spill onto the adjacent output. The main extent shrinks to the
    // visible part (a straddler is cropped at the edge); the cross axis is left
    // alone so stacked tiles keep their shares.
    qreal pos = mainPos;
    qreal extent = mainExtent;
    if (pos < 0.0) {
        extent += pos; // pos is negative — drop the off-screen overhang
        pos = 0.0;
    }
    if (pos + extent > 1.0) {
        extent = 1.0 - pos;
    }
    extent = qBound(MinColumnExtent, extent, qreal(1.0));

    leaf->setRelativeGeometry(makeRect(pos, crossPos, extent, crossExtent));
}

qreal EmergeLayoutEngine::resolveColumnExtentRel(const EmergeColumn &col) const
{
    return resolveColumnExtentRel(col.width);
}

qreal EmergeLayoutEngine::resolveColumnExtentRel(const EmergeColumnWidth &width) const
{
    qreal extent = width.proportion;
    switch (width.kind) {
    case EmergeWidthKind::Proportion:
        extent = width.proportion;
        break;
    case EmergeWidthKind::Fixed: {
        const qreal mainPx = outputMainPx();
        extent = mainPx > 0.0 ? width.fixedPx / mainPx : 0.5;
        break;
    }
    case EmergeWidthKind::Preset:
        extent = nearestPreset(m_presetColumnWidths, width.presetFraction);
        break;
    }
    return qBound(MinColumnExtent, extent, qreal(1.0));
}

void EmergeLayoutEngine::resolveCrossExtents(const EmergeColumn &col, QList<qreal> &out) const
{
    const int n = col.tiles.size();
    out.clear();
    out.resize(n);
    if (n <= 0) {
        return;
    }

    const qreal crossPx = outputCrossPx();
    qreal autoWeightTotal = 0.0;
    qreal fixedTotal = 0.0;
    for (int i = 0; i < n; ++i) {
        const EmergeWindowHeight &h = col.tiles.at(i).height;
        switch (h.kind) {
        case EmergeHeightKind::Fixed:
            out[i] = crossPx > 0.0 ? qMax(qreal(0.0), h.fixedPx / crossPx) : 0.0;
            fixedTotal += out[i];
            break;
        case EmergeHeightKind::Preset:
            out[i] = nearestPreset(m_presetWindowHeights, h.presetFraction);
            fixedTotal += out[i];
            break;
        case EmergeHeightKind::Auto:
            out[i] = 0.0;
            autoWeightTotal += qMax(qreal(0.0001), h.weight);
            break;
        }
    }

    // Auto tiles share whatever the Fixed/Preset tiles did not claim. The floor
    // keeps a heavily over-subscribed column from collapsing to zero; minimum
    // sizes are always respected.
    const qreal avail = qMax(MinCrossShare, 1.0 - fixedTotal);
    if (autoWeightTotal > 0.0) {
        for (int i = 0; i < n; ++i) {
            if (col.tiles.at(i).height.kind == EmergeHeightKind::Auto) {
                out[i] = avail * qMax(qreal(0.0001), col.tiles.at(i).height.weight) / autoWeightTotal;
            }
        }
    }

    // If Fixed/Preset tiles over-subscribe the cross extent, scale the whole
    // stack down so it still tiles [0,1] exactly.
    qreal total = 0.0;
    for (qreal e : out) {
        total += e;
    }
    if (total > 1.0) {
        for (qreal &e : out) {
            e /= total;
        }
    }
}

qreal EmergeLayoutEngine::columnStripPos(int index) const
{
    qreal pos = 0.0;
    const int end = qBound(0, index, int(m_columns.size()));
    for (int i = 0; i < end; ++i) {
        const qreal extent = resolveColumnExtentRel(m_columns.at(i));
        if (extent > 0.0) {
            pos += extent;
        }
    }
    return pos;
}

qreal EmergeLayoutEngine::stripExtent() const
{
    qreal total = 0.0;
    for (const EmergeColumn &col : m_columns) {
        total += resolveColumnExtentRel(col);
    }
    return total;
}

qreal EmergeLayoutEngine::viewOffset() const
{
    if (m_activeColumnIdx < 0 || m_activeColumnIdx >= m_columns.size()) {
        return 0.0;
    }
    return columnStripPos(m_activeColumnIdx) - m_viewAnchor;
}

qreal EmergeLayoutEngine::clampedAnchorFor(int columnIndex, qreal anchor) const
{
    if (columnIndex < 0 || columnIndex >= m_columns.size()) {
        return 0.0;
    }
    const qreal activePos = columnStripPos(columnIndex);
    const qreal maxViewOffset = qMax(qreal(0), stripExtent() - 1.0);
    const qreal offset = qBound(qreal(0), activePos - anchor, maxViewOffset);
    return activePos - offset;
}

qreal EmergeLayoutEngine::focusAnchorFor(int targetIdx, int prevIdx, qreal oldViewOffset) const
{
    if (targetIdx < 0 || targetIdx >= m_columns.size()) {
        return 0.0;
    }

    const qreal colMain = resolveColumnExtentRel(m_columns.at(targetIdx));
    const qreal activePos = columnStripPos(targetIdx);

    // Scroll the minimum so the focused column is fully visible, pinning it to
    // the edge it entered from.
    qreal pos = activePos - oldViewOffset;
    if (colMain >= 1.0) {
        pos = (prevIdx >= 0 && targetIdx < prevIdx) ? 1.0 - colMain : 0.0;
    } else if (pos < 0.0) {
        pos = 0.0;
    } else if (pos + colMain > 1.0) {
        pos = 1.0 - colMain;
    }
    return clampedAnchorFor(targetIdx, pos);
}

void EmergeLayoutEngine::setActiveColumn(int newIndex)
{
    if (newIndex < 0 || newIndex >= m_columns.size() || newIndex == m_activeColumnIdx) {
        return;
    }
    const int prevIdx = m_activeColumnIdx;
    // Captured BEFORE the switch: viewOffset() is derived from the OLD active.
    const qreal oldViewOffset = viewOffset();
    m_activeColumnIdx = newIndex;
    m_viewAnchor = focusAnchorFor(newIndex, prevIdx, oldViewOffset);
}

// ── Lookups ─────────────────────────────────────────────────────────────────

int EmergeLayoutEngine::columnOfWindow(Window *window) const
{
    for (int c = 0; c < m_columns.size(); ++c) {
        const EmergeColumn &col = m_columns.at(c);
        for (const EmergeTile &tile : col.tiles) {
            if (tile.leaf && tile.leaf->windows().contains(window)) {
                return c;
            }
        }
    }
    return -1;
}

int EmergeLayoutEngine::tileIndexOfWindow(Window *window) const
{
    const int c = columnOfWindow(window);
    if (c < 0) {
        return -1;
    }
    const EmergeColumn &col = m_columns.at(c);
    for (int ti = 0; ti < col.tiles.size(); ++ti) {
        if (col.tiles.at(ti).leaf && col.tiles.at(ti).leaf->windows().contains(window)) {
            return ti;
        }
    }
    return -1;
}

int EmergeLayoutEngine::totalLeafCount() const
{
    int count = 0;
    for (const EmergeColumn &col : m_columns) {
        count += col.tiles.size();
    }
    return count;
}

std::optional<qreal> EmergeLayoutEngine::appWidthFraction(const Window *window) const
{
    if (!window || m_appWidthRules.isEmpty()) {
        return std::nullopt;
    }
    const QString windowClass = window->resourceClass().trimmed().toLower();
    if (windowClass.isEmpty()) {
        return std::nullopt;
    }
    const auto it = m_appWidthRules.constFind(windowClass);
    if (it == m_appWidthRules.constEnd()) {
        return std::nullopt;
    }
    return it.value();
}

void EmergeLayoutEngine::focusWindow(Window *window)
{
    if (m_columns.isEmpty() || !window) {
        return;
    }
    const int column = columnOfWindow(window);
    if (column < 0) {
        return; // focus is outside the strip (a floating window)
    }

    bool changed = false;
    const int tile = tileIndexOfWindow(window);
    if (tile >= 0 && m_columns[column].activeTileIdx != tile) {
        // Track the focused tile so a Tabbed column shows the focused window.
        m_columns[column].activeTileIdx = tile;
        changed = true;
    }
    if (column != m_activeColumnIdx) {
        // setActiveColumn() scrolls the view so the newly focused column is fully
        // visible; the previous column keeps its width and is simply pushed aside.
        setActiveColumn(column);
        changed = true;
    }

    if (changed) {
        reflow();
    }
}

// ── Window lifecycle ────────────────────────────────────────────────────────

void EmergeLayoutEngine::addWindow(Window *window)
{
    if (!m_root) {
        return;
    }
    if (columnOfWindow(window) >= 0) {
        return;
    }

    CustomTile *leaf = m_root->createChildAt(RectF(0, 0, 1, 1), Tile::LayoutDirection::Floating, totalLeafCount());
    // The strip lives beyond the output edges; opt this tile out of the
    // on-screen clamp so off-view columns keep their true geometry.
    leaf->setAllowOffscreen(true);

    EmergeColumn column;
    EmergeTile tile;
    tile.leaf = leaf;
    column.tiles.append(tile);
    column.activeTileIdx = 0;

    if (const std::optional<qreal> fraction = appWidthFraction(window)) {
        column.width.kind = EmergeWidthKind::Proportion;
        column.width.proportion = *fraction;
        column.width.userSet = true;
        column.width.pinned = true;
        column.width.ruleOwned = true;
    }

    const int insertAt = (m_activeColumnIdx >= 0) ? qMin(m_activeColumnIdx + 1, int(m_columns.size()))
                                                  : 0;
    m_columns.insert(insertAt, column);

    if (!leaf->manage(window)) {
        qWarning() << "EmergeLayoutEngine: failed to manage window" << window->caption();
        m_columns.removeAt(insertAt);
        m_root->destroyChild(leaf);
        return;
    }

    // Equalise all column widths so the first window fills the full width and
    // subsequent windows split evenly (like MasterStack/Columns).
    rebalanceColumnWidths();

    ensureAxisShortcutRegistered();

    // Focus the new column (a fresh window is the one to look at), keeping the
    // previously focused column from drifting via the active-relative anchor.
    setActiveColumn(insertAt);
    reflow();
}

void EmergeLayoutEngine::removeWindow(Window *window)
{
    const int c = columnOfWindow(window);
    if (c < 0) {
        return;
    }
    const int ti = tileIndexOfWindow(window);
    if (ti < 0) {
        return;
    }

    CustomTile *leaf = m_columns[c].tiles.at(ti).leaf;
    if (leaf) {
        leaf->unmanage(window);
        m_root->destroyChild(leaf);
    }
    m_columns[c].tiles.removeAt(ti);

    if (m_columns[c].tiles.isEmpty()) {
        m_columns.removeAt(c);
        if (m_columns.isEmpty()) {
            m_activeColumnIdx = -1;
            m_viewAnchor = 0.0;
        } else {
            if (m_activeColumnIdx >= m_columns.size()) {
                m_activeColumnIdx = m_columns.size() - 1;
            } else if (m_activeColumnIdx > c) {
                m_activeColumnIdx--;
            }
            // Re-clamp the view after the strip shortens. Without this a column
            // removed to the LEFT of the active one leaves the derived view
            // offset negative, so the survivors sit past the trailing edge and
            // an empty gap remains where the closed window was. Clamping slides
            // them over to fill it.
            m_viewAnchor = clampedAnchorFor(m_activeColumnIdx, m_viewAnchor);
        }
    } else if (m_columns[c].activeTileIdx >= m_columns[c].tiles.size()) {
        m_columns[c].activeTileIdx = m_columns[c].tiles.size() - 1;
    }

    // Going back down to a single column restores its full-width default.
    rebalanceColumnWidths();

    reflow();
}

void EmergeLayoutEngine::moveWindow(Window *window, int delta)
{
    if (delta == 0) {
        return;
    }
    const int sourceIdx = columnOfWindow(window);
    const int sourceTi = tileIndexOfWindow(window);
    if (sourceIdx < 0 || sourceTi < 0) {
        return;
    }

    // Find the target by walking the flat window list in the requested
    // direction — the same list swapInDirection() indexes into.
    const QList<Window *> ws = windows();
    const int from = ws.indexOf(window);
    if (from < 0) {
        return;
    }
    const int to = from + delta;
    if (to < 0 || to >= ws.size()) {
        return;
    }
    Window *target = ws.at(to);
    if (!target || target == window) {
        return;
    }
    const int targetIdx = columnOfWindow(target);
    const int targetTi = tileIndexOfWindow(target);
    if (targetIdx < 0 || targetTi < 0) {
        return;
    }

    // Hold the viewport still across the swap. The anchor is active-relative, so
    // letting it follow the active window into its new column would scroll the
    // view with it and make the swap look like nothing happened; keeping the view
    // offset fixed is what makes the two windows visibly exchange slots (and lets
    // the geometry animation show it).
    const qreal oldViewOffset = viewOffset();

    if (sourceIdx == targetIdx) {
        // Same column: swap the two tiles within the stack.
        m_columns[sourceIdx].tiles.swapItemsAt(sourceTi, targetTi);
    } else {
        // Different columns: swap the columns, then follow the active window.
        qSwap(m_columns[sourceIdx], m_columns[targetIdx]);
        if (m_activeColumnIdx == sourceIdx) {
            m_activeColumnIdx = targetIdx;
        } else if (m_activeColumnIdx == targetIdx) {
            m_activeColumnIdx = sourceIdx;
        }
    }

    if (m_activeColumnIdx >= 0 && m_activeColumnIdx < m_columns.size()) {
        m_viewAnchor = columnStripPos(m_activeColumnIdx) - oldViewOffset;
    }

    reflow();
}

void EmergeLayoutEngine::beginMoveWindow(Window *window)
{
    // Remember the leaf, not just the column: KWin detaches a dragged tiled
    // window from its tile on the first movement, so by drop time the window is
    // no longer findable by id — the (now empty) leaf is the only handle on
    // where it came from. The column/tile indices are kept alongside it so the
    // drop-zone path can slot the tile back into a column without searching.
    m_moveSourceLeaf.clear();
    m_moveSourceColumn = -1;
    m_moveSourceTile = -1;
    const int column = columnOfWindow(window);
    if (column < 0) {
        return;
    }
    const int tile = tileIndexOfWindow(window);
    if (tile >= 0) {
        m_moveSourceLeaf = m_columns[column].tiles.at(tile).leaf;
        m_moveSourceColumn = column;
        m_moveSourceTile = tile;
    }
}

bool EmergeLayoutEngine::endMoveWindow(Window *window, Window *target)
{
    QPointer<CustomTile> sourceLeaf = m_moveSourceLeaf;
    m_moveSourceLeaf.clear();
    m_moveSourceColumn = -1;
    m_moveSourceTile = -1;

    if (!sourceLeaf) {
        reflow();
        return false;
    }

    if (target && target != window) {
        const int targetColumn = columnOfWindow(target);
        const int targetTile = tileIndexOfWindow(target);
        if (targetColumn >= 0 && targetTile >= 0) {
            CustomTile *targetLeaf = m_columns[targetColumn].tiles.at(targetTile).leaf;
            if (targetLeaf && targetLeaf != sourceLeaf) {
                // Exchange the two windows between their leaves. The leaves stay
                // put; only their window membership moves, so a detached dragged
                // window is re-adopted rather than left homeless.
                const qreal oldViewOffset = viewOffset();
                targetLeaf->unmanage(target);
                targetLeaf->manage(window);
                sourceLeaf->manage(target);
                // Follow the dragged window to its new column, holding the
                // viewport still so the two windows visibly exchange slots.
                m_activeColumnIdx = targetColumn;
                if (m_activeColumnIdx >= 0 && m_activeColumnIdx < m_columns.size()) {
                    m_viewAnchor = columnStripPos(m_activeColumnIdx) - oldViewOffset;
                }
                reflow();
                return true;
            }
        }
    }

    // No valid target (or dropped on its own slot): snap the dragged window
    // back into the leaf it came from.
    if (!sourceLeaf->windows().contains(window)) {
        sourceLeaf->manage(window);
    }
    reflow();
    return true;
}

bool EmergeLayoutEngine::insertWindow(Window *window, Window *target, DropZone zone)
{
    if (!window || !target || target == window) {
        return false;
    }

    // Locate the source tile: directly when the window is managed (e.g. just
    // adopted from another output), otherwise via beginMoveWindow() bookkeeping
    // (a real drag detaches the window from its tile).
    QPointer<CustomTile> sourceLeaf;
    int srcCol = -1;
    int srcTile = -1;
    if (const int c = columnOfWindow(window); c >= 0) {
        srcCol = c;
        srcTile = tileIndexOfWindow(window);
        if (srcTile >= 0) {
            sourceLeaf = m_columns[c].tiles.at(srcTile).leaf;
        }
    } else if (m_moveSourceLeaf) {
        sourceLeaf = m_moveSourceLeaf;
        srcCol = m_moveSourceColumn;
        srcTile = m_moveSourceTile;
    }

    if (!sourceLeaf) {
        m_moveSourceColumn = -1;
        m_moveSourceTile = -1;
        return false;
    }

    // The recorded indices are usually authoritative, but fall back to a search
    // in case an unrelated reflow moved the source tile mid-drag. Locating the
    // tile is what makes the insertion possible.
    const bool indicesValid = srcCol >= 0 && srcCol < m_columns.size() && srcTile >= 0
        && srcTile < m_columns[srcCol].tiles.size() && m_columns[srcCol].tiles.at(srcTile).leaf == sourceLeaf;
    if (!indicesValid) {
        bool found = false;
        for (int c = 0; c < m_columns.size() && !found; ++c) {
            for (int t = 0; t < m_columns[c].tiles.size(); ++t) {
                if (m_columns[c].tiles.at(t).leaf == sourceLeaf) {
                    srcCol = c;
                    srcTile = t;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            // Nothing to insert relative to; let the swap path at least restore
            // the window to its source slot (uses the still-set bookkeeping).
            return endMoveWindow(window, target);
        }
    }

    const int targetCol0 = columnOfWindow(target);
    const int targetTile0 = tileIndexOfWindow(target);
    if (targetCol0 < 0 || targetTile0 < 0) {
        // The target vanished mid-drag; restore the window to its source slot.
        m_moveSourceLeaf.clear();
        m_moveSourceColumn = -1;
        m_moveSourceTile = -1;
        if (!sourceLeaf->windows().contains(window)) {
            sourceLeaf->manage(window);
        }
        reflow();
        return true;
    }
    int targetCol = targetCol0;
    int targetTile = targetTile0;

    // Capture the pre-move view offset so the re-anchor below is relative to
    // where the strip already was; the drop then scrolls only as far as needed
    // to keep the target visible instead of lurching.
    const int prevActive = m_activeColumnIdx;
    const qreal oldViewOffset = viewOffset();

    if (zone == DropZone::Swap) {
        CustomTile *targetLeaf = m_columns[targetCol].tiles.at(targetTile).leaf;
        m_moveSourceLeaf.clear();
        m_moveSourceColumn = -1;
        m_moveSourceTile = -1;
        if (targetLeaf && targetLeaf != sourceLeaf) {
            // Exchange the two windows between their leaves; the leaves stay
            // put, so a detached dragged window is re-adopted rather than left
            // homeless.
            targetLeaf->unmanage(target);
            targetLeaf->manage(window);
            sourceLeaf->manage(target);
            m_activeColumnIdx = targetCol;
            m_viewAnchor = columnStripPos(m_activeColumnIdx) - oldViewOffset;
        } else if (!sourceLeaf->windows().contains(window)) {
            sourceLeaf->manage(window);
        }
        reflow();
        return true;
    }

    if (zone == DropZone::InsertLeft || zone == DropZone::InsertRight) {
        // Left/Right: give the dragged tile its own new column on the requested
        // side of the target's column (a lone source column is simply moved).
        EmergeTile tile = m_columns[srcCol].tiles.takeAt(srcTile);
        if (m_columns[srcCol].activeTileIdx >= m_columns[srcCol].tiles.size()) {
            m_columns[srcCol].activeTileIdx = qMax(0, m_columns[srcCol].tiles.size() - 1);
        }
        if (m_columns[srcCol].tiles.isEmpty()) {
            m_columns.removeAt(srcCol);
            if (srcCol < targetCol) {
                targetCol -= 1;
            }
        }

        EmergeColumn column;
        column.tiles.append(tile);
        column.activeTileIdx = 0;
        column.width.proportion = DefaultColumnExtent;
        const int insertCol = (zone == DropZone::InsertLeft) ? targetCol : targetCol + 1;
        m_columns.insert(insertCol, column);

        m_moveSourceLeaf.clear();
        m_moveSourceColumn = -1;
        m_moveSourceTile = -1;

        if (!sourceLeaf->windows().contains(window)) {
            sourceLeaf->manage(window);
        }
        m_activeColumnIdx = insertCol;
        m_viewAnchor = focusAnchorFor(m_activeColumnIdx, prevActive, oldViewOffset);
        reflow();
        return true;
    }

    // Above/Below: join the target's column. Detach the dragged tile (leaf +
    // height intent) from its source column.
    EmergeTile tile = m_columns[srcCol].tiles.takeAt(srcTile);

    // Same-column reorder: removing the source shifted the target up.
    if (srcCol == targetCol && srcTile < targetTile) {
        targetTile -= 1;
    }

    if (m_columns[srcCol].tiles.isEmpty()) {
        m_columns.removeAt(srcCol);
        if (srcCol < targetCol) {
            targetCol -= 1;
        }
    } else if (m_columns[srcCol].activeTileIdx >= m_columns[srcCol].tiles.size()) {
        m_columns[srcCol].activeTileIdx = m_columns[srcCol].tiles.size() - 1;
    }

    const int insertTile = (zone == DropZone::InsertAbove) ? targetTile : targetTile + 1;
    m_columns[targetCol].tiles.insert(insertTile, tile);
    m_columns[targetCol].activeTileIdx = insertTile;

    // A real interactive drag detaches the window from its tile on the first
    // movement, so the leaf we just moved is usually empty. Re-adopt the window
    // so it snaps into its new slot instead of staying floating.
    if (!sourceLeaf->windows().contains(window)) {
        sourceLeaf->manage(window);
    }

    m_moveSourceLeaf.clear();
    m_moveSourceColumn = -1;
    m_moveSourceTile = -1;

    // Follow the dragged window into the target column.
    m_activeColumnIdx = targetCol;
    m_viewAnchor = focusAnchorFor(m_activeColumnIdx, prevActive, oldViewOffset);
    reflow();
    return true;
}

void EmergeLayoutEngine::rebalanceColumnWidths()
{
    const int n = m_columns.size();
    if (n <= 0) {
        return;
    }
    // A lone column fills the viewport; two split it evenly; from the third on
    // each is half the viewport so the strip overflows and the view scrolls.
    // Cycling changes a single column's width; nothing else is resized.
    const qreal defined = (n == 1) ? 1.0 : DefaultColumnExtent;
    for (EmergeColumn &col : m_columns) {
        // A width the user set by hand (or an app rule pinned) persists across
        // window add/remove and focus changes; only never-touched columns pick
        // up the default again.
        if (col.width.pinned || col.width.userSet) {
            continue;
        }
        col.width = EmergeColumnWidth{};
        col.width.proportion = defined;
    }
    normalizeWidthsToFill();
}

void EmergeLayoutEngine::normalizeWidthsToFill()
{
    qreal total = 0.0;
    qreal pinnedTotal = 0.0;
    for (const EmergeColumn &col : m_columns) {
        const qreal extent = resolveColumnExtentRel(col);
        total += extent;
        if (col.width.pinned) {
            pinnedTotal += extent;
        }
    }
    if (total <= 0.0 || total >= 1.0) {
        return;
    }

    const bool hasPinned = pinnedTotal > 0.0;
    const qreal remaining = 1.0 - pinnedTotal;
    const qreal scalable = total - pinnedTotal;
    if (hasPinned && (remaining <= 0.0 || scalable <= 0.0)) {
        // Nothing unpinned to stretch, or no room left: keep the intentional gap.
        return;
    }
    // Pinned columns keep their explicit width; the rest are scaled up to fill
    // the screen. Without pinned columns every column shares the scale.
    const qreal scale = hasPinned ? remaining / scalable : 1.0 / total;
    for (EmergeColumn &col : m_columns) {
        if (col.width.pinned) {
            continue;
        }
        const qreal scaled = resolveColumnExtentRel(col) * scale;
        // Keep the user-set marker across the rescale, so a column that was
        // widened to fill the screen is not later treated as an untouched
        // default and reset underneath the user.
        const bool userSet = col.width.userSet;
        col.width = EmergeColumnWidth{};
        col.width.proportion = qBound(MinColumnExtent, scaled, qreal(1.0));
        col.width.userSet = userSet;
    }
}

void EmergeLayoutEngine::ensureAxisShortcutRegistered()
{
    if (m_wheelForwardAction) {
        return;
    }
    // Shift+Meta+scroll steps focus through the strip. Delegate to the
    // controller's keyboard focus path so the wheel and Meta+Left/Right share
    // one implementation and always target the active output/desktop, not
    // whichever engine happened to register this action first. The keyboard
    // path also activates before reflowing, which matters with
    // FocusFollowsMouse (see stepWheelFocus()).
    m_wheelForwardAction = new QAction(this);
    connect(m_wheelForwardAction.data(), &QAction::triggered, this, [this]() {
        stepWheelFocus(1);
    });
    auto *wheelBackAction = new QAction(this);
    connect(wheelBackAction, &QAction::triggered, this, [this]() {
        stepWheelFocus(-1);
    });
    // Shift+Meta+scroll — avoids conflict with the VirtualDesktopManager's
    // Meta+scroll desktop-switch shortcuts.
    input()->registerAxisShortcut(Qt::MetaModifier | Qt::ShiftModifier,
                                  PointerAxisDown, m_wheelForwardAction.data());
    input()->registerAxisShortcut(Qt::MetaModifier | Qt::ShiftModifier,
                                  PointerAxisUp, wheelBackAction);
}

void EmergeLayoutEngine::stepWheelFocus(int delta)
{
    // This action may belong to an engine on another output or desktop (every
    // Emerge engine registers the same axis shortcut), so resolve the active
    // strip at trigger time instead of acting on this instance.
    TilingController *controller = workspace()->tilingController();
    EmergeLayoutEngine *engine = controller ? controller->activeEmergeLayoutEngine() : nullptr;
    if (!engine) {
        return;
    }

    // Reuse the keyboard paths: they start from the actually focused window
    // and activate the target before the strip reflows. Reflowing first would
    // move windows under the stationary pointer and make FocusFollowsMouse
    // schedule a delayed focus for whatever landed under the cursor, undoing
    // the move.
    const bool forward = delta > 0;
    if (engine->axis() == Qt::Horizontal) {
        if (forward) {
            controller->focusRight();
        } else {
            controller->focusLeft();
        }
    } else {
        if (forward) {
            controller->focusDown();
        } else {
            controller->focusUp();
        }
    }
}

void EmergeLayoutEngine::cancelMoveWindow(Window *window)
{
    m_moveSourceLeaf.clear();
    m_moveSourceColumn = -1;
    m_moveSourceTile = -1;

    // The window is leaving this engine for another output/desktop. It may
    // still be tracked here (TilingController calls cancelMoveWindow BEFORE the
    // destination engine adopts it, so KWin has not necessarily detached it
    // yet), in which case take it out and close its column so the survivors fill
    // the gap. Otherwise KWin already detached it during the drag and left an
    // empty leaf/column behind, so prune that.
    if (columnOfWindow(window) >= 0) {
        removeWindow(window);
        return;
    }
    pruneEmptyLeaves();
}

void EmergeLayoutEngine::pruneEmptyLeaves()
{
    if (!m_root) {
        return;
    }

    bool changed = false;
    for (int c = m_columns.size() - 1; c >= 0; --c) {
        for (int ti = m_columns[c].tiles.size() - 1; ti >= 0; --ti) {
            CustomTile *leaf = m_columns[c].tiles.at(ti).leaf;
            if (leaf && !leaf->windows().isEmpty()) {
                continue;
            }
            if (leaf) {
                m_root->destroyChild(leaf);
            }
            m_columns[c].tiles.removeAt(ti);
            changed = true;
        }
        if (m_columns[c].tiles.isEmpty()) {
            m_columns.removeAt(c);
            changed = true;
        }
    }

    if (!changed) {
        return;
    }

    if (m_columns.isEmpty()) {
        m_activeColumnIdx = -1;
        m_viewAnchor = 0.0;
    } else if (m_activeColumnIdx >= m_columns.size()) {
        m_activeColumnIdx = m_columns.size() - 1;
        m_viewAnchor = clampedAnchorFor(m_activeColumnIdx, m_viewAnchor);
    }
    reflow();
}

void EmergeLayoutEngine::consumeWindow(Window *window)
{
    const int c = columnOfWindow(window);
    if (c < 0) {
        return;
    }
    const int ti = tileIndexOfWindow(window);
    if (ti < 0) {
        return;
    }

    // Prefer the column to the right; fall back to the left.
    int target = c + 1;
    if (target >= m_columns.size()) {
        target = c - 1;
    }
    if (target < 0) {
        return; // a lone column has nowhere to consume into
    }

    // Follow the consumed window: the target column becomes active and the view
    // scrolls the minimum needed to keep it visible. Without this the window
    // could move off-screen while the view stayed behind on its old column.
    const int prevActive = m_activeColumnIdx;
    const qreal oldViewOffset = viewOffset();

    EmergeTile tile = m_columns[c].tiles.takeAt(ti);
    if (m_columns[c].tiles.isEmpty()) {
        m_columns.removeAt(c);
        if (c < target) {
            target--;
        }
    } else if (m_columns[c].activeTileIdx >= m_columns[c].tiles.size()) {
        m_columns[c].activeTileIdx = m_columns[c].tiles.size() - 1;
    }

    m_columns[target].tiles.append(tile);
    // Track the consumed window in its new column (important for Tabbed).
    m_columns[target].activeTileIdx = m_columns[target].tiles.size() - 1;

    m_activeColumnIdx = target;
    m_viewAnchor = focusAnchorFor(m_activeColumnIdx, prevActive, oldViewOffset);
    reflow();
}

void EmergeLayoutEngine::expelWindow(Window *window)
{
    const int c = columnOfWindow(window);
    if (c < 0 || m_columns[c].tiles.size() <= 1) {
        return; // already alone in its column
    }
    const int ti = tileIndexOfWindow(window);
    if (ti < 0) {
        return;
    }

    // Follow the expelled window into its new column; otherwise the window
    // lands off-screen while the view stays behind and looks like it vanished.
    const int prevActive = m_activeColumnIdx;
    const qreal oldViewOffset = viewOffset();

    EmergeTile tile = m_columns[c].tiles.takeAt(ti);
    if (m_columns[c].activeTileIdx >= m_columns[c].tiles.size()) {
        m_columns[c].activeTileIdx = m_columns[c].tiles.size() - 1;
    }

    EmergeColumn column;
    column.tiles.append(tile);
    column.activeTileIdx = 0;
    // A freshly expelled column gets the normal multi-column width instead of
    // the struct default (1.0), so it does not dwarf its neighbours.
    column.width.proportion = DefaultColumnExtent;

    // The new column goes to the right of the old one, or to its left when the
    // old column is already rightmost.
    int insertAt = c + 1;
    if (c == m_columns.size() - 1) {
        insertAt = c;
    }
    m_columns.insert(insertAt, column);

    m_activeColumnIdx = insertAt;
    m_viewAnchor = focusAnchorFor(m_activeColumnIdx, prevActive, oldViewOffset);
    reflow();
}

// ── Queries ─────────────────────────────────────────────────────────────────

QList<Window *> EmergeLayoutEngine::windows() const
{
    QList<Window *> result;
    for (const EmergeColumn &col : m_columns) {
        for (const EmergeTile &tile : col.tiles) {
            if (tile.leaf) {
                result.append(tile.leaf->windows());
            }
        }
    }
    return result;
}

Window *EmergeLayoutEngine::primaryWindow() const
{
    if (m_activeColumnIdx >= 0 && m_activeColumnIdx < m_columns.size()) {
        const EmergeColumn &col = m_columns.at(m_activeColumnIdx);
        if (!col.tiles.isEmpty()) {
            const int ti = qBound(0, col.activeTileIdx, int(col.tiles.size()) - 1);
            if (col.tiles.at(ti).leaf && !col.tiles.at(ti).leaf->windows().isEmpty()) {
                return col.tiles.at(ti).leaf->windows().first();
            }
        }
    }
    for (const EmergeColumn &col : m_columns) {
        for (const EmergeTile &tile : col.tiles) {
            if (tile.leaf && !tile.leaf->windows().isEmpty()) {
                return tile.leaf->windows().first();
            }
        }
    }
    return nullptr;
}

Window *EmergeLayoutEngine::windowInDirection(Window *from, FocusDirection direction) const
{
    const QList<Window *> ws = windows();
    if (ws.isEmpty()) {
        return nullptr;
    }
    if (!from || !ws.contains(from)) {
        return ws.first();
    }

    const int c = columnOfWindow(from);
    const int ti = tileIndexOfWindow(from);
    if (c < 0 || ti < 0) {
        return nullptr;
    }

    // Map the screen direction onto the strip's roles: for a horizontal strip
    // Left/Right walk columns and Up/Down walk the stack; a vertical strip
    // inverts that.
    int dCol = 0;
    int dTile = 0;
    switch (direction) {
    case FocusDirection::Left:
        (m_axis == Qt::Horizontal ? dCol : dTile) = -1;
        break;
    case FocusDirection::Right:
        (m_axis == Qt::Horizontal ? dCol : dTile) = 1;
        break;
    case FocusDirection::Up:
        (m_axis == Qt::Horizontal ? dTile : dCol) = -1;
        break;
    case FocusDirection::Down:
        (m_axis == Qt::Horizontal ? dTile : dCol) = 1;
        break;
    }

    if (dCol != 0) {
        const int nc = qBound(0, c + dCol, int(m_columns.size()) - 1);
        if (nc == c) {
            return nullptr;
        }
        const EmergeColumn &col = m_columns.at(nc);
        if (col.tiles.isEmpty()) {
            return nullptr;
        }
        const int nt = qBound(0, qMin(ti, int(col.tiles.size()) - 1), int(col.tiles.size()) - 1);
        if (col.tiles.at(nt).leaf && !col.tiles.at(nt).leaf->windows().isEmpty()) {
            return col.tiles.at(nt).leaf->windows().first();
        }
        return nullptr;
    }

    const EmergeColumn &col = m_columns.at(c);
    const int nt = qBound(0, ti + dTile, int(col.tiles.size()) - 1);
    if (nt == ti) {
        return nullptr;
    }
    int flat = 0;
    for (int cc = 0; cc < c; ++cc) {
        flat += m_columns.at(cc).tiles.size();
    }
    flat += nt;
    return ws.value(flat);
}

// ── Viewport / focus verbs ──────────────────────────────────────────────────

void EmergeLayoutEngine::setViewportOffset(qreal offset)
{
    if (m_activeColumnIdx < 0 || m_activeColumnIdx >= m_columns.size()) {
        return;
    }
    m_viewAnchor = columnStripPos(m_activeColumnIdx) - offset;
    reflow();
}

qreal EmergeLayoutEngine::viewportOffset() const
{
    return viewOffset();
}

EmergeViewportSnapshot EmergeLayoutEngine::viewportSnapshot() const
{
    EmergeViewportSnapshot snap;
    if (m_activeColumnIdx < 0 || m_activeColumnIdx >= m_columns.size()) {
        return snap;
    }

    snap.valid = true;
    snap.activeIndex = m_activeColumnIdx;
    snap.viewOffset = viewOffset();
    const qreal viewEnd = snap.viewOffset + 1.0;
    constexpr qreal epsilon = 1e-6;

    qreal cursor = 0.0;
    for (int i = 0; i < m_columns.size(); ++i) {
        const qreal extent = resolveColumnExtentRel(m_columns.at(i));
        if (extent <= 0.0) {
            continue;
        }
        if (cursor + extent <= snap.viewOffset + epsilon) {
            // Fully before the viewport (left/up). Strip order means the first
            // entry is the farthest from the viewport.
            snap.columnsLeft.append(i);
        } else if (cursor >= viewEnd - epsilon) {
            // Fully past the viewport (right/down). Strip order means the first
            // entry is the nearest to the viewport.
            snap.columnsRight.append(i);
        }
        cursor += extent;
    }
    return snap;
}

void EmergeLayoutEngine::focusAdjacentColumn(int delta)
{
    if (m_columns.isEmpty()) {
        return;
    }
    const int current = m_activeColumnIdx >= 0 ? m_activeColumnIdx : 0;
    const int next = qBound(0, current + delta, int(m_columns.size()) - 1);
    if (next == current && m_activeColumnIdx >= 0) {
        return;
    }
    setActiveColumn(next);
    reflow();
}

void EmergeLayoutEngine::focusColumnLeft()
{
    focusAdjacentColumn(-1);
}

void EmergeLayoutEngine::focusColumnRight()
{
    focusAdjacentColumn(1);
}

void EmergeLayoutEngine::focusColumn(int index)
{
    if (index < 0 || index >= m_columns.size()) {
        return;
    }
    if (index != m_activeColumnIdx) {
        setActiveColumn(index);
    }
    reflow();

    // Hand keyboard focus to the column so the scroll matches what the user
    // clicked; the controller's windowActivated hook then re-anchors the view.
    if (Window *target = primaryWindow()) {
        workspace()->activateWindow(target);
    }
}

void EmergeLayoutEngine::moveColumnLeft()
{
    moveWindow(primaryWindow(), -1);
}

void EmergeLayoutEngine::moveColumnRight()
{
    moveWindow(primaryWindow(), 1);
}

void EmergeLayoutEngine::switchPresetColumnWidth()
{
    if (m_activeColumnIdx < 0 || m_activeColumnIdx >= m_columns.size()) {
        return;
    }
    const qreal current = resolveColumnExtentRel(m_columns.at(m_activeColumnIdx));
    const int idx = cyclePresetIndexByExtent(m_presetColumnWidths, current, 1);
    if (idx < 0) {
        return;
    }
    // Change only this column's width, then scroll the view to keep it visible.
    // Neighbours keep their widths and are pushed aside / off-screen.
    const qreal oldViewOffset = viewOffset();
    const bool wasPinned = m_columns.at(m_activeColumnIdx).width.pinned;
    m_columns[m_activeColumnIdx].width = EmergeColumnWidth{};
    m_columns[m_activeColumnIdx].width.proportion = m_presetColumnWidths.at(idx);
    m_columns[m_activeColumnIdx].width.pinned = wasPinned;
    // A lone unpinned column always fills the viewport, so cycling it is a
    // visual no-op; flagging it would wrongly keep it full once a second column
    // opens. A pinned (app-rule) column keeps a real width, so it is flagged.
    if (wasPinned || m_columns.size() > 1) {
        m_columns[m_activeColumnIdx].width.userSet = true;
    }
    m_viewAnchor = focusAnchorFor(m_activeColumnIdx, m_activeColumnIdx, oldViewOffset);
    reflow();
}

void EmergeLayoutEngine::adjustColumnRatio(qreal delta)
{
    if (m_activeColumnIdx < 0 || m_activeColumnIdx >= m_columns.size() || qFuzzyIsNull(delta)) {
        return;
    }
    const qreal current = resolveColumnExtentRel(m_columns.at(m_activeColumnIdx));
    const qreal oldViewOffset = viewOffset();
    const bool wasPinned = m_columns.at(m_activeColumnIdx).width.pinned;
    m_columns[m_activeColumnIdx].width = EmergeColumnWidth{};
    m_columns[m_activeColumnIdx].width.proportion = qBound(MinColumnExtent, current + delta, qreal(1.0));
    m_columns[m_activeColumnIdx].width.pinned = wasPinned;
    if (wasPinned || m_columns.size() > 1) {
        m_columns[m_activeColumnIdx].width.userSet = true;
    }
    m_viewAnchor = focusAnchorFor(m_activeColumnIdx, m_activeColumnIdx, oldViewOffset);
    reflow();
}

void EmergeLayoutEngine::adjustTileSize(Window *window, qreal weightDelta, Qt::Orientation axis)
{
    if (qFuzzyIsNull(weightDelta)) {
        return;
    }
    const int c = columnOfWindow(window);
    const int ti = tileIndexOfWindow(window);
    if (c < 0 || ti < 0) {
        return;
    }

    const bool crossAxis = (m_axis == Qt::Horizontal) ? (axis == Qt::Vertical) : (axis == Qt::Horizontal);
    if (!crossAxis) {
        // Main axis: the drag changes this column's width. The view scrolls to
        // keep it visible; neighbouring columns are not resized.
        const qreal current = resolveColumnExtentRel(m_columns.at(c));
        const qreal oldViewOffset = viewOffset();
        const bool wasPinned = m_columns.at(c).width.pinned;
        m_columns[c].width = EmergeColumnWidth{};
        m_columns[c].width.proportion = qBound(MinColumnExtent, current + weightDelta, qreal(1.0));
        m_columns[c].width.pinned = wasPinned;
        if (wasPinned || m_columns.size() > 1) {
            m_columns[c].width.userSet = true;
        }
        m_activeColumnIdx = c;
        m_viewAnchor = focusAnchorFor(c, c, oldViewOffset);
        reflow();
        return;
    }

    EmergeColumn &col = m_columns[c];
    const int n = col.tiles.size();
    if (n < 2) {
        return;
    }
    const int neighbour = (ti < n - 1) ? ti + 1 : ti - 1;
    if (neighbour < 0 || neighbour >= n) {
        return;
    }

    EmergeWindowHeight &self = col.tiles[ti].height;
    EmergeWindowHeight &other = col.tiles[neighbour].height;
    const qreal selfWeight = (self.kind == EmergeHeightKind::Auto) ? self.weight : DefaultWeight;
    const qreal otherWeight = (other.kind == EmergeHeightKind::Auto) ? other.weight : DefaultWeight;

    const qreal newSelf = std::clamp(selfWeight + weightDelta, MinWeight, MaxWeight);
    const qreal actualDelta = newSelf - selfWeight;
    const qreal newOther = std::clamp(otherWeight - actualDelta, MinWeight, MaxWeight);
    if (qFuzzyCompare(newSelf, selfWeight) && qFuzzyCompare(newOther, otherWeight)) {
        return;
    }

    // A resize settles a concrete height, so an explicit resize drops any
    // Fixed/Preset intent in favour of the new Auto weight.
    self.kind = EmergeHeightKind::Auto;
    self.weight = newSelf;
    other.kind = EmergeHeightKind::Auto;
    other.weight = newOther;
    reflow();
}

void EmergeLayoutEngine::interactiveResizeEnded(Window *window, const RectF &before, const RectF &after,
                                                Qt::Edge edge, const QSizeF &outputSize)
{
    if (!window || before.size() == QSizeF() || after.size() == QSizeF() || outputSize.width() <= 0.0
        || outputSize.height() <= 0.0) {
        return;
    }
    const int c = columnOfWindow(window);
    if (c < 0) {
        return;
    }

    const bool horizontal = m_axis == Qt::Horizontal;
    const bool mainAxisEdge = horizontal ? (edge == Qt::LeftEdge || edge == Qt::RightEdge)
                                         : (edge == Qt::TopEdge || edge == Qt::BottomEdge);
    if (!mainAxisEdge) {
        // Cross axis: the per-tile height stack handles it.
        LayoutEngine::interactiveResizeEnded(window, before, after, edge, outputSize);
        return;
    }

    // Main axis: the drag moves the divider between this column and its
    // neighbour on the dragged side. Trading the width between just those two
    // keeps the strip's total extent constant, so a drag never reveals an
    // off-screen column (which would show three at once) and never leaves empty
    // space.
    qreal delta = 0.0;
    if (horizontal) {
        delta = (edge == Qt::RightEdge) ? (after.right() - before.right()) / outputSize.width()
                                        : (before.left() - after.left()) / outputSize.width();
    } else {
        delta = (edge == Qt::BottomEdge) ? (after.bottom() - before.bottom()) / outputSize.height()
                                         : (before.top() - after.top()) / outputSize.height();
    }
    if (qFuzzyIsNull(delta)) {
        return;
    }

    const bool leadingDrag = horizontal ? (edge == Qt::LeftEdge) : (edge == Qt::TopEdge);
    int neighbour = leadingDrag ? (c - 1) : (c + 1);
    if (neighbour < 0 || neighbour >= m_columns.size()) {
        neighbour = leadingDrag ? (c + 1) : (c - 1);
    }
    if (neighbour < 0 || neighbour >= m_columns.size()) {
        // A lone unpinned column simply fills the viewport. A pinned (app-rule)
        // column can still be dragged, which takes manual ownership of its width.
        if (!m_columns.at(c).width.pinned) {
            reflow();
            return;
        }
        const qreal loneOld = resolveColumnExtentRel(m_columns.at(c));
        const qreal loneViewOffset = viewOffset();
        m_activeColumnIdx = c;
        m_columns[c].width = EmergeColumnWidth{};
        m_columns[c].width.proportion = qBound(MinColumnExtent, loneOld + delta, qreal(1.0));
        m_columns[c].width.userSet = true;
        m_columns[c].width.pinned = true;
        m_viewAnchor = focusAnchorFor(c, c, loneViewOffset);
        reflow();
        return;
    }

    const bool selfPinned = m_columns.at(c).width.pinned;
    const bool neighbourPinned = m_columns.at(neighbour).width.pinned;
    const qreal selfOld = resolveColumnExtentRel(m_columns.at(c));
    const qreal neighbourOld = resolveColumnExtentRel(m_columns.at(neighbour));
    const qreal selfTarget = qBound(MinColumnExtent, selfOld + delta, qreal(1.0));
    // The neighbour absorbs the opposite change, clamped so it stays valid; the
    // self width is re-derived from the clamped neighbour so the pair's total is
    // preserved exactly.
    const qreal neighbourTarget = qBound(MinColumnExtent, neighbourOld - (selfTarget - selfOld), qreal(1.0));
    const qreal actual = neighbourOld - neighbourTarget;

    const qreal oldViewOffset = viewOffset();
    m_activeColumnIdx = c;
    m_columns[c].width = EmergeColumnWidth{};
    m_columns[c].width.proportion = selfOld + actual;
    m_columns[c].width.userSet = true;
    m_columns[c].width.pinned = selfPinned;
    m_columns[neighbour].width = EmergeColumnWidth{};
    m_columns[neighbour].width.proportion = neighbourTarget;
    m_columns[neighbour].width.userSet = true;
    m_columns[neighbour].width.pinned = neighbourPinned;
    m_viewAnchor = focusAnchorFor(c, c, oldViewOffset);
    reflow();
}

// ── Relayout ────────────────────────────────────────────────────────────────

void EmergeLayoutEngine::reflow()
{
    if (!m_root) {
        return;
    }
    m_axis = effectiveAxis();
    if (m_columns.isEmpty()) {
        // Still announce: the D-Bus bridge publishes an empty viewport when the
        // last column of the active strip goes away.
        Q_EMIT layoutChanged();
        return;
    }

    // Keep the strip at least one viewport wide so the screen is never empty.
    normalizeWidthsToFill();

    // Clamp the view to the strip so the viewport is always filled: an
    // out-of-range offset (e.g. after a resize or a focus change) would show
    // empty space past the end of the strip.
    const qreal maxViewOffset = qMax(qreal(0), stripExtent() - 1.0);
    const qreal view = qBound(qreal(0), viewOffset(), maxViewOffset);

    // Resolve every column's main-axis placement once, so the off-view columns
    // are known before anything moves and the peek band can be sized first.
    struct PlacedColumn
    {
        int index;
        qreal mainPos;
        qreal extent;
        bool offView;
        bool peek = false;
    };
    QList<PlacedColumn> placed;
    QList<int> offViewColumns;
    qreal cursor = -view;
    for (int ci = 0; ci < m_columns.size(); ++ci) {
        const EmergeColumn &col = m_columns.at(ci);
        const qreal extent = resolveColumnExtentRel(col);
        if (extent <= 0.0 || col.tiles.isEmpty()) {
            continue;
        }
        const bool offView = (cursor + extent <= 0.0) || (cursor >= 1.0);
        placed.append({ci, cursor, extent, offView});
        if (offView) {
            offViewColumns.append(int(placed.size()) - 1);
        }
        cursor += extent;
    }

    // The bottom peek band only exists when it can be seen: enabled, at least
    // one column fully off-view, and no monitor underneath to bleed onto.
    qreal peekFrac = 0.0;
    const qreal crossPx = outputCrossPx();
    if (m_peekEnabled && !offViewColumns.isEmpty() && m_peekHeightPx > 0 && crossPx > 0.0
        && !outputHasMonitorBelow()) {
        peekFrac = qBound(qreal(0.0), m_peekHeightPx / crossPx, qreal(0.4));
    }

    // Only the nearest MaxPeekedColumns off-view columns peek; the rest stay
    // parked. Slots are then divided evenly rather than by column width, so a
    // wide column and a narrow one peek at the same size.
    int peeked = 0;
    if (peekFrac > 0.0) {
        const auto distanceFromViewport = [&placed](int i) {
            const PlacedColumn &pc = placed.at(i);
            return (pc.mainPos + pc.extent <= 0.0) ? -(pc.mainPos + pc.extent) : (pc.mainPos - 1.0);
        };
        if (offViewColumns.size() > MaxPeekedColumns) {
            std::sort(offViewColumns.begin(), offViewColumns.end(), [&distanceFromViewport](int a, int b) {
                return distanceFromViewport(a) < distanceFromViewport(b);
            });
            offViewColumns = offViewColumns.mid(0, MaxPeekedColumns);
        }
        for (int i : offViewColumns) {
            placed[i].peek = true;
        }
        peeked = int(offViewColumns.size());
    }
    const qreal crossScale = 1.0 - peekFrac;

    // Where parked tiles land, computed once per pass.
    const int unionBottom = unionBottomPx();
    qreal peekCursor = 0.0;

    for (const PlacedColumn &pc : placed) {
        EmergeColumn &col = m_columns[pc.index];

        if (pc.offView) {
            if (pc.peek && peeked > 0) {
                // Peeked columns share the band evenly, in strip order, so a
                // column on the left stays left and one on the right stays
                // right; windowGeometry's half-gaps separate the slots.
                const qreal slot = 1.0 / qreal(peeked);
                peekColumn(pc.index, peekCursor, slot, peekFrac, unionBottom);
                peekCursor += slot;
            } else {
                for (const EmergeTile &tile : col.tiles) {
                    parkTile(tile.leaf, pc.extent, 1.0, unionBottom);
                }
            }
            continue;
        }

        if (col.display == EmergeColumnDisplay::Tabbed) {
            // Only the active tile occupies the column; the rest are parked so
            // they render nowhere.
            const int active = qBound(0, col.activeTileIdx, int(col.tiles.size()) - 1);
            for (int ti = 0; ti < col.tiles.size(); ++ti) {
                CustomTile *leaf = col.tiles.at(ti).leaf;
                if (!leaf) {
                    continue;
                }
                if (ti == active) {
                    setTileRect(leaf, pc.mainPos, 0.0, pc.extent, crossScale, unionBottom);
                } else {
                    parkTile(leaf, pc.extent, crossScale, unionBottom);
                }
            }
        } else {
            QList<qreal> heights;
            resolveCrossExtents(col, heights);
            qreal cross = 0.0;
            for (int ti = 0; ti < col.tiles.size(); ++ti) {
                CustomTile *leaf = col.tiles.at(ti).leaf;
                const qreal height = heights.value(ti, 1.0) * crossScale;
                setTileRect(leaf, pc.mainPos, cross, pc.extent, height, unionBottom);
                cross += height;
            }
        }
    }

    Q_EMIT layoutChanged();
}

// ── Config ──────────────────────────────────────────────────────────────────

void EmergeLayoutEngine::setEmergePresetColumnWidths(const QList<qreal> &presets)
{
    if (presets.isEmpty()) {
        return;
    }
    m_presetColumnWidths = presets;
    reflow();
}

void EmergeLayoutEngine::setEmergePresetWindowHeights(const QList<qreal> &presets)
{
    if (presets.isEmpty()) {
        return;
    }
    m_presetWindowHeights = presets;
    reflow();
}

void EmergeLayoutEngine::setEmergeWindowWidthRules(const QHash<QString, qreal> &rules)
{
    m_appWidthRules = rules;

    bool changed = false;
    for (EmergeColumn &col : m_columns) {
        CustomTile *leaf = col.tiles.isEmpty() ? nullptr : col.tiles.first().leaf;
        const QList<Window *> windows = leaf ? leaf->windows() : QList<Window *>();
        const std::optional<qreal> fraction = windows.isEmpty() ? std::nullopt : appWidthFraction(windows.first());

        if (col.width.ruleOwned) {
            if (!fraction) {
                col.width = EmergeColumnWidth{};
                col.width.proportion = (m_columns.size() == 1) ? 1.0 : DefaultColumnExtent;
                changed = true;
            } else if (col.width.kind != EmergeWidthKind::Proportion
                       || !qFuzzyCompare(col.width.proportion, *fraction)) {
                col.width.kind = EmergeWidthKind::Proportion;
                col.width.proportion = *fraction;
                changed = true;
            }
        } else if (!col.width.userSet && fraction) {
            col.width.kind = EmergeWidthKind::Proportion;
            col.width.proportion = *fraction;
            col.width.userSet = true;
            col.width.pinned = true;
            col.width.ruleOwned = true;
            changed = true;
        }
    }

    if (changed) {
        reflow();
    }
}

void EmergeLayoutEngine::refreshWindowWidthRule(Window *window)
{
    const int c = columnOfWindow(window);
    if (c < 0) {
        return;
    }
    EmergeColumn &col = m_columns[c];
    if (col.tiles.size() != 1 || col.width.userSet || col.width.pinned) {
        return;
    }
    const std::optional<qreal> fraction = appWidthFraction(window);
    if (!fraction) {
        return;
    }
    col.width.kind = EmergeWidthKind::Proportion;
    col.width.proportion = *fraction;
    col.width.userSet = true;
    col.width.pinned = true;
    col.width.ruleOwned = true;
    reflow();
}

void EmergeLayoutEngine::setEmergeAxis(int axis)
{
    m_axisSetting = qBound(0, axis, 2);
    reflow();
}

void EmergeLayoutEngine::setEmergePeek(bool enabled)
{
    if (m_peekEnabled == enabled) {
        return;
    }
    m_peekEnabled = enabled;
    reflow();
}

void EmergeLayoutEngine::setEmergePeekHeightPx(int heightPx)
{
    const int clamped = qBound(0, heightPx, 1000);
    if (m_peekHeightPx == clamped) {
        return;
    }
    m_peekHeightPx = clamped;
    reflow();
}

} // namespace KWin
