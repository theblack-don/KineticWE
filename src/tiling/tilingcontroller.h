/*
    KWin - the KDE window manager
    SPDX-FileCopyrightText: 2026 KWin Tiling Fork
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "kwin_export.h"
#include "tilingrules.h"
#include "tilingstate.h"
#include "tiles/layoutengine.h"

#include <KConfigWatcher>
#include <KSharedConfig>

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <memory>

class KConfigGroup;

namespace KWin
{

class LayoutEngine;
class LogicalOutput;
class EmergeLayoutEngine;
class TileManager;
class TilingDBusInterface;
class VirtualDesktop;
class Window;
class Workspace;

/**
 * Singleton controller that manages the native tiling state.
 *
 * It decides which windows tile, reacts to window creation/destruction,
 * and handles keyboard-driven tiling commands.
 */
class KWIN_EXPORT TilingController : public QObject
{
    Q_OBJECT

public:
    explicit TilingController(Workspace *workspace);
    ~TilingController() override;

    /**
     * Called by Workspace when a window is added.
     */
    void onWindowAdded(Window *window);

    /**
     * Called by Workspace when a window is removed.
     */
    void onWindowRemoved(Window *window);

    /**
     * Re-read configuration from kwinrc.
     */
    void reconfigure();

    /**
     * Initialize default layout engines for all existing outputs/desktops.
     */
    void initializeLayouts();

    /**
     * Initialize default layout engines for a newly added output.
     */
    void onOutputAdded(LogicalOutput *output);

    enum class TilingDirection {
        West,
        East,
    };

    // Keyboard actions
    void focusLeft();
    void focusRight();
    void focusUp();
    void focusDown();
    void swapUp();
    void swapDown();
    void swapLeft();
    void swapRight();
    void toggleFloating();
    void promoteToMaster();
    void moveWindowToOutput(TilingDirection direction);
    void moveWindowNext();
    void moveWindowPrevious();

    /**
     * Column-consume action (Columns layout). The active tiled window leaves
     * its column and joins the column to its right; if there is no column to
     * the right it joins the column to its left. No-op otherwise.
     */
    void consume();
    /**
     * Column-expel action (Columns layout). The active tiled window leaves
     * its column and gets its own new column, inserted to the right of its
     * old column (or to the left if its old column is the rightmost).
     * No-op if the window is already alone in its column or the layout is
     * at its column cap.
     */
    void expel();

    /**
     * Emerge layout: cycle the active column through the configured preset
     * widths. No-op for layouts without an Emerge strip.
     */
    void switchPresetColumnWidth();

    /**
     * Per-tile resize actions. Routes to the active window's layout engine
     * and asks it to grow (@p grow == true) or shrink the active leaf along
     * the given @p axis. The delta is m_resizeStep (configurable via kwinrc).
     * No-op if there is no active tiled window or the engine does not
     * support per-tile resize.
     */
    void growActiveTileSize(Qt::Orientation axis);
    void shrinkActiveTileSize(Qt::Orientation axis);

    /**
     * Current per-tile resize step. Exposed for tests, scripts, and the KCM
     * to display. Always within (0, 1].
     */
    qreal resizeStep() const { return m_resizeStep; }

    /**
     * Mouse-driven resize plumbing. Called from onInteractiveMoveResizeStarted
     * when the user grabs a resize handle on a tiled window. Captures the
     * source leaf and the edge being dragged so the corresponding end of
     * endInteractiveResize can apply a weight delta to the engine.
     */
    void beginInteractiveResize(Window *window);
    /**
     * Applies the resize. @p finalGeometry is the window's geometry at the
     * moment the user released the mouse. The pixel delta vs. the
     * pre-resize geometry is translated into a weight delta on the
     * appropriate engine. @p edge is the edge the user grabbed.
     */
    void endInteractiveResize(Window *window, const RectF &finalGeometry, Qt::Edge edge);

