/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/effecttogglablestate.h"
#include "effect/quickeffect.h"

#include <QHash>
#include <QMetaObject>

namespace KWin
{
class VirtualDesktop;
class Window;
class Item;

class OverviewEffect : public QuickSceneEffect
{
    Q_OBJECT
    Q_PROPERTY(int animationDuration READ animationDuration NOTIFY animationDurationChanged)
    Q_PROPERTY(bool ignoreMinimized READ ignoreMinimized NOTIFY ignoreMinimizedChanged)
    Q_PROPERTY(qreal overviewPartialActivationFactor READ overviewPartialActivationFactor NOTIFY overviewPartialActivationFactorChanged)
    // More efficient from a property binding pov rather than checking if partialActivationFactor is strictly between 0 and 1
    Q_PROPERTY(bool overviewGestureInProgress READ overviewGestureInProgress NOTIFY overviewGestureInProgressChanged)
    Q_PROPERTY(qreal transitionPartialActivationFactor READ transitionPartialActivationFactor NOTIFY transitionPartialActivationFactorChanged)
    Q_PROPERTY(bool transitionGestureInProgress READ transitionGestureInProgress NOTIFY transitionGestureInProgressChanged)
    Q_PROPERTY(qreal gridPartialActivationFactor READ gridPartialActivationFactor NOTIFY gridPartialActivationFactorChanged)
    Q_PROPERTY(bool gridGestureInProgress READ gridGestureInProgress NOTIFY gridGestureInProgressChanged)
    Q_PROPERTY(bool windowDragActive READ windowDragActive WRITE setWindowDragActive NOTIFY windowDragActiveChanged)

public:
    OverviewEffect();
    ~OverviewEffect() override;

    bool ignoreMinimized() const;

    int animationDuration() const;
    void setAnimationDuration(int duration);

    qreal overviewPartialActivationFactor() const;
    bool overviewGestureInProgress() const;
    qreal transitionPartialActivationFactor() const;
    bool transitionGestureInProgress() const;
    qreal gridPartialActivationFactor() const;
    bool gridGestureInProgress() const;
    bool windowDragActive() const;
    void setWindowDragActive(bool active);
    QPointF desktopOffset() const;
    Q_INVOKABLE QPointF desktopOffsetForScreen(LogicalOutput *screen) const;

    int requestedEffectChainPosition() const override;
    bool borderActivated(ElectricBorder border) override;
    void pointerAxis(PointerAxisEvent *event) override;
    void reconfigure(ReconfigureFlags flags) override;
    Rect viewInputGeometry(LogicalOutput *screen) const override;
    bool acceptsInputAt(LogicalOutput *screen, const QPoint &pos) const override;

Q_SIGNALS:
    void animationDurationChanged();
    void overviewPartialActivationFactorChanged();
    void overviewGestureInProgressChanged();
    void transitionPartialActivationFactorChanged();
    void transitionGestureInProgressChanged();
    void gridPartialActivationFactorChanged();
    void gridGestureInProgressChanged();
    void windowDragActiveChanged();
    void ignoreMinimizedChanged();
    void desktopOffsetChanged(KWin::LogicalOutput *screen);

public Q_SLOTS:
    void activate();
    void deactivate();

private:
    void deactivateNow();
    void cycle();
    void reverseCycle();

    void raiseAboveLayers();
    void restoreAboveLayers();
    void raiseWindow(Window *window);

    EffectTogglableState *const m_overviewState;
    EffectTogglableState *const m_transitionState;
    EffectTogglableState *const m_gridState;
    EffectTogglableTouchBorder *const m_border;
    EffectTogglableTouchBorder *const m_gridBorder;

    struct RaisedWindow
    {
        Item *oldParent;
        bool wasElevated;
    };
    QHash<Window *, RaisedWindow> m_raisedWindows;
    QMetaObject::Connection m_windowAddedConnection;
    int m_oldCursorZ = 1;
    bool m_overviewRaised = false;
    bool m_windowDragActive = false;
    qreal m_dragWheelAccumulator = 0;

    QTimer *m_shutdownTimer;
    QList<ElectricBorder> m_borderActivate;
    QList<ElectricBorder> m_gridBorderActivate;
    QHash<LogicalOutput *, QPointF> m_screenDesktopOffsets;
    int m_animationDuration = 400;
};

} // namespace KWin
