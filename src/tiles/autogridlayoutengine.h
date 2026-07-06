/*
    KWin - the KDE window manager
    SPDX-FileCopyrightText: 2026 KWin Tiling Fork

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "layoutengine.h"
#include "tile.h"

#include <QList>
#include <QPointer>

namespace KWin
{

class CustomTile;
class RootTile;
class Window;

/**
 * AutoGrid layout.
 *
 * A progressive grid: windows are placed in the smallest near-square grid
 * frame that holds them (2x2, then 3x2, 3x3, 4x3, ...). While N is below
 * the frame's cell count, the first window absorbs the unused cells by
 * spanning the top of the left column; once N reaches a perfect grid every
 * cell equalizes.
 *
 * Layout cases by number of tiled windows N:
 *   N == 1   full area
 *   N == 2   left half / right half
 *   N == 3   2x2 frame; W1 spans the full left column
 *   N == 4   perfect 2x2, ordered clockwise TL / TR / BR / BL (an
 *            intentional break from row-major, kept for continuity with the
 *            original layout)
 *   N == 5   3x2 frame; W1 spans the full left column
 *   N == 6   perfect 3x2, row-major
 *   N == 7   3x3 frame; W1 spans the full left column
 *   N == 8   3x3 frame; W1 spans the top 2 rows of the left column
 *   N == 9   perfect 3x3, row-major
 *   ...and so on: the frame is cols = ceil(sqrt(N)) by rows = ceil(N/cols).
 *
 * Tiles own their geometry: there is no per-tile resize, and a drag-move
 * onto another tile swaps the two windows' grid slots.
 *
 * Ported from the autoGrid layout in the Kwilt KWin script.
 */
class KWIN_EXPORT AutoGridLayoutEngine : public LayoutEngine
{
    Q_OBJECT

public:
    explicit AutoGridLayoutEngine(QObject *parent = nullptr);
    ~AutoGridLayoutEngine() override;

    LayoutKind layoutKind() const override { return LayoutKind::AutoGrid; }
    void attach(RootTile *root) override;
    void addWindow(Window *window) override;
    void removeWindow(Window *window) override;
    void moveWindow(Window *window, int delta) override;
    void beginMoveWindow(Window *window) override;
    bool endMoveWindow(Window *window, Window *target) override;
    void cancelMoveWindow(Window *window) override;
    void reflow() override;
    void pruneEmptyLeaves() override;

    QList<Window *> windows() const override;
    Window *primaryWindow() const override;
    Window *windowInDirection(Window *from, FocusDirection direction) const override;

    /**
     * Returns the relative (0-1) geometry of each of the @p count grid
     * slots, in layout order. Exposed for the autotests.
     */
    static QList<RectF> gridGeometries(int count);

private:
    int indexOfWindow(Window *window) const;

    QPointer<RootTile> m_root;
    QList<QPointer<CustomTile>> m_leaves;

    // Source leaf remembered during an interactive drag-move.
    QPointer<CustomTile> m_moveSourceLeaf;
    int m_moveSourceIndex = -1;
};

} // namespace KWin