    /**
     * Switch the layout of the active monitor's current desktop to @p kind,
     * rebuilding the engine in place and re-adding the windows it was
     * managing so the visual change is immediate.
     */
    void setLayout(LayoutEngine::LayoutKind kind);

    /**
     * Cycle the active monitor's current desktop through the list of layouts
     * currently enabled in kwinrc.
     */
    void cycleLayout();

    /**
     * Flip the active monitor's outer (left/right) gaps between the two
     * [Tiling] GapToggleWide / GapToggleCentered presets by writing a
     * [Tiling][Output <name>] override and applying it live. Bound to the
     * "Tiling Toggle Outer Gaps" shortcut and exposed over D-Bus.
     */
    void toggleOuterGaps();

    /**
     * Toggle float-only tiling for the active output's current desktop. The
     * state is persisted as a [Tiling][DesktopOutput <n>:<out>] "FloatOnly"
     * override and applied live: enabling it floats every tiled window on
     * that desktop and tears down its layout engine; disabling it rebuilds
     * the engine and re-tiles the windows the toggle had floated. Bound to
     * the "Tiling Toggle Float Only Workspace" shortcut and exposed over
     * D-Bus.
     */
    void toggleFloatOnly();

    TilingRules *rules() const { return m_rules.get(); }

    /** Whether native tiling is currently enabled (kwinrc [Tiling] Enabled). */
    bool isEnabled() const { return m_enabled; }

    /**
     * The layout kind in effect for (@p output, @p desktop): the live
     * engine's kind when one is attached (reflecting runtime layout
     * switches), otherwise the configured kind from the kwinrc fallback
     * chain (per-desktop+output -> per-output -> global default).
     */
    LayoutEngine::LayoutKind layoutKindFor(LogicalOutput *output, VirtualDesktop *desktop) const;

    /**
     * Kind string of the layout in effect for the active output's current
     * desktop, or an empty string when tiling is disabled or there is no
     * active output/desktop. Exposed for the D-Bus interface.
     */
    QString currentLayoutKindString() const;

    /**
     * The layouts enabled in kwinrc ([Tiling] EnabledLayouts), in cycle
     * order. Exposed for the D-Bus interface.
     */
    QList<LayoutEngine::LayoutKind> enabledLayoutKinds() const;

    /**
     * The active output's current-desktop layout engine when it is an Emerge
     * strip, otherwise nullptr. Exposed for the D-Bus interface.
     */
    EmergeLayoutEngine *activeEmergeLayoutEngine() const;

