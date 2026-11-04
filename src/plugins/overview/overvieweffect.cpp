/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "overvieweffect.h"
#include "effect/effecthandler.h"
#include "input_event.h"
#include "overviewconfig.h"
#include "scene/item.h"
#include "scene/windowitem.h"
#include "scene/workspacescene.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#include <KGlobalAccel>
#include <KLocalizedString>

#include <QAction>
#include <QDebug>
#include <QQuickItem>
#include <QTimer>

#include <cmath>
#include <limits>

using namespace std::chrono_literals;

namespace KWin
{
OverviewEffect::OverviewEffect()
    // manages the transition between inactive -> overview
    : m_overviewState(new EffectTogglableState(this))
    // manages the transition between overview -> grid
    , m_transitionState(new EffectTogglableState(this))
    // manages the transition between inactive -> overview
    , m_gridState(new EffectTogglableState(this))
    , m_border(new EffectTogglableTouchBorder(m_overviewState))
    , m_gridBorder(new EffectTogglableTouchBorder(m_gridState))
    , m_shutdownTimer(new QTimer(this))
{
    setAutoSizeRootItem(false);

    auto gesture = new EffectTogglableGesture(m_overviewState);
    gesture->addTouchpadSwipeGesture(SwipeDirection::Up, 4);
    gesture->addTouchscreenSwipeGesture(SwipeDirection::Up, 3);

    auto transitionGesture = new EffectTogglableGesture(m_transitionState);
    transitionGesture->addTouchpadSwipeGesture(SwipeDirection::Up, 4);
    transitionGesture->addTouchscreenSwipeGesture(SwipeDirection::Up, 3);
    m_transitionState->stop();

    auto gridGesture = new EffectTogglableGesture(m_gridState);
    gridGesture->addTouchpadSwipeGesture(SwipeDirection::Down, 4);
    gridGesture->addTouchscreenSwipeGesture(SwipeDirection::Down, 3);

    connect(m_overviewState, &EffectTogglableState::inProgressChanged, this, &OverviewEffect::overviewGestureInProgressChanged);
    connect(m_overviewState, &EffectTogglableState::partialActivationFactorChanged, this, &OverviewEffect::overviewPartialActivationFactorChanged);

    connect(m_overviewState, &EffectTogglableState::statusChanged, this, [this](EffectTogglableState::Status status) {
        if (status == EffectTogglableState::Status::Activating || status == EffectTogglableState::Status::Active) {
            setRunning(true);
            m_gridState->stop();
        }
        if (status == EffectTogglableState::Status::Active) {
            m_transitionState->deactivate();
        }
        if (status == EffectTogglableState::Status::Inactive || status == EffectTogglableState::Status::Deactivating) {
            m_transitionState->stop();
        }
        if (status == EffectTogglableState::Status::Inactive) {
            m_gridState->deactivate();
            deactivate();
        }
    });

    connect(m_transitionState, &EffectTogglableState::statusChanged, this, [this](EffectTogglableState::Status status) {
        if (status == EffectTogglableState::Status::Activating || status == EffectTogglableState::Status::Active) {
            m_overviewState->stop();
        }
        if (status == EffectTogglableState::Status::Inactive) {
            m_overviewState->activate();
        }
        if (status == EffectTogglableState::Status::Active) {
            m_gridState->activate();
        }
        if (status == EffectTogglableState::Status::Inactive || status == EffectTogglableState::Status::Deactivating) {
            m_gridState->stop();
        }
    });

    connect(m_gridState, &EffectTogglableState::statusChanged, this, [this](EffectTogglableState::Status status) {
        if (status == EffectTogglableState::Status::Activating || status == EffectTogglableState::Status::Active) {
            setRunning(true);
            m_overviewState->stop();
        }
        if (status == EffectTogglableState::Status::Inactive) {
            m_overviewState->deactivate();
            deactivate();
        }
        if (status == EffectTogglableState::Status::Active) {
            m_transitionState->activate();
        }
        if (status == EffectTogglableState::Status::Inactive || status == EffectTogglableState::Status::Deactivating) {
            m_transitionState->stop();
        }
    });

    connect(m_transitionState, &EffectTogglableState::inProgressChanged, this, &OverviewEffect::transitionGestureInProgressChanged);
    connect(m_transitionState, &EffectTogglableState::partialActivationFactorChanged, this, &OverviewEffect::transitionPartialActivationFactorChanged);

    connect(m_gridState, &EffectTogglableState::inProgressChanged, this, &OverviewEffect::gridGestureInProgressChanged);
    connect(m_gridState, &EffectTogglableState::partialActivationFactorChanged, this, &OverviewEffect::gridPartialActivationFactorChanged);

    connect(this, &QuickSceneEffect::activated, this, &OverviewEffect::raiseAboveLayers);
    connect(this, &QuickSceneEffect::deactivated, this, &OverviewEffect::restoreAboveLayers);

    connect(effects, &EffectsHandler::desktopChanging, this, [this](VirtualDesktop *old, QPointF desktopOffset, EffectWindow *, LogicalOutput *output) {
        m_screenDesktopOffsets.insertOrAssign(output, desktopOffset);
        Q_EMIT desktopOffsetChanged(output);
    });
    connect(effects, &EffectsHandler::desktopChanged, this, [this](KWin::VirtualDesktop *, KWin::VirtualDesktop *, EffectWindow *, KWin::LogicalOutput *output) {
        m_screenDesktopOffsets.insertOrAssign(output, QPointF(0, 0));
        Q_EMIT desktopOffsetChanged(output);
    });
    connect(effects, &EffectsHandler::desktopChangingCancelled, this, [this]() {
        m_screenDesktopOffsets.clear();
        const auto screens = effects->screens();

        for (LogicalOutput *output : screens) {
            Q_EMIT desktopOffsetChanged(output);
        }
    });
    connect(effects, &EffectsHandler::screenRemoved, this, [this](KWin::LogicalOutput *output) {
        m_screenDesktopOffsets.remove(output);
        Q_EMIT desktopOffsetChanged(output);
    });

    m_shutdownTimer->setSingleShot(true);
    connect(m_shutdownTimer, &QTimer::timeout, this, [this]() {
        if (m_overviewState->status() == EffectTogglableState::Status::Inactive) {
            setRunning(false);
        }
    });

    auto cycleAction = new QAction(this);
    connect(cycleAction, &QAction::triggered, this, &OverviewEffect::cycle);
    cycleAction->setObjectName(QStringLiteral("Cycle Overview"));
    cycleAction->setText(i18nc("@action Grid View and Overview are the name of KWin effects", "Cycle through Overview and Grid View"));
    KGlobalAccel::setGlobalShortcut(cycleAction, QList<QKeySequence>{});

    auto reverseCycleAction = new QAction(this);
    connect(reverseCycleAction, &QAction::triggered, this, &OverviewEffect::reverseCycle);
    reverseCycleAction->setObjectName(QStringLiteral("Cycle Overview Opposite"));
    reverseCycleAction->setText(i18nc("@action Grid View and Overview are the name of KWin effects", "Cycle through Grid View and Overview"));
    KGlobalAccel::setGlobalShortcut(reverseCycleAction, QList<QKeySequence>{});

    KGlobalAccel::setInverseShortcutActions(cycleAction, reverseCycleAction);

    auto overviewAction = m_overviewState->toggleAction();
    overviewAction->setObjectName(QStringLiteral("Overview"));
    overviewAction->setText(i18nc("@action Overview is the name of a Kwin effect", "Toggle Overview"));
    overviewAction->setAutoRepeat(false);
    KGlobalAccel::setGlobalShortcut(overviewAction, QKeySequence(Qt::META | Qt::Key_O));

    auto gridAction = m_gridState->toggleAction();
    gridAction->setObjectName(QStringLiteral("Grid View"));
    gridAction->setText(i18nc("@action Grid view is the name of a Kwin effect", "Toggle Grid View"));
    gridAction->setAutoRepeat(false);
    KGlobalAccel::setGlobalShortcut(gridAction, QList<QKeySequence>{});

    connect(effects, &EffectsHandler::screenAboutToLock, this, &OverviewEffect::deactivateNow);

    OverviewConfig::instance(effects->config());
    reconfigure(ReconfigureAll);

    auto delegate = new QQmlComponent(effects->qmlEngine(), this);
    connect(delegate, &QQmlComponent::statusChanged, this, [delegate]() {
        if (delegate->isError()) {
            qWarning() << "Failed to load overview:" << delegate->errorString();
        }
    });
    delegate->loadFromModule(QStringLiteral("org.kineticwe.kwin.overview"), QStringLiteral("Main"), QQmlComponent::Asynchronous);
    setDelegate(delegate);
}

OverviewEffect::~OverviewEffect()
{
}

void OverviewEffect::reconfigure(ReconfigureFlags)
{
    OverviewConfig::self()->read();
    setAnimationDuration(animationTime(300ms).count());

    for (const ElectricBorder &border : std::as_const(m_borderActivate)) {
        effects->unreserveElectricBorder(border, this);
    }
    for (const ElectricBorder &border : std::as_const(m_gridBorderActivate)) {
        effects->unreserveElectricBorder(border, this);
    }

    m_borderActivate.clear();
    m_gridBorderActivate.clear();

    const QList<int> activateBorders = OverviewConfig::borderActivate();
    for (const int &border : activateBorders) {
        m_borderActivate.append(ElectricBorder(border));
        effects->reserveElectricBorder(ElectricBorder(border), this);
    }

    const QList<int> gridActivateBorders = OverviewConfig::gridBorderActivate();
    for (const int &border : gridActivateBorders) {
        m_gridBorderActivate.append(ElectricBorder(border));
        effects->reserveElectricBorder(ElectricBorder(border), this);
    }

    m_border->setBorders(OverviewConfig::touchBorderActivate());
    m_gridBorder->setBorders(OverviewConfig::gridTouchBorderActivate());
}

int OverviewEffect::animationDuration() const
{
    return m_animationDuration;
}

void OverviewEffect::setAnimationDuration(int duration)
{
    if (m_animationDuration != duration) {
        m_animationDuration = duration;
        Q_EMIT animationDurationChanged();
    }
}

Rect OverviewEffect::viewInputGeometry(LogicalOutput *screen) const
{
    return effects->clientArea(MaximizeArea, screen).toRect();
}

bool OverviewEffect::acceptsInputAt(LogicalOutput *screen, const QPoint &pos) const
{
    if (!viewInputGeometry(screen).contains(pos)) {
        return false;
    }
    for (Window *window : workspace()->windows()) {
        if (window->layer() >= AboveLayer && window->frameGeometry().contains(QPointF(pos))) {
            return false;
        }
    }
    return true;
}

void OverviewEffect::raiseWindow(Window *window)
{
    if (!window || window->layer() < AboveLayer) {
        return;
    }
    WindowItem *item = window->windowItem();
    if (!item || item->parentItem() == effects->scene()->overlayItem()) {
        return;
    }
    if (m_raisedWindows.contains(window)) {
        return;
    }

    const bool wasElevated = item->isElevated();
    m_raisedWindows.insert(window, RaisedWindow{
        .oldParent = item->parentItem(),
        .wasElevated = wasElevated,
    });

    connect(window, &QObject::destroyed, this, [this, window] {
        m_raisedWindows.remove(window);
    });

    item->setParentItem(effects->scene()->overlayItem());
    if (!wasElevated) {
        item->elevate();
    }
}

void OverviewEffect::raiseAboveLayers()
{
    if (m_overviewRaised) {
        return;
    }
    m_overviewRaised = true;

    m_oldCursorZ = effects->scene()->cursorItem()->z();
    effects->scene()->cursorItem()->setZ(std::numeric_limits<int>::max());

    const QList<Window *> windows = workspace()->windows();
    for (Window *window : windows) {
        raiseWindow(window);
    }

    m_windowAddedConnection = connect(workspace(), &Workspace::windowAdded, this, [this](Window *window) {
        raiseWindow(window);
    });
}

void OverviewEffect::restoreAboveLayers()
{
    if (!m_overviewRaised) {
        return;
    }
    m_overviewRaised = false;

    disconnect(m_windowAddedConnection);

    const QList<Window *> windows = m_raisedWindows.keys();
    for (Window *window : windows) {
        const auto it = m_raisedWindows.constFind(window);
        if (it == m_raisedWindows.constEnd()) {
            continue;
        }
        WindowItem *item = window->windowItem();
        if (!item) {
            continue;
        }
        item->setParentItem(it->oldParent);
        if (!it->wasElevated) {
            item->deelevate();
        }
    }
    m_raisedWindows.clear();

    effects->scene()->cursorItem()->setZ(m_oldCursorZ);
}

qreal OverviewEffect::overviewPartialActivationFactor() const
{
    return m_overviewState->partialActivationFactor();
}

bool OverviewEffect::overviewGestureInProgress() const
{
    return m_overviewState->inProgress();
}

qreal OverviewEffect::transitionPartialActivationFactor() const
{
    return m_transitionState->partialActivationFactor();
}

bool OverviewEffect::transitionGestureInProgress() const
{
    return m_transitionState->inProgress();
}

qreal OverviewEffect::gridPartialActivationFactor() const
{
    return m_gridState->partialActivationFactor();
}

bool OverviewEffect::gridGestureInProgress() const
{
    return m_gridState->inProgress();
}

bool OverviewEffect::windowDragActive() const
{
    return m_windowDragActive;
}

void OverviewEffect::setWindowDragActive(bool active)
{
    if (m_windowDragActive == active) {
        return;
    }
    m_windowDragActive = active;
    if (!active) {
        m_dragWheelAccumulator = 0;
    }
    Q_EMIT windowDragActiveChanged();
}

QPointF OverviewEffect::desktopOffsetForScreen(LogicalOutput *screen) const
{
    return m_screenDesktopOffsets.value(screen, QPointF(0, 0));
}

bool OverviewEffect::ignoreMinimized() const
{
    return OverviewConfig::ignoreMinimized();
}

int OverviewEffect::requestedEffectChainPosition() const
{
    return 70;
}

bool OverviewEffect::borderActivated(ElectricBorder border)
{
    if (m_borderActivate.contains(border)) {
        m_overviewState->toggle();
        return true;
    } else if (m_gridBorderActivate.contains(border)) {
        m_gridState->toggle();
        return true;
    }
    return false;
}

void OverviewEffect::pointerAxis(PointerAxisEvent *event)
{
    if (m_gridState->status() != EffectTogglableState::Status::Active || !m_windowDragActive || event->modifiers != Qt::NoModifier) {
        QuickSceneEffect::pointerAxis(event);
        return;
    }

    qreal delta = event->deltaV120 != 0 ? event->deltaV120 / 120.0 : event->delta / 15.0;
    if (event->inverted) {
        delta = -delta;
    }
    if (delta == 0) {
        QuickSceneEffect::pointerAxis(event);
        return;
    }

    if (std::signbit(delta) != std::signbit(m_dragWheelAccumulator)) {
        m_dragWheelAccumulator = 0;
    }
    m_dragWheelAccumulator += delta;

    LogicalOutput *output = workspace()->outputAt(event->position);
    if (!output) {
        output = workspace()->activeOutput();
    }

    const bool wrap = VirtualDesktopManager::self()->isNavigationWrappingAround();
    while (std::abs(m_dragWheelAccumulator) >= 1.0) {
        const bool next = m_dragWheelAccumulator > 0;
        m_dragWheelAccumulator += next ? -1.0 : 1.0;

        VirtualDesktop *current = VirtualDesktopManager::self()->currentDesktop(output);
        VirtualDesktop *target = VirtualDesktopManager::self()->inDirection(current, next ? VirtualDesktopManager::Direction::Next : VirtualDesktopManager::Direction::Previous, wrap);
        if (target == current) {
            m_dragWheelAccumulator = 0;
            break;
        }
        VirtualDesktopManager::self()->setCurrent(target, output);
    }
}

void OverviewEffect::activate()
{
    if (effects->isScreenLocked()) {
        return;
    }

    setWindowDragActive(false);
    m_dragWheelAccumulator = 0;

    updateViewGeometries();
    m_overviewState->activate();
    m_shutdownTimer->stop();
}

void OverviewEffect::deactivate()
{
    const auto screens = effects->screens();
    setWindowDragActive(false);
    m_dragWheelAccumulator = 0;
    m_shutdownTimer->start(animationDuration());
    m_overviewState->deactivate();
}

void OverviewEffect::deactivateNow()
{
    setWindowDragActive(false);
    m_dragWheelAccumulator = 0;
    m_shutdownTimer->stop();
    m_overviewState->deactivate();
    setRunning(false);
}

void OverviewEffect::cycle()
{
    if (m_overviewState->status() == EffectTogglableState::Status::Inactive) {
        m_overviewState->activate();
    } else if (m_transitionState->status() == EffectTogglableState::Status::Inactive) {
        m_transitionState->activate();
    } else if (m_gridState->status() == EffectTogglableState::Status::Active) {
        m_overviewState->deactivate();
    }
}

void OverviewEffect::reverseCycle()
{
    if (m_overviewState->status() == EffectTogglableState::Status::Active) {
        m_overviewState->deactivate();
    } else if (m_transitionState->status() == EffectTogglableState::Status::Active) {
        m_transitionState->deactivate();
    } else if (m_gridState->status() == EffectTogglableState::Status::Inactive) {
        m_gridState->activate();
    }
}

} // namespace KWin

#include "moc_overvieweffect.cpp"
