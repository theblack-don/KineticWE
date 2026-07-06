/*
    KWin - the KDE window manager
    SPDX-FileCopyrightText: 2026 KWin Tiling Fork

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "autogridlayoutengine.h"
#include "customtile.h"
#include "window.h"

#include <cmath>

namespace KWin
{

AutoGridLayoutEngine::AutoGridLayoutEngine(QObject *parent)
    : LayoutEngine(parent)
{
}

AutoGridLayoutEngine::~AutoGridLayoutEngine() = default;

QList<RectF> AutoGridLayoutEngine::gridGeometries(int count)
{
    QList<RectF> rects;
    if (count <= 0) {
        return rects;
    }
    rects.reserve(count);

    if (count == 1) {
        rects.append(RectF(0, 0, 1, 1));
        return rects;
    }

    // N=4: perfect 2x2 ordered clockwise TL / TR / BR / BL. Intentional
    // break from the row-major fill used by every other perfect grid.
    if (count == 4) {
        rects.append(RectF(0, 0, 0.5, 0.5));
        rects.append(RectF(0.5, 0, 0.5, 0.5));
        rects.append(RectF(0.5, 0.5, 0.5, 0.5));
        rects.append(RectF(0, 0.5, 0.5, 0.5));
        return rects;
    }

    // Smallest near-square frame that holds count cells. This reproduces the
    // 2x1 / 2x2 / 3x2 / 3x3 / 4x3 progression and keeps going past it.
    const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<qreal>(count))));
    const int rows = (count + cols - 1) / cols;

    // The first window absorbs the frame's unused cells by spanning that many
    // extra rows at the top of the left column. deficit == rows - 1 means it
    // spans the full left column; deficit == 0 is a perfect grid.
    const int deficit = std::min(cols * rows - count, rows - 1);

    auto cell = [cols, rows](int col, int row, int rowSpan) {
        const qreal x = static_cast<qreal>(col) / cols;
        const qreal y = static_cast<qreal>(row) / rows;
        return RectF(x, y,
                     static_cast<qreal>(col + 1) / cols - x,
                     static_cast<qreal>(row + rowSpan) / rows - y);
    };

    if (deficit > 0) {
        rects.append(cell(0, 0, deficit + 1));
    }
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            if (deficit > 0 && col == 0 && row <= deficit) {
                continue; // covered by the first window's span
            }
            rects.append(cell(col, row, 1));
        }
    }
    return rects;
}

void AutoGridLayoutEngine::attach(RootTile *root)
{
    m_root = root;
    if (!m_root) {
        return;
    }

    // Remove any pre-existing children so the engine owns the root completely.
    const QList<Tile *> existingChildren = m_root->childTiles();
    for (Tile *child : existingChildren) {
        if (CustomTile *custom = qobject_cast<CustomTile *>(child)) {
            m_root->destroyChild(custom);
        }
    }
    m_root->setLayoutDirection(Tile::LayoutDirection::Floating);
    m_root->setRelativeGeometry(RectF(0, 0, 1, 1));
}

void AutoGridLayoutEngine::addWindow(Window *window)
{
    if (!m_root) {
        return;
    }

    // New windows are appended at the end of the grid order; reflow assigns
    // the actual slot geometry.
    CustomTile *leaf = m_root->createChildAt(RectF(0, 0, 1, 1), Tile::LayoutDirection::Floating, m_leaves.count());
    m_leaves.append(leaf);
    if (!leaf->manage(window)) {
        qWarning() << "AutoGridLayoutEngine: failed to manage window" << window->caption() << "in leaf";
        m_root->destroyChild(leaf);
        m_leaves.removeLast();
        return;
    }

    reflow();
}

void AutoGridLayoutEngine::removeWindow(Window *window)
{
    const int idx = indexOfWindow(window);
    if (idx < 0) {
        return;
    }

    CustomTile *leaf = m_leaves.takeAt(idx);
    leaf->unmanage(window);
    m_root->destroyChild(leaf);

    reflow();
}

void AutoGridLayoutEngine::pruneEmptyLeaves()
{
    if (!m_root) {
        return;
    }

    bool changed = false;
    for (int i = m_leaves.count() - 1; i >= 0; --i) {
        const auto &leaf = m_leaves[i];
        if (leaf && !leaf->windows().isEmpty()) {
            continue;
        }
        if (leaf) {
            m_root->destroyChild(leaf);
        }
        m_leaves.removeAt(i);
        changed = true;
    }

    if (changed) {
        reflow();
    }
}

void AutoGridLayoutEngine::moveWindow(Window *window, int delta)
{
    const int idx = indexOfWindow(window);
    if (idx < 0) {
        return;
    }

    const int newIdx = qBound(0, idx + delta, m_leaves.count() - 1);
    if (newIdx == idx) {
        return;
    }

    m_leaves.move(idx, newIdx);
    reflow();
}

void AutoGridLayoutEngine::beginMoveWindow(Window *window)
{
    m_moveSourceIndex = indexOfWindow(window);
    m_moveSourceLeaf = (m_moveSourceIndex >= 0 && m_moveSourceIndex < m_leaves.count())
        ? m_leaves[m_moveSourceIndex] : nullptr;
}

bool AutoGridLayoutEngine::endMoveWindow(Window *window, Window *target)
{
    if (!window || !m_moveSourceLeaf || m_moveSourceIndex < 0) {
        return false;
    }

    if (target) {
        // Swap the two windows' grid slots; everything else stays in place.
        const int targetIdx = indexOfWindow(target);
        if (targetIdx >= 0 && targetIdx != m_moveSourceIndex) {
            m_leaves.swapItemsAt(m_moveSourceIndex, targetIdx);
            reflow();
        }
    }
    // If no target, the window snaps back to its slot (reflow is a no-op on
    // order, but restores the dragged geometry).
    reflow();

    m_moveSourceLeaf = nullptr;
    m_moveSourceIndex = -1;
    return true;
}

void AutoGridLayoutEngine::cancelMoveWindow(Window *window)
{
    Q_UNUSED(window)
    m_moveSourceLeaf = nullptr;
    m_moveSourceIndex = -1;
}

void AutoGridLayoutEngine::reflow()
{
    if (!m_root) {
        return;
    }

    const QList<RectF> rects = gridGeometries(m_leaves.count());
    for (int i = 0; i < m_leaves.count(); ++i) {
        if (m_leaves[i]) {
            m_leaves[i]->setRelativeGeometry(rects[i]);
        }
    }
}

QList<Window *> AutoGridLayoutEngine::windows() const
{
    QList<Window *> result;
    result.reserve(m_leaves.count());
    for (const auto &leaf : m_leaves) {
        if (leaf && !leaf->windows().isEmpty()) {
            result.append(leaf->windows().first());
        }
    }
    return result;
}

Window *AutoGridLayoutEngine::primaryWindow() const
{
    if (m_leaves.isEmpty() || !m_leaves.first()) {
        return nullptr;
    }
    const QList<Window *> ws = m_leaves.first()->windows();
    return ws.isEmpty() ? nullptr : ws.first();
}

Window *AutoGridLayoutEngine::windowInDirection(Window *from, FocusDirection direction) const
{
    if (m_leaves.isEmpty()) {
        return nullptr;
    }

    const int idx = from ? indexOfWindow(from) : -1;
    if (idx < 0) {
        return primaryWindow();
    }

    // Geometric neighbour search over the grid slots: among the leaves that
    // overlap the current one on the perpendicular axis and lie strictly in
    // the requested direction, pick the nearest.
    const QList<RectF> rects = gridGeometries(m_leaves.count());
    const RectF cur = rects[idx];

    int best = -1;
    qreal bestDistance = 0;
    for (int i = 0; i < m_leaves.count(); ++i) {
        if (i == idx || !m_leaves[i]) {
            continue;
        }
        const RectF candidate = rects[i];
        qreal distance = -1;
        switch (direction) {
        case FocusDirection::Left:
            if (candidate.right() <= cur.left() + 0.001
                && candidate.bottom() > cur.top() && candidate.top() < cur.bottom()) {
                distance = cur.left() - candidate.right();
            }
            break;
        case FocusDirection::Right:
            if (candidate.left() >= cur.right() - 0.001
                && candidate.bottom() > cur.top() && candidate.top() < cur.bottom()) {
                distance = candidate.left() - cur.right();
            }
            break;
        case FocusDirection::Up:
            if (candidate.bottom() <= cur.top() + 0.001
                && candidate.right() > cur.left() && candidate.left() < cur.right()) {
                distance = cur.top() - candidate.bottom();
            }
            break;
        case FocusDirection::Down:
            if (candidate.top() >= cur.bottom() - 0.001
                && candidate.right() > cur.left() && candidate.left() < cur.right()) {
                distance = candidate.top() - cur.bottom();
            }
            break;
        }
        if (distance >= -0.001 && (best < 0 || distance < bestDistance)) {
            best = i;
            bestDistance = distance;
        }
    }

    if (best < 0) {
        return nullptr;
    }
    const QList<Window *> ws = m_leaves[best]->windows();
    return ws.isEmpty() ? nullptr : ws.first();
}

int AutoGridLayoutEngine::indexOfWindow(Window *window) const
{
    for (int i = 0; i < m_leaves.count(); ++i) {
        if (m_leaves[i] && m_leaves[i]->windows().contains(window)) {
            return i;
        }
    }
    return -1;
}

} // namespace KWin