    enum class BorderMode {
        None,
        AllTiled,
        ActiveOnly,
        AllWindows,
    };

private Q_SLOTS:
    void onInteractiveMoveResizeStarted();
    void onInteractiveMoveResizeFinished();
    void onWindowDesktopsChanged(Window *window);
    void onWindowMinimizedChanged(Window *window);
    void onWindowMaximizedChanged(Window *window);
    void onWindowFullScreenChanged(Window *window);
    // Re-evaluate ignore/float rules when a window's WM_CLASS arrives
    // late (XWayland clients set WM_CLASS asynchronously after map), so
    // a window tiled before its class was known can still be floated.
    void onWindowClassChanged(Window *window);
    // Called when the noctalia colors file (or kdeglobals) reports a
    // change. Debounces the actual re-read so we only re-resolve colors
    // once per batch of file events.
    void onColorSourcesChanged();

private:
    bool shouldTile(const Window *window) const;
    /**
     * Sanitize the xwaylandvideobridge capture surface — the one window that
     * is documented to be fully transparent (see TilingRules::isVideoBridgeSurface)
     * — so it can never present as an opaque black box:
     *  - undo any maximized state. A maximized window is a direct-scanout
     *    candidate, and direct scanout bypasses the compositor's opacity
     *    blending entirely, so even an opacity-0 window would scan out
     *    opaque black; and
     *  - pin it to opacity 0. KWin reads _NET_WM_WINDOW_OPACITY once at map
     *    time; if the bridge sets the property after that initial read (a
     *    race that does occur for the bridge at boot), the cached
     *    Window::opacity() stays at 1.0 and the surface is composited opaque.
     *
     * This surface is invisible by design, so forcing the intended state is
     * safe for capture (its content is read via X11, not via its on-screen
     * pixels). The narrow predicate means no user-ignored or unmanaged window
     * is ever touched.
     *
     * Idempotent: every guard early-returns when the window is already in the
     * desired state (Window::setOpacity()/setMaximize() no-op on no change),
     * so it is safe to call from the opacityChanged/maximizedChanged handlers
     * without recursing.
     */
    void sanitizeVideoBridgeSurface(Window *window);
    void onWindowMoveFinished(Window *window);
    void focusInDirection(LayoutEngine::FocusDirection direction);
    void swapInDirection(LayoutEngine::FocusDirection direction);
    Window *windowUnderCursorInEngine(LayoutEngine *engine) const;
    /**
     * Which drop zone of @p target the cursor is currently over: the top
     * quarter is InsertAbove, the bottom quarter is InsertBelow, and the
     * middle half is Swap.
     */
    LayoutEngine::DropZone dropZoneForTarget(Window *target) const;
    /** Push m_columnsMaxColumns to every live layout engine. */
    void applyMaxColumnsToEngines();
    /** Push the Emerge strip settings to every live Emerge engine. */
    void applyEmergeSettingsToEngines();
    /** Apply the cached Emerge settings to one engine. */
    void configureEmergeEngine(EmergeLayoutEngine *engine);
    void addWindowToLayout(Window *window, LogicalOutput *output, VirtualDesktop *desktop);
    void removeWindowFromLayouts(Window *window);
    void migrateWindow(Window *window, LogicalOutput *newOutput, VirtualDesktop *newDesktop);
    LayoutEngine *activeLayoutEngine() const;
    LayoutEngine *layoutEngineForWindow(Window *window, LogicalOutput **output = nullptr, VirtualDesktop **desktop = nullptr) const;
    Window *activeTiledWindow() const;

    void setupLayoutEngine(TileManager *manager, VirtualDesktop *desktop, LayoutEngine::LayoutKind kind);
    LayoutEngine::LayoutKind resolveLayoutKind(LogicalOutput *output, VirtualDesktop *desktop = nullptr) const;
    LayoutEngine::LayoutKind globalDefaultLayoutKind() const;
    /**
     * Whether tiling is disabled (float-only) for @p output's @p desktop, per
     * the [Tiling][DesktopOutput <n>:<out>] "FloatOnly" override.
     */
    [[nodiscard]] bool isFloatOnly(LogicalOutput *output, VirtualDesktop *desktop) const;
    /**
     * Float every window in @p desktop's engine on @p output and tear the
     * engine down, remembering the windows so clearFloatOnly() can re-tile
     * exactly them (not ones the user floated manually).
     */
    void applyFloatOnly(LogicalOutput *output, VirtualDesktop *desktop);
    /**
     * Rebuild @p desktop's engine on @p output and re-tile the windows
     * previously floated by applyFloatOnly(). No-op when nothing was floated.
     */
    void clearFloatOnly(LogicalOutput *output, VirtualDesktop *desktop);
    /**
     * Float every window on every (output, desktop) and drop all engines.
     * Used when tiling is disabled globally via [Tiling] Enabled.
     */
    void floatAllWindows();
    /**
     * Re-tile the windows remembered by applyFloatOnly() for every
     * (output, desktop) whose float-only override is no longer set. Safe to
     * call on every reconfigure: it is a no-op when nothing is remembered.
     */
    void restoreFloatedWindows();
    bool isLayoutEnabled(LayoutEngine::LayoutKind kind) const;
    void applyGapSettingsToOutput(LogicalOutput *output);
    void applyCenterTileRatioToOutput(LogicalOutput *output);
    void onCenterTileRatioChanged(qreal ratio);
    void updateBorders();

    void setLayoutOn(LogicalOutput *output, VirtualDesktop *desktop, LayoutEngine::LayoutKind kind);
    void reconcileLayoutKinds();

    /**
     * Republish the active output's Emerge viewport over D-Bus when its
     * off-view column lists changed. Connected to every live Emerge engine's
     * layoutChanged() signal and called after layout/desktop switches.
     */
    void publishEmergeViewport();

    /** Show an on-screen notification for the given layout kind via
     *  org.kde.osdService (the Plasma OSD). No-op when the DBus service
     *  is unavailable or in test mode.
     */
    void showLayoutNotification(LayoutEngine::LayoutKind kind);

    QPointer<Workspace> m_workspace;
    std::unique_ptr<TilingRules> m_rules;
    // Owned by this controller. Registered on the session bus at /Tiling.
    TilingDBusInterface *m_dbusInterface = nullptr;
    bool m_enabled = true;
    LayoutEngine::LayoutKind m_defaultLayout = LayoutEngine::LayoutKind::MasterStack;
    // Per-tile resize step. Units: weight (master/stack column ratio) for
    // horizontal axis on a master/stack layout, otherwise a per-leaf weight
    // delta. Clamped to a sane range in reconfigure().
    qreal m_resizeStep = 0.1;
    // Last-used master column ratio for the CenterTile layout.
    // Persisted to kwinrc so the user's preferred centre-column width
    // survives restarts. Updated whenever the user interactively adjusts
    // the ratio via mouse resize or keyboard shortcut. Clamped to
    // [0.2, 0.95] in reconfigure() and when reading from config.
    qreal m_lastCenterTileRatio = 0.75;
    // Maximum number of columns the Columns layout may grow to.
    // Configurable via kwinrc ("ColumnsMaxColumns", clamped to [3, 5] in
    // reconfigure()); pushed to every Columns engine via setMaxColumns().
    int m_columnsMaxColumns = 3;

    // Last Emerge viewport signature published over D-Bus. Republishing is
    // skipped when the off-view column lists are unchanged, so a busy strip
    // (scroll/resize reflows) does not flood the bus.
    QString m_lastEmergeViewportSignature = QStringLiteral("none");

    // Emerge strip layout. Global for now; per-monitor behaviour is a later
    // addition. Values are read in reconfigure() and pushed to every live
    // Emerge engine via applyEmergeSettingsToEngines().
    int m_emergeAxis = 0; // 0 auto, 1 horizontal, 2 vertical
    QList<qreal> m_emergePresetColumnWidths;
    QList<qreal> m_emergePresetWindowHeights;
    // Per-app Emerge column widths, keyed by lowercased window class.
    QHash<QString, qreal> m_emergeAppWidths;
    bool m_emergePeek = false;
    int m_emergePeekHeight = 48;
    QStringList m_enabledLayouts;
    BorderMode m_borderMode = BorderMode::None;
    qreal m_borderThickness = 2.0;
    int m_cornerRadius = 0;
    enum class ColorSource {
        Custom,
        SystemAccent,
        SystemAccentFaded,
        NoctaliaPrimary,
        NoctaliaAccent,
    };
    ColorSource m_colorSourceActive = ColorSource::SystemAccent;
    ColorSource m_colorSourceInactive = ColorSource::SystemAccentFaded;
    QColor m_borderColorActive;
    QColor m_borderColorInactive;

    QColor resolveColor(ColorSource source, const QColor &custom) const;
    void applyCornerRadius(Window *window);

    // Drop shadow drawn under tiled windows. One DecorationShadow is shared
    // by every window and rebuilt only when its parameters change.
    BorderMode m_shadowMode = BorderMode::None;
    int m_shadowSize = 16;
    QColor m_shadowColor = QColor(0, 0, 0, 120);
    int m_shadowOffsetX = 0;
    int m_shadowOffsetY = 4;
    int m_shadowOpacity = 100; // percent, multiplies the color's own alpha
    std::shared_ptr<KDecoration3::DecorationShadow> m_shadow;
    QString m_shadowKey;
    std::shared_ptr<KDecoration3::DecorationShadow> currentShadow();

    void readNoctaliaColors();
    void readSystemAccent();
    bool usesNoctaliaSource() const;

    QColor m_noctaliaPrimaryColor;
    QColor m_noctaliaAccentColor;
    QColor m_cachedSystemAccent;
    // KSharedConfig instances are cached per-name. We hold long-lived
    // references so we can call reparseConfiguration() when the underlying
    // file changes (via KConfigWatcher), and so all reads see fresh data.
    KSharedConfigPtr m_noctaliaConfig;
    KSharedConfigPtr m_kdeglobalsConfig;
    KSharedConfigPtr m_kwinrcConfig;
    KConfigWatcher::Ptr m_noctaliaWatcher;
    // QFileSystemWatcher is used for the noctalia colors file because
    // KConfigWatcher silently refuses to watch absolute paths, and
    // QStandardPaths::locate() returns an absolute path.
    QFileSystemWatcher *m_noctaliaFsWatcher = nullptr;
    QString m_noctaliaPath;
    qint64 m_noctaliaMtime = -1;
    qint64 m_noctaliaSize = -1;
    // kdeglobals is opened by absolute path too (real config home, outside
    // the compositor's redirected config root), so it needs the same
    // QFileSystemWatcher fallback.
    QFileSystemWatcher *m_kdeglobalsFsWatcher = nullptr;
    qint64 m_kdeglobalsMtime = -1;
    qint64 m_kdeglobalsSize = -1;
    // Debounce the color re-reads: editors and shell-integration daemons
    // often rewrite the file in two steps (write tmp + rename), which
    // produces multiple directoryChanged events in quick succession.
    QTimer m_colorReloadTimer;
    KConfigWatcher::Ptr m_kdeglobalsWatcher;
    KConfigWatcher::Ptr m_kwinrcWatcher;

    struct MoveContext {
        QPointer<LayoutEngine> engine;
        QPointer<LogicalOutput> output;
        // The desktop the window was on when the move started. Used to detect
        // a cross-workspace drag (see onWindowMoveFinished) so the window is
        // migrated to the destination workspace's layout engine on release.
        QPointer<VirtualDesktop> desktop;
        RectF originalGeometryRestore;
    };
    QHash<Window *, MoveContext> m_activeMoves;

    struct ResizeContext {
        QPointer<LayoutEngine> engine;
        RectF originalGeometry;
        Qt::Edge edge = Qt::RightEdge;
    };
    QHash<Window *, ResizeContext> m_activeResizes;

    // The geometry a window had immediately before the tiling engine snapped
    // it into a tile. Used to restore an ignored system surface (e.g. the
    // xwaylandvideobridge capture surface, whose WM_CLASS can arrive after
    // it was already adopted) back to its intended geometry instead of
    // leaving it stuck at the fullscreen tile geometry — a giant black box
    // covering the output. Populated in addWindowToLayout(), consumed (and
    // cleared) in onWindowClassChanged(), and cleaned up in onWindowRemoved().
    QHash<Window *, RectF> m_preTileGeometry;

    // Last configured layout kind per "<output>:<desktop number>", as resolved
    // from kineticwe.kwe by reconcileLayoutKinds(). Reconfigure uses this to
    // tell an *intentional* default-layout change (resolved kind differs ->
    // apply it) from an unrelated reconfigure (resolved kind unchanged ->
    // leave the engine alone, preserving runtime setLayout()/cycleLayout()
    // switches).
    QHash<QString, LayoutEngine::LayoutKind> m_reconciledLayoutKinds;

    // Windows floated by the float-only toggle / global disable, keyed by
    // "<output>:<desktop number>". QPointer so a destroyed window is skipped
    // rather than re-tiled. Lets the toggle round-trip re-tile exactly the
    // windows it floated.
    QHash<QString, QList<QPointer<Window>>> m_floatOnlyWindows;
};

} // namespace KWin
