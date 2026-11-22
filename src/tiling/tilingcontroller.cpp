/*
    KWin - the KDE window manager
    SPDX-FileCopyrightText: 2026 KWin Tiling Fork
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "tilingcontroller.h"
#include "config-kwin.h"

#include "tilingdbusinterface.h"

#include "core/output.h"
#include "core/rect.h"
#include "cursor.h"
#include "scene/borderradius.h"
#include "tiles/autogridlayoutengine.h"
#include "tiles/centertilelayoutengine.h"
#include "tiles/columnslayoutengine.h"
#include "tiles/layoutengine.h"
#include "tiles/masterstacklayoutengine.h"
#include "tiles/emergelayoutengine.h"
#include "tiles/stackedlayoutengine.h"
#include "tiles/monoclelayoutengine.h"
#include "tiles/tilemanager.h"
#include "utils/gravity.h"
#include "virtualdesktops.h"
#include "window.h"
#include "workspace.h"

#include <KColorScheme>
#include <KConfigGroup>
#include <KDecoration3/DecorationShadow>
#include <KSharedConfig>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QStandardPaths>
#include <QVariant>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>
#include <vector>

namespace KWin
{

namespace
{

std::unique_ptr<LayoutEngine> createLayoutEngine(LayoutEngine::LayoutKind kind, QObject *parent)
{
    switch (kind) {
    case LayoutEngine::LayoutKind::Stacked:
        return std::make_unique<StackedLayoutEngine>(parent);
    case LayoutEngine::LayoutKind::CenterTile:
        return std::make_unique<CenterTileLayoutEngine>(parent);
    case LayoutEngine::LayoutKind::Monocle:
        return std::make_unique<MonocleLayoutEngine>(parent);
    case LayoutEngine::LayoutKind::AutoGrid:
        return std::make_unique<AutoGridLayoutEngine>(parent);
    case LayoutEngine::LayoutKind::Columns:
        return std::make_unique<ColumnsLayoutEngine>(parent);
    case LayoutEngine::LayoutKind::Emerge:
        return std::make_unique<EmergeLayoutEngine>(parent);
    case LayoutEngine::LayoutKind::MasterStack:
    default:
        return std::make_unique<MasterStackLayoutEngine>(parent);
    }
}

// Canonical preset vocabulary for the Emerge layout. Kept in sync with the
// KCM/kcfg default; a value out of (0,1] is dropped on read.
const QList<qreal> kDefaultEmergePresets{1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 1.0};

QList<qreal> parseProportionList(const QStringList &entries)
{
    QList<qreal> result;
    for (const QString &entry : entries) {
        bool ok = false;
        const qreal value = entry.trimmed().toDouble(&ok);
        if (ok && value > 0.0 && value <= 1.0) {
            result.append(value);
        }
    }
    return result.isEmpty() ? kDefaultEmergePresets : result;
}

// "windowClass=fraction" entries for per-app Emerge column widths. The class is
// matched exactly against Window::resourceClass() (case-insensitively); a value
// out of (0,1] or an empty class is dropped and the last duplicate wins.
QHash<QString, qreal> parseEmergeAppWidths(const QStringList &entries)
{
    QHash<QString, qreal> result;
    for (const QString &entry : entries) {
        // Last '=' so a class name containing '=' still parses.
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        if (separator < 0) {
            continue;
        }
        const QString windowClass = entry.left(separator).trimmed().toLower();
        if (windowClass.isEmpty()) {
            continue;
        }
        bool ok = false;
        const qreal value = entry.mid(separator + 1).trimmed().toDouble(&ok);
        if (ok && value > 0.0 && value <= 1.0) {
            result.insert(windowClass, value);
        }
    }
    return result;
}

// An enum setting can be stored as the kcfg choice NAME (Kinetic Settings
// writes strings) or as the integer index (a KDE KCM writes ints). Accept both
// and fall back to @p fallback for anything unrecognised.
int readEnumSetting(const KConfigGroup &group, const QString &key, const QStringList &names, int fallback)
{
    const QString value = group.readEntry(key, QString());
    if (value.isEmpty()) {
        return fallback;
    }
    const int byName = names.indexOf(value, 0, Qt::CaseInsensitive);
    if (byName >= 0) {
        return byName;
    }
    bool ok = false;
    const int byIndex = value.toInt(&ok);
    return ok ? byIndex : fallback;
}

// One-time migration of the Emerge layout's config keys from the pre-rename
// "Scrolling*" names. Copies each old value onto the new key when the new one is
// absent, then removes the old key so the config converges after one load.
// Returns true when anything changed, so the caller can sync the file.
bool migrateEmergeKeyNames(KConfigGroup &group)
{
    static const std::array<std::pair<const char *, const char *>, 3> kRenamedKeys{{
        {"ScrollingAxis", "EmergeAxis"},
        {"ScrollingPresetColumnWidths", "EmergePresetColumnWidths"},
        {"ScrollingPresetWindowHeights", "EmergePresetWindowHeights"},
    }};
    bool changed = false;
    for (const auto &[oldKey, newKey] : kRenamedKeys) {
        const QString oldName = QString::fromLatin1(oldKey);
        const QString newName = QString::fromLatin1(newKey);
        if (!group.hasKey(oldName)) {
            continue;
        }
        if (!group.hasKey(newName)) {
            group.writeEntry(newName, group.readEntry(oldName, QVariant()));
        }
        group.deleteEntry(oldName);
        changed = true;
    }

    // Also rename the persisted layout NAME, which lives in values rather than
    // keys: EnabledLayouts (a list) and DefaultLayout (a string), plus the
    // per-(desktop, output) override groups' DefaultLayout.
    const QStringList enabled = group.readEntry(QStringLiteral("EnabledLayouts"), QStringList());
    if (enabled.contains(QStringLiteral("Scrolling"))) {
        QStringList renamed = enabled;
        for (QString &name : renamed) {
            if (name == QLatin1String("Scrolling")) {
                name = QStringLiteral("Emerge");
            }
        }
        group.writeEntry(QStringLiteral("EnabledLayouts"), renamed);
        changed = true;
    }
    const auto maybeRenameLayoutValue = [&changed](KConfigGroup &owner) {
        const QString value = owner.readEntry(QStringLiteral("DefaultLayout"), QString());
        if (value.compare(QLatin1String("Scrolling"), Qt::CaseInsensitive) == 0) {
            owner.writeEntry(QStringLiteral("DefaultLayout"), QStringLiteral("Emerge"));
            changed = true;
        }
    };
    maybeRenameLayoutValue(group);
    const QStringList subGroups = group.groupList();
    for (const QString &sub : subGroups) {
        if (!sub.startsWith(QLatin1String("DesktopOutput "))) {
            continue;
        }
        KConfigGroup subGroup(&group, sub);
        maybeRenameLayoutValue(subGroup);
    }

    return changed;
}

// Key identifying one (output, desktop) pair for the float-only bookkeeping,
// matching the [Tiling][DesktopOutput <n>:<out>] override group naming.
QString floatOnlyKey(LogicalOutput *output, VirtualDesktop *desktop)
{
    if (!output || !desktop) {
        return QString();
    }
    return output->name() + QLatin1Char(':') + QString::number(desktop->x11DesktopNumber());
}

} // namespace

TilingController::TilingController(Workspace *workspace)
    : QObject(workspace)
    , m_workspace(workspace)
    , m_rules(std::make_unique<TilingRules>())
{
    // Expose the live tiling state on the session bus before the first
    // reconfigure() so external consumers (Noctalia bar plugin, scripts)
    // see the /Tiling object from the start. The object becomes reachable
    // once DBusInterface acquires the org.kde.KWin service name.
    m_dbusInterface = new TilingDBusInterface(this, this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Tiling"), m_dbusInterface,
                                                 QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);

    reconfigure();

    if (m_workspace) {
        connect(m_workspace, &Workspace::windowActivated, this, [this]() {
            updateBorders();
            // Emerge strips follow focus: scroll to the newly activated
            // window's column when it belongs to a strip. This runs AFTER
            // activation (unlike addWindow, which precedes it), so it cannot
            // fight a new window taking focus.
            if (Window *win = m_workspace->activeWindow()) {
                if (auto *sc = qobject_cast<EmergeLayoutEngine *>(layoutEngineForWindow(win))) {
                    sc->focusWindow(win);
                }
            }
        });
        // The active output's strip is what the D-Bus bridge reports, so a
        // focus move between monitors republishes its viewport.
        connect(m_workspace, &Workspace::activeOutputChanged, this, &TilingController::publishEmergeViewport);
    }

    // Hold long-lived KSharedConfig handles for the two config files we
    // tail. KSharedConfig caches parsed values in memory; without
    // reparseConfiguration() the second read after an external edit would
    // see stale data even though the watcher fired.
    //
    // KConfigWatcher relies on the kconfig D-Bus notify interface and
    // only works for configs opened with a *relative* name. noctalia.colors
    // is opened via QStandardPaths::locate() which returns an absolute
    // path, so we fall back to a QFileSystemWatcher for that file. The
    // kdeglobals config below is opened with a relative name and so the
    // KConfigWatcher path is correct there.
    const QString noctaliaPath = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("color-schemes/noctalia.colors"));
    if (!noctaliaPath.isEmpty()) {
        m_noctaliaConfig = KSharedConfig::openConfig(noctaliaPath, KConfig::SimpleConfig);
        m_noctaliaPath = noctaliaPath;
        m_noctaliaWatcher = KConfigWatcher::create(m_noctaliaConfig);
        // The KConfigWatcher is best-effort for the absolute-path case
        // above. We still install it so we get notified for kconfig-style
        // (in-process) writes; for external writes the QFileSystemWatcher
        // below is what actually fires.
        connect(m_noctaliaWatcher.data(), &KConfigWatcher::configChanged, this, [this]() {
            onColorSourcesChanged();
        });

        m_noctaliaFsWatcher = new QFileSystemWatcher(this);
        // Watch the parent directory rather than the file itself: writers use
        // atomic-rename semantics which replace the inode and silently kill a
        // direct file watch's inotify watch. A directory's inode is never
        // replaced, so its watch survives; filter the noisy directoryChanged
        // signal by the watched file's mtime + size.
        if (m_noctaliaFsWatcher->addPath(QFileInfo(noctaliaPath).absolutePath())) {
            m_noctaliaMtime = QFileInfo(noctaliaPath).lastModified().toMSecsSinceEpoch();
            m_noctaliaSize = QFileInfo(noctaliaPath).size();
            connect(m_noctaliaFsWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &) {
                if (m_noctaliaPath.isEmpty()) {
                    return;
                }
                const QFileInfo info(m_noctaliaPath);
                const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
                const qint64 size = info.size();
                if (mtime == m_noctaliaMtime && size == m_noctaliaSize) {
                    return;
                }
                m_noctaliaMtime = mtime;
                m_noctaliaSize = size;
                onColorSourcesChanged();
            });
        }
    }
    m_kdeglobalsConfig = KSharedConfig::openConfig(kineticweKdeGlobalsPath(), KConfig::FullConfig);
    m_kdeglobalsWatcher = KConfigWatcher::create(m_kdeglobalsConfig);
    connect(m_kdeglobalsWatcher.data(), &KConfigWatcher::configChanged, this, [this]() {
        onColorSourcesChanged();
    });
    // kdeglobals is opened by absolute path (it lives in the user's real
    // config home, outside the compositor's redirected config root), so the
    // kconfig D-Bus notify interface does not cover it — the same QFileSystemWatcher
    // fallback as noctalia.colors above is needed for live theme changes.
    m_kdeglobalsFsWatcher = new QFileSystemWatcher(this);
    // Same directory-watch approach as the noctalia colors file above: the
    // theme service replaces kdeglobals with an atomic rename, which kills a
    // direct file watch's inotify watch.
    const QString kdeglobalsPath = kineticweKdeGlobalsPath();
    if (m_kdeglobalsFsWatcher->addPath(QFileInfo(kdeglobalsPath).absolutePath())) {
        m_kdeglobalsMtime = QFileInfo(kdeglobalsPath).lastModified().toMSecsSinceEpoch();
        m_kdeglobalsSize = QFileInfo(kdeglobalsPath).size();
        connect(m_kdeglobalsFsWatcher, &QFileSystemWatcher::directoryChanged, this, [this, kdeglobalsPath]() {
            const QFileInfo info(kdeglobalsPath);
            const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
            const qint64 size = info.size();
            if (mtime == m_kdeglobalsMtime && size == m_kdeglobalsSize) {
                return;
            }
            m_kdeglobalsMtime = mtime;
            m_kdeglobalsSize = size;
            onColorSourcesChanged();
        });
    }

    // Debounce: a single Noctalia theme switch can fire multiple
    // directoryChanged events (write-to-tmp + rename produces two), and the
    // KConfigWatcher for kdeglobals can fire repeatedly during a kcmshell
    // save. Coalesce them into a single re-read + repaint.
    m_colorReloadTimer.setSingleShot(true);
    m_colorReloadTimer.setInterval(50);
    connect(&m_colorReloadTimer, &QTimer::timeout, this, [this]() {
        readNoctaliaColors();
        readSystemAccent();
        updateBorders();
    });

    // Initial reads so the very first border paint already has the right color.
    readNoctaliaColors();
    readSystemAccent();

    // Watch kwinrc so that changes made via the KCM (border color source,
    // border mode, thickness, etc.) take effect live on the running session
    // without requiring a restart or DBus reconfigure call.
    m_kwinrcConfig = KSharedConfig::openConfig(KWIN_CONFIG);
    m_kwinrcWatcher = KConfigWatcher::create(m_kwinrcConfig);
    connect(m_kwinrcWatcher.data(), &KConfigWatcher::configChanged,
            this, [this](const KConfigGroup &group, const QByteArrayList &) {
        if (group.name() == QLatin1StringView("Tiling")) {
            reconfigure();
        }
    });
}

void TilingController::onColorSourcesChanged()
{
    // Start (or restart) the debounce window. The timer will fire once
    // the dust settles, at which point we re-read both color sources and
    // repaint every window. Calling updateBorders() unconditionally is
    // cheap when nothing changed and correct when the user just toggled
    // a color source in the KCM.
    m_colorReloadTimer.start();
}

TilingController::~TilingController() = default;

void TilingController::reconfigure()
{
    KSharedConfigPtr config = KSharedConfig::openConfig(KWIN_CONFIG);
    KConfigGroup tilingGroup(config, QStringLiteral("Tiling"));
    KConfigGroup rulesGroup(config, QStringLiteral("TilingRules"));

    // Rename the layout's config keys from the old "Scrolling*" names once.
    if (migrateEmergeKeyNames(tilingGroup)) {
        config->sync();
    }

    const bool wasEnabled = m_enabled;
    m_enabled = tilingGroup.readEntry("Enabled", true);
    m_defaultLayout = LayoutEngine::layoutKindFromString(
        tilingGroup.readEntry("DefaultLayout", QStringLiteral("MasterStack")));
    // Whitelist of layouts the user wants available. Order in the list also
    // defines the cycle order used by cycleLayout().
    const QStringList previousEnabledLayouts = m_enabledLayouts;
    m_enabledLayouts = tilingGroup.readEntry("EnabledLayouts",
        QStringList{QLatin1String("MasterStack"), QLatin1String("Stacked"), QLatin1String("CenterTile"), QLatin1String("Columns"), QLatin1String("Monocle"), QLatin1String("AutoGrid"), QLatin1String("Emerge")});
    m_rules->load(rulesGroup);

    // Per-tile resize step. Weight units; see tilingcontroller.h. Clamped to
    // a sane range so a misconfigured kwinrc cannot disable resize or make
    // a single press span the whole column.
    m_resizeStep = std::clamp(tilingGroup.readEntry("ResizeStep", 0.1), qreal(0.01), qreal(1.0));

    // Last-used master column ratio for the CenterTile layout. Persisted
    // to kwinrc whenever the user interactively adjusts the centre-column
    // width via mouse resize or keyboard shortcut. Clamped to [0.2, 0.95].
    m_lastCenterTileRatio = std::clamp(tilingGroup.readEntry("CenterTileLastMasterRatio", 0.75), 0.2, 0.95);

    // Maximum number of columns the Columns layout may grow to. Clamped to
    // the range the KCM exposes so a misconfigured kwinrc cannot produce
    // unusably narrow columns.
    m_columnsMaxColumns = std::clamp(tilingGroup.readEntry("ColumnsMaxColumns", 3), 3, 5);
    applyMaxColumnsToEngines();

    // Emerge strip layout. Axis and presets are global for now (per-monitor
    // behaviour is a later addition); pushed to every live Emerge engine.
    m_emergeAxis = std::clamp(readEnumSetting(tilingGroup, QStringLiteral("EmergeAxis"),
                                                 {QStringLiteral("Auto"), QStringLiteral("Horizontal"), QStringLiteral("Vertical")}, 0), 0, 2);
    m_emergePresetColumnWidths = parseProportionList(tilingGroup.readEntry(
        "EmergePresetColumnWidths",
        QStringList{QStringLiteral("0.333"), QStringLiteral("0.5"), QStringLiteral("0.667"), QStringLiteral("0.75"), QStringLiteral("1")}));
    m_emergePresetWindowHeights = parseProportionList(tilingGroup.readEntry(
        "EmergePresetWindowHeights",
        QStringList{QStringLiteral("0.333"), QStringLiteral("0.5"), QStringLiteral("0.667"), QStringLiteral("0.75"), QStringLiteral("1")}));
    m_emergePeek = tilingGroup.readEntry("EmergePeek", false);
    m_emergePeekHeight = std::clamp(tilingGroup.readEntry("EmergePeekHeight", 48), 0, 1000);
    m_emergeAppWidths = parseEmergeAppWidths(tilingGroup.readEntry("EmergeAppWidths", QStringList()));
    applyEmergeSettingsToEngines();

    const QString borderModeString = tilingGroup.readEntry("TilingBorderMode", QStringLiteral("None"));
    if (borderModeString == QLatin1String("AllTiled")) {
        m_borderMode = BorderMode::AllTiled;
    } else if (borderModeString == QLatin1String("ActiveOnly")) {
        m_borderMode = BorderMode::ActiveOnly;
    } else if (borderModeString == QLatin1String("AllWindows")) {
        m_borderMode = BorderMode::AllWindows;
    } else {
        m_borderMode = BorderMode::None;
    }
    m_borderThickness = tilingGroup.readEntry("TilingBorderThickness", 2.0);
    m_cornerRadius = tilingGroup.readEntry("TilingCornerRadius", 0);

    const QString shadowModeString = tilingGroup.readEntry("TilingShadowMode", QStringLiteral("None"));
    if (shadowModeString == QLatin1String("AllTiled")) {
        m_shadowMode = BorderMode::AllTiled;
    } else if (shadowModeString == QLatin1String("ActiveOnly")) {
        m_shadowMode = BorderMode::ActiveOnly;
    } else if (shadowModeString == QLatin1String("AllWindows")) {
        m_shadowMode = BorderMode::AllWindows;
    } else {
        m_shadowMode = BorderMode::None;
    }
    m_shadowSize = std::clamp(tilingGroup.readEntry("TilingShadowSize", 16), 0, 64);
    m_shadowColor = tilingGroup.readEntry("TilingShadowColor", QColor(0, 0, 0, 120));
    m_shadowOffsetX = std::clamp(tilingGroup.readEntry("TilingShadowOffsetX", 0), -32, 32);
    m_shadowOffsetY = std::clamp(tilingGroup.readEntry("TilingShadowOffsetY", 4), 0, 32);
    m_shadowOpacity = std::clamp(tilingGroup.readEntry("TilingShadowOpacity", 100), 0, 100);

    const QString activeSourceString = tilingGroup.readEntry("TilingBorderColorSourceActive", QStringLiteral("SystemAccent"));
    if (activeSourceString == QLatin1String("Custom")) {
        m_colorSourceActive = ColorSource::Custom;
    } else if (activeSourceString == QLatin1String("NoctaliaPrimary")) {
        m_colorSourceActive = ColorSource::NoctaliaPrimary;
    } else if (activeSourceString == QLatin1String("NoctaliaAccent")) {
        m_colorSourceActive = ColorSource::NoctaliaAccent;
    } else {
        m_colorSourceActive = ColorSource::SystemAccent;
    }
    const QString inactiveSourceString = tilingGroup.readEntry("TilingBorderColorSourceInactive", QStringLiteral("SystemAccentFaded"));
    if (inactiveSourceString == QLatin1String("Custom")) {
        m_colorSourceInactive = ColorSource::Custom;
    } else if (inactiveSourceString == QLatin1String("SystemAccent")) {
        m_colorSourceInactive = ColorSource::SystemAccent;
    } else if (inactiveSourceString == QLatin1String("NoctaliaPrimary")) {
        m_colorSourceInactive = ColorSource::NoctaliaPrimary;
    } else if (inactiveSourceString == QLatin1String("NoctaliaAccent")) {
        m_colorSourceInactive = ColorSource::NoctaliaAccent;
    } else {
        m_colorSourceInactive = ColorSource::SystemAccentFaded;
    }
    m_borderColorActive = tilingGroup.readEntry("TilingBorderColorActive",
        QGuiApplication::palette().color(QPalette::Active, QPalette::Highlight));
    m_borderColorInactive = tilingGroup.readEntry("TilingBorderColorInactive", QColor(128, 128, 128, 200));

    if (!m_enabled) {
        // Tiling is off (or stayed off): make sure nothing is left tiled, so
        // disabling tiling floats every window immediately instead of leaving
        // already-tiled windows snapped until they are moved by hand.
        floatAllWindows();
    } else {
        initializeLayouts();
        reconcileLayoutKinds();
        // Restore any windows floated by a previous float-only pass or by the
        // global disable; clearFloatOnly() only touches remembered windows, so
        // this cannot disturb manual floats.
        restoreFloatedWindows();
    }

    if (m_dbusInterface) {
        if (m_enabledLayouts != previousEnabledLayouts) {
            Q_EMIT m_dbusInterface->enabledLayoutsChanged(m_enabledLayouts);
        }
        // Toggling tiling on/off changes what currentLayout() reports even
        // though no engine kind changed.
        if (m_enabled != wasEnabled) {
            Q_EMIT m_dbusInterface->layoutChanged(currentLayoutKindString());
        }
    }

    if (m_workspace) {
        for (LogicalOutput *output : m_workspace->outputs()) {
            applyGapSettingsToOutput(output);
            applyCenterTileRatioToOutput(output);
        }
        updateBorders();
    }

    publishEmergeViewport();
}

void TilingController::initializeLayouts()
{
    if (!m_enabled || !m_workspace) {
        return;
    }

    for (LogicalOutput *output : m_workspace->outputs()) {
        onOutputAdded(output);
    }
}

void TilingController::onOutputAdded(LogicalOutput *output)
{
    if (!m_enabled || !m_workspace || !output) {
        return;
    }

    TileManager *manager = m_workspace->tileManager(output);
    if (!manager) {
        return;
    }
    for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
        // Float-only workspaces have no engine.
        if (isFloatOnly(output, desktop)) {
            continue;
        }
        const LayoutEngine::LayoutKind kind = resolveLayoutKind(output, desktop);
        setupLayoutEngine(manager, desktop, kind);
    }
    applyGapSettingsToOutput(output);
}

void TilingController::setupLayoutEngine(TileManager *manager, VirtualDesktop *desktop, LayoutEngine::LayoutKind kind)
{
    if (!manager || !desktop) {
        return;
    }

    // If a layout engine is already attached, leave it alone. Switching the
    // layout on a live engine is handled by setLayout() / cycleLayout() so
    // that already-tiled windows can be migrated cleanly.
    if (manager->layoutEngine(desktop)) {
        return;
    }

    auto engine = createLayoutEngine(kind, manager);
    engine->setMaxColumns(m_columnsMaxColumns);
    if (auto *sc = qobject_cast<EmergeLayoutEngine *>(engine.get())) {
        configureEmergeEngine(sc);
        // The widget points at the active output/desktop, so any strip reflow
        // is a candidate republish; publishEmergeViewport() filters the rest.
        connect(sc, &LayoutEngine::layoutChanged, this, &TilingController::publishEmergeViewport);
    }
    if (kind == LayoutEngine::LayoutKind::CenterTile) {
        if (auto *ct = qobject_cast<CenterTileLayoutEngine *>(engine.get())) {
            // Apply the persisted last-used ratio so the user sees their
            // preferred centre-column width immediately.
            ct->setMasterRatio(m_lastCenterTileRatio);
            // Persist future interactive ratio changes.
            connect(ct, &CenterTileLayoutEngine::masterRatioChanged,
                    this, &TilingController::onCenterTileRatioChanged);
        }
    }
    manager->setLayoutEngine(desktop, std::move(engine));
}

void TilingController::applyMaxColumnsToEngines()
{
    if (!m_workspace) {
        return;
    }
    for (LogicalOutput *output : m_workspace->outputs()) {
        TileManager *manager = m_workspace->tileManager(output);
        if (!manager) {
            continue;
        }
        for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
            if (LayoutEngine *engine = manager->layoutEngine(desktop)) {
                engine->setMaxColumns(m_columnsMaxColumns);
            }
        }
    }
}

void TilingController::applyEmergeSettingsToEngines()
{
    if (!m_workspace) {
        return;
    }
    for (LogicalOutput *output : m_workspace->outputs()) {
        TileManager *manager = m_workspace->tileManager(output);
        if (!manager) {
            continue;
        }
        for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
            if (auto *sc = qobject_cast<EmergeLayoutEngine *>(manager->layoutEngine(desktop))) {
                configureEmergeEngine(sc);
            }
        }
    }
}

void TilingController::configureEmergeEngine(EmergeLayoutEngine *engine)
{
    if (!engine) {
        return;
    }
    engine->setEmergePresetColumnWidths(m_emergePresetColumnWidths);
    engine->setEmergePresetWindowHeights(m_emergePresetWindowHeights);
    engine->setEmergeAxis(m_emergeAxis);
    engine->setEmergePeek(m_emergePeek);
    engine->setEmergePeekHeightPx(m_emergePeekHeight);
    engine->setEmergeWindowWidthRules(m_emergeAppWidths);
}

LayoutEngine::LayoutKind TilingController::globalDefaultLayoutKind() const
{
    return m_defaultLayout;
}

LayoutEngine::LayoutKind TilingController::layoutKindFor(LogicalOutput *output, VirtualDesktop *desktop) const
{
    if (m_workspace && output && desktop) {
        if (TileManager *manager = m_workspace->tileManager(output)) {
            if (LayoutEngine *engine = manager->layoutEngine(desktop)) {
                return engine->layoutKind();
            }
        }
    }
    return resolveLayoutKind(output, desktop);
}

QString TilingController::currentLayoutKindString() const
{
    if (!m_enabled || !m_workspace) {
        return QString();
    }
    LogicalOutput *output = m_workspace->activeOutput();
    if (!output) {
        return QString();
    }
    VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop(output);
    if (!desktop) {
        return QString();
    }
    // A float-only workspace reports no layout, exactly like tiling being
    // disabled globally.
    if (isFloatOnly(output, desktop)) {
        return QString();
    }
    return LayoutEngine::layoutKindToString(layoutKindFor(output, desktop));
}

LayoutEngine::LayoutKind TilingController::resolveLayoutKind(LogicalOutput *output, VirtualDesktop *desktop) const
{
    KSharedConfigPtr config = KSharedConfig::openConfig(KWIN_CONFIG);
    KConfigGroup tilingGroup(config, QStringLiteral("Tiling"));
    LayoutEngine::LayoutKind kind = globalDefaultLayoutKind();

    // Check per-(desktop, output) override first (most specific).
    // This allows different layouts for the same desktop number on
    // different monitors when per-output virtual desktops is enabled.
    if (desktop && output) {
        const KConfigGroup combinedGroup(&tilingGroup, QStringLiteral("DesktopOutput %1:%2").arg(desktop->x11DesktopNumber()).arg(output->name()));
        if (combinedGroup.hasKey("DefaultLayout")) {
            kind = LayoutEngine::layoutKindFromString(combinedGroup.readEntry("DefaultLayout", QString()));
        }
    }

    // Fall back to per-output override
    if (desktop && output && kind == globalDefaultLayoutKind()) {
        const KConfigGroup outputGroup(&tilingGroup, QStringLiteral("Output %1").arg(output->name()));
        if (outputGroup.hasKey("DefaultLayout")) {
            kind = LayoutEngine::layoutKindFromString(outputGroup.readEntry("DefaultLayout", QString()));
        }
    }

    // If the configured default is not currently enabled, fall back to the
    // first enabled layout so the monitor is always in a usable state.
    if (!isLayoutEnabled(kind)) {
        const QList<LayoutEngine::LayoutKind> enabled = enabledLayoutKinds();
        if (!enabled.isEmpty()) {
            kind = enabled.first();
        }
    }
    return kind;
}

bool TilingController::isFloatOnly(LogicalOutput *output, VirtualDesktop *desktop) const
{
    if (!output || !desktop) {
        return false;
    }
    KSharedConfigPtr config = KSharedConfig::openConfig(KWIN_CONFIG);
    KConfigGroup tilingGroup(config, QStringLiteral("Tiling"));
    const KConfigGroup group(&tilingGroup,
        QStringLiteral("DesktopOutput %1:%2").arg(desktop->x11DesktopNumber()).arg(output->name()));
    return group.readEntry("FloatOnly", false);
}

void TilingController::applyFloatOnly(LogicalOutput *output, VirtualDesktop *desktop)
{
    if (!m_workspace || !output || !desktop) {
        return;
    }
    TileManager *manager = m_workspace->tileManager(output);
    if (!manager) {
        return;
    }
    LayoutEngine *engine = manager->layoutEngine(desktop);
    if (!engine) {
        return;
    }

    // Remember the windows so toggling back on re-tiles exactly them (and not
    // ones the user floated by hand). Each window keeps its current geometry —
    // it simply stops being managed by the engine.
    QList<QPointer<Window>> &remembered = m_floatOnlyWindows[floatOnlyKey(output, desktop)];
    const QList<Window *> windows = engine->windows();
    for (Window *window : windows) {
        if (!window || window->isDeleted()) {
            continue;
        }
        engine->removeWindow(window);
        window->tilingState().mode = TilingState::Mode::Floating;
        window->tilingState().userToggledFloat = false;
        m_preTileGeometry.remove(window);
        m_activeMoves.remove(window);
        m_activeResizes.remove(window);
        remembered.append(window);
    }
    engine->pruneEmptyLeaves();
    manager->setLayoutEngine(desktop, nullptr);
}

void TilingController::clearFloatOnly(LogicalOutput *output, VirtualDesktop *desktop)
{
    if (!m_workspace || !output || !desktop) {
        return;
    }
    TileManager *manager = m_workspace->tileManager(output);
    if (!manager) {
        return;
    }

    // Rebuild the engine the float-only pass tore down.
    if (!manager->layoutEngine(desktop)) {
        setupLayoutEngine(manager, desktop, resolveLayoutKind(output, desktop));
    }

    const auto it = m_floatOnlyWindows.find(floatOnlyKey(output, desktop));
    if (it == m_floatOnlyWindows.end()) {
        return;
    }
    const QList<QPointer<Window>> remembered = it.value();
    m_floatOnlyWindows.erase(it);

    for (const QPointer<Window> &window : remembered) {
        if (!window || window->isDeleted()) {
            continue;
        }
        // Respect a manual float (or a rule float applied while float-only
        // was on).
        if (window->tilingState().mode != TilingState::Mode::Floating
            || window->tilingState().userToggledFloat) {
            continue;
        }
        // A window that moved away, is minimized or maximized re-joins through
        // the normal handlers when it returns to a tileable state.
        if (window->output() != output
            || (!window->desktops().isEmpty() && window->desktops().constFirst() != desktop)
            || window->isMinimized() || window->maximizeMode() != MaximizeRestore) {
            continue;
        }
        addWindowToLayout(window, output, desktop);
    }
}

void TilingController::floatAllWindows()
{
    if (!m_workspace) {
        return;
    }
    for (LogicalOutput *output : m_workspace->outputs()) {
        if (!output) {
            continue;
        }
        TileManager *manager = m_workspace->tileManager(output);
        if (!manager) {
            continue;
        }
        for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
            if (manager->layoutEngine(desktop)) {
                applyFloatOnly(output, desktop);
            }
        }
    }
    updateBorders();
}

void TilingController::restoreFloatedWindows()
{
    if (!m_workspace) {
        return;
    }
    for (LogicalOutput *output : m_workspace->outputs()) {
        if (!output) {
            continue;
        }
        for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
            if (isFloatOnly(output, desktop)) {
                continue;
            }
            if (m_floatOnlyWindows.contains(floatOnlyKey(output, desktop))) {
                clearFloatOnly(output, desktop);
            }
        }
    }
    updateBorders();
}

void TilingController::toggleFloatOnly()
{
    if (!m_workspace) {
        return;
    }
    LogicalOutput *output = m_workspace->activeOutput();
    if (!output) {
        return;
    }
    VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop(output);
    if (!desktop) {
        return;
    }

    KSharedConfigPtr config = KSharedConfig::openConfig(KWIN_CONFIG);
    // Reparse so edits made outside the compositor (e.g. the settings page
    // writing the file) are picked up before the key is flipped.
    config->reparseConfiguration();
    const bool enable = !isFloatOnly(output, desktop);

    KConfigGroup tilingGroup(config, QStringLiteral("Tiling"));
    KConfigGroup group(&tilingGroup,
        QStringLiteral("DesktopOutput %1:%2").arg(desktop->x11DesktopNumber()).arg(output->name()));
    if (enable) {
        group.writeEntry("FloatOnly", true);
    } else {
        group.deleteEntry("FloatOnly");
    }
    config->sync();

    if (enable) {
        applyFloatOnly(output, desktop);
    } else {
        clearFloatOnly(output, desktop);
    }
    updateBorders();

    if (m_dbusInterface) {
        Q_EMIT m_dbusInterface->layoutChanged(currentLayoutKindString());
    }
}

QList<LayoutEngine::LayoutKind> TilingController::enabledLayoutKinds() const
{
    QList<LayoutEngine::LayoutKind> result;
    for (const QString &name : m_enabledLayouts) {
        // Only include kinds the controller actually knows how to build.
        if (name.compare(QLatin1String("MasterStack"), Qt::CaseInsensitive) == 0) {
            result.append(LayoutEngine::LayoutKind::MasterStack);
        } else if (name.compare(QLatin1String("Stacked"), Qt::CaseInsensitive) == 0) {
            result.append(LayoutEngine::LayoutKind::Stacked);
        } else if (name.compare(QLatin1String("CenterTile"), Qt::CaseInsensitive) == 0) {
            result.append(LayoutEngine::LayoutKind::CenterTile);
        } else if (name.compare(QLatin1String("Monocle"), Qt::CaseInsensitive) == 0) {
            result.append(LayoutEngine::LayoutKind::Monocle);
        } else if (name.compare(QLatin1String("AutoGrid"), Qt::CaseInsensitive) == 0) {
            result.append(LayoutEngine::LayoutKind::AutoGrid);
        } else if (name.compare(QLatin1String("Columns"), Qt::CaseInsensitive) == 0) {
            result.append(LayoutEngine::LayoutKind::Columns);
        } else if (name.compare(QLatin1String("Emerge"), Qt::CaseInsensitive) == 0
                   || name.compare(QLatin1String("Scrolling"), Qt::CaseInsensitive) == 0) {
            // "Scrolling" is the layout's pre-rename name, still honored so an
            // un-migrated config keeps the layout enabled.
            result.append(LayoutEngine::LayoutKind::Emerge);
        }
    }
    if (result.isEmpty()) {
        // The user has disabled everything; fall back to the global default so
        // we always have at least one layout available.
        result.append(globalDefaultLayoutKind());
    }
    return result;
}

bool TilingController::isLayoutEnabled(LayoutEngine::LayoutKind kind) const
{
    return enabledLayoutKinds().contains(kind);
}

void TilingController::updateBorders()
{
    if (!m_workspace) {
        return;
    }

    Window *activeWindow = m_workspace->activeWindow();
    const bool activeOnly = (m_borderMode == BorderMode::ActiveOnly);
    const bool allWindows = (m_borderMode == BorderMode::AllWindows);

    for (Window *window : m_workspace->windows()) {
        if (!window || window->isDeleted()) {
            continue;
        }

        const bool isTiled = (window->tilingState().mode == TilingState::Mode::Tiled);
        // Fullscreen and maximized windows should not show borders or rounded
        // corners — they take the full usable area and a border would look
        // redundant (like a frame around an edge-to-edge canvas).
        const bool isMaximizedOrFullscreen = window->isFullScreen()
            || window->maximizeMode() == MaximizeFull;
        // AllWindows should still skip non-application surfaces: the
        // desktop background, panels/docks, notifications, OSDs, screen
        // locker, and the compositor's own outline. Drawing a border on
        // them produces the visual artifacts seen in the AllWindows
        // preview (border on the wallpaper, on the panel, etc.).
        //
        // Some shells (e.g. Noctalia) don't set the _NET_WM_WINDOW_TYPE_*
        // hints consistently, so we also filter by KWin layer and by the
        // skipTaskbar/skipPager hints that layer-shell windows set.
        const Layer layer = window->layer();
        const bool isSystemSurface = window->isDesktop()
            || window->isDock()
            || window->isNotification()
            || window->isCriticalNotification()
            || window->isOnScreenDisplay()
            || window->isAppletPopup()
            || window->isLockScreen()
            || window->isInputMethod()
            || window->isOutline()
            || layer == DesktopLayer
            || layer == BelowLayer
            || layer == NotificationLayer
            || layer == CriticalNotificationLayer
            || layer == OnScreenDisplayLayer
            || layer == OverlayLayer
            || layer == PopupLayer
            || (window->skipTaskbar() && window->skipPager());
        bool shouldShowShadow = false;
        if (m_shadowMode != BorderMode::None && !isSystemSurface && !isMaximizedOrFullscreen) {
            switch (m_shadowMode) {
            case BorderMode::AllTiled:
                shouldShowShadow = isTiled;
                break;
            case BorderMode::ActiveOnly:
                shouldShowShadow = (window == activeWindow);
                break;
            case BorderMode::AllWindows:
                shouldShowShadow = true;
                break;
            case BorderMode::None:
                break;
            }
        }

        bool shouldShow = false;
        if (m_borderMode != BorderMode::None) {
            if (allWindows || isTiled || (activeOnly && window == activeWindow)) {
                if (isSystemSurface || isMaximizedOrFullscreen) {
                    // Skip system surfaces (desktop, panels, notifications,
                    // popups, lock screen, etc.) in all border modes.
                    // Also skip fullscreen and maximized windows — they fill
                    // the usable area and a border would be redundant.
                } else if (!activeOnly || window == activeWindow) {
                    shouldShow = true;
                }
            }
        }

        const QColor sourceColor = (window == activeWindow) ? m_borderColorActive : m_borderColorInactive;
        const ColorSource source = (window == activeWindow) ? m_colorSourceActive : m_colorSourceInactive;
        const QColor borderColor = resolveColor(source, sourceColor);

        if (isMaximizedOrFullscreen) {
            // Square corners for fullscreen/maximized windows: reset any
            // radius applied while tiled/floating so it does not linger
            // (rounded corners clash with the edge-to-edge appearance and
            // leak onto adjacent outputs).
            if (window->borderRadius() != BorderRadius()) {
                window->setBorderRadius(BorderRadius());
            }
        } else if (isTiled || allWindows || (activeOnly && window == activeWindow)) {
            applyCornerRadius(window);
        }

        TilingState &state = window->tilingState();
        if (state.showBorder != shouldShow || state.borderThickness != m_borderThickness || state.borderColor != borderColor) {
            state.showBorder = shouldShow;
            state.borderThickness = m_borderThickness;
            state.borderColor = borderColor;
            Q_EMIT window->tilingBorderChanged();
        }

        const std::shared_ptr<KDecoration3::DecorationShadow> shadow = shouldShowShadow ? currentShadow() : nullptr;
        if (state.shadow != shadow) {
            state.shadow = shadow;
            window->updateShadow();
        }
    }
}

// Blurs the alpha channel of a premultiplied ARGB32 image in place with three
// box-blur passes per axis, which approximates a gaussian blur.
static void blurAlpha(QImage &image, int radius)
{
    if (radius <= 0) {
        return;
    }
    const int width = image.width();
    const int height = image.height();
    std::vector<int> alpha(size_t(width) * height);
    for (int y = 0; y < height; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            alpha[size_t(y) * width + x] = qAlpha(line[x]);
        }
    }

    std::vector<int> scratch(std::max(width, height));
    const int window = 2 * radius + 1;
    auto boxBlur = [&](int count, int stride, int base) {
        int sum = 0;
        for (int i = -radius; i <= radius; ++i) {
            sum += alpha[base + std::clamp(i, 0, count - 1) * stride];
        }
        for (int i = 0; i < count; ++i) {
            scratch[i] = sum / window;
            sum += alpha[base + std::min(i + radius + 1, count - 1) * stride];
            sum -= alpha[base + std::max(i - radius, 0) * stride];
        }
        for (int i = 0; i < count; ++i) {
            alpha[base + i * stride] = scratch[i];
        }
    };
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < height; ++y) {
            boxBlur(width, 1, y * width);
        }
        for (int x = 0; x < width; ++x) {
            boxBlur(height, width, x);
        }
    }

    for (int y = 0; y < height; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int a = alpha[size_t(y) * width + x];
            line[x] = qRgba(a, a, a, a);
        }
    }
}

std::shared_ptr<KDecoration3::DecorationShadow> TilingController::currentShadow()
{
    QColor tint = m_shadowColor;
    tint.setAlphaF(tint.alphaF() * m_shadowOpacity / 100.0);
    if (m_shadowSize <= 0 || tint.alpha() == 0) {
        return nullptr;
    }

    const QString key = QStringLiteral("%1/%2/%3/%4/%5")
                            .arg(m_shadowSize)
                            .arg(tint.name(QColor::HexArgb))
                            .arg(m_shadowOffsetX)
                            .arg(m_shadowOffsetY)
                            .arg(m_cornerRadius);
    if (m_shadow && m_shadowKey == key) {
        return m_shadow;
    }

    // Three box passes of radius r spread an edge over ~3r on either side.
    const int blurRadius = std::max(1, m_shadowSize / 2);
    const int blurExtent = 3 * blurRadius;

    // The image holds a window-shaped box with the shadow around it. The box
    // must be large enough that the blur cannot reach across it, otherwise
    // the falloff eats into the shadow and it renders almost transparent.
    // Like Breeze, only the centre pixel is stretched: the four corner tiles
    // reach into the window area, which the cut-out below keeps clear.
    const int radius = std::max(m_cornerRadius, 0);
    const int boxSize = 2 * radius + 2 * blurExtent + 1;
    const int margin = blurExtent + std::max(std::abs(m_shadowOffsetX), m_shadowOffsetY);
    const int imageSize = boxSize + 2 * margin;
    const QRectF boxRect(margin, margin, boxSize, boxSize);

    QImage image(imageSize, imageSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRoundedRect(boxRect.translated(m_shadowOffsetX, m_shadowOffsetY), radius, radius);
    }

    blurAlpha(image, blurRadius);

    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        // Tint the blurred mask with the configured color.
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(image.rect(), tint);
        // Punch out the area under the window so translucent windows do not
        // show their own shadow through them.
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawRoundedRect(boxRect, radius, radius);
    }

    auto shadow = std::make_shared<KDecoration3::DecorationShadow>();
    shadow->setPadding(QMarginsF(margin, margin, margin, margin));
    shadow->setInnerShadowRect(QRectF(imageSize / 2, imageSize / 2, 1, 1));
    shadow->setShadow(image);
    m_shadow = shadow;
    m_shadowKey = key;
    return m_shadow;
}

QColor TilingController::resolveColor(ColorSource source, const QColor &custom) const
{
    switch (source) {
    case ColorSource::SystemAccent:
    case ColorSource::SystemAccentFaded: {
        QColor accent = m_cachedSystemAccent;
        if (source == ColorSource::SystemAccentFaded) {
            accent.setAlpha(128);
        }
        return accent;
    }
    case ColorSource::NoctaliaPrimary:
        return m_noctaliaPrimaryColor.isValid() ? m_noctaliaPrimaryColor : custom;
    case ColorSource::NoctaliaAccent:
        return m_noctaliaAccentColor.isValid() ? m_noctaliaAccentColor : custom;
    case ColorSource::Custom:
    default:
        return custom;
    }
}

void TilingController::readNoctaliaColors()
{
    if (!m_noctaliaConfig) {
        return;
    }
    // KSharedConfig caches the parsed file in memory; reparseConfiguration()
    // forces a re-read so we see changes the user made in noctalia since the
    // last call. Without this, the second-and-onward watcher callbacks
    // would read stale values and the border would never update live.
    m_noctaliaConfig->reparseConfiguration();

    // Noctalia primary: Colors:Selection BackgroundNormal
    KConfigGroup selectionGroup(m_noctaliaConfig, QStringLiteral("Colors:Selection"));
    m_noctaliaPrimaryColor = selectionGroup.readEntry("BackgroundNormal", QColor());

    // Noctalia accent: Colors:Button ForegroundPositive
    KConfigGroup buttonGroup(m_noctaliaConfig, QStringLiteral("Colors:Button"));
    m_noctaliaAccentColor = buttonGroup.readEntry("ForegroundPositive", QColor());
}

void TilingController::readSystemAccent()
{
    if (!m_kdeglobalsConfig) {
        m_kdeglobalsConfig = KSharedConfig::openConfig(kineticweKdeGlobalsPath(), KConfig::FullConfig);
    }
    // Same staleness pitfall as readNoctaliaColors().
    m_kdeglobalsConfig->reparseConfiguration();
    // KColorScheme::createApplicationPalette reads [Colors:Selection] and
    // friends from the given config, then returns a QPalette that
    // QPalette::Highlight tracks correctly. Using QGuiApplication::palette()
    // directly would return KWin's default blue, not the user's scheme.
    const QPalette palette = KColorScheme::createApplicationPalette(m_kdeglobalsConfig);
    m_cachedSystemAccent = palette.color(QPalette::Active, QPalette::Highlight);
}

bool TilingController::usesNoctaliaSource() const
{
    return m_colorSourceActive == ColorSource::NoctaliaPrimary
        || m_colorSourceActive == ColorSource::NoctaliaAccent
        || m_colorSourceInactive == ColorSource::NoctaliaPrimary
        || m_colorSourceInactive == ColorSource::NoctaliaAccent;
}

void TilingController::applyCornerRadius(Window *window)
{
    const BorderRadius desired(m_cornerRadius);
    if (window->borderRadius() != desired) {
        window->setBorderRadius(desired);
    }
}

void TilingController::applyGapSettingsToOutput(LogicalOutput *output)
{
    if (!m_workspace || !output) {
        return;
    }

    TileManager *manager = m_workspace->tileManager(output);
    if (!manager) {
        return;
    }

    KSharedConfigPtr config = KSharedConfig::openConfig(KWIN_CONFIG);
    KConfigGroup tilingGroup(config, QStringLiteral("Tiling"));

    // Defaults from the [Tiling] group.
    const qreal defaultGapBetween = tilingGroup.readEntry("GapBetween", 10.0);
    const int defaultGapLeft = tilingGroup.readEntry("GapLeft", 10);
    const int defaultGapRight = tilingGroup.readEntry("GapRight", 10);
    const int defaultGapTop = tilingGroup.readEntry("GapTop", 10);
    const int defaultGapBottom = tilingGroup.readEntry("GapBottom", 10);

    // Per-output override in the [Tiling][Output "name"] sub-group, if any.
    // Entries fall back to the defaults above when not present in the override.
    const QString outputKey = QStringLiteral("Output %1").arg(output->name());
    KConfigGroup outputGroup(&tilingGroup, outputKey);

    const qreal gapBetween = outputGroup.readEntry("GapBetween", defaultGapBetween);
    const int gapLeft = outputGroup.readEntry("GapLeft", defaultGapLeft);
    const int gapRight = outputGroup.readEntry("GapRight", defaultGapRight);
    const int gapTop = outputGroup.readEntry("GapTop", defaultGapTop);
    const int gapBottom = outputGroup.readEntry("GapBottom", defaultGapBottom);
    const QMarginsF gapMargins(gapLeft, gapTop, gapRight, gapBottom);

    for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
        if (RootTile *root = manager->rootTile(desktop)) {
            root->setGapBetween(gapBetween);
            root->setGapMargins(gapMargins);
        }
    }
}

void TilingController::applyCenterTileRatioToOutput(LogicalOutput *output)
{
    if (!m_workspace || !output) {
        return;
    }

    TileManager *manager = m_workspace->tileManager(output);
    if (!manager) {
        return;
    }

    // Push the persisted last-used master ratio into any live CenterTile
    // engine on this output. Other layout kinds are unaffected.
    for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
        if (LayoutEngine *engine = manager->layoutEngine(desktop)) {
            if (engine->layoutKind() == LayoutEngine::LayoutKind::CenterTile) {
                if (auto *centerTileEngine = qobject_cast<CenterTileLayoutEngine *>(engine)) {
                    centerTileEngine->setMasterRatio(m_lastCenterTileRatio);
                }
            }
        }
    }
}

void TilingController::onCenterTileRatioChanged(qreal ratio)
{
    // Clamp and persist the new ratio so it survives restarts.
    m_lastCenterTileRatio = std::clamp(ratio, qreal(0.2), qreal(0.95));
    KSharedConfigPtr config = KSharedConfig::openConfig(KWIN_CONFIG);
    KConfigGroup tilingGroup(config, QStringLiteral("Tiling"));
    tilingGroup.writeEntry("CenterTileLastMasterRatio", m_lastCenterTileRatio);
    tilingGroup.sync();
}

void TilingController::onWindowAdded(Window *window)
{
    if (!m_enabled || !window) {
        return;
    }

    // Watch for mouse-driven moves so we can swap with the window under the
    // cursor on release rather than treating the dragged window as a new one.
    connect(window, &Window::interactiveMoveResizeStarted,
            this, &TilingController::onInteractiveMoveResizeStarted,
            Qt::UniqueConnection);
    connect(window, &Window::interactiveMoveResizeFinished,
            this, &TilingController::onInteractiveMoveResizeFinished,
            Qt::UniqueConnection);

    // When the window is moved between desktops (e.g. via the
    // "Window to Next/Previous/Up/Down Desktop" shortcuts) the layout engines
    // need to migrate the window so it stays tiled on the new desktop and the
    // old layout reflows to fill the empty slot.
    //
    // NOTE: Qt::UniqueConnection does not work with lambda receivers (Qt
    // silently drops the connection with a runtime warning), so we omit it
    // here. onWindowAdded is invoked exactly once per window via the
    // windowAdded signal, so the connection is naturally single-shot per
    // window and there is no need to deduplicate.
    connect(window, &Window::desktopsChanged, this,
            [this, window]() { onWindowDesktopsChanged(window); });

    // When a window is minimized it should vacate its tile so the layout
    // reflows and fills the gap (otherwise a "ghost window" is left
    // behind); when it is unminimized it should re-join the tiling
    // layout, exactly like a window that was just re-added.
    //
    // NOTE: same Qt::UniqueConnection caveat as the desktopsChanged
    // connection above - onWindowAdded runs once per window, so the
    // connection is naturally single-shot per window.
    connect(window, &Window::minimizedChanged, this,
            [this, window]() { onWindowMinimizedChanged(window); });

    // When a window is maximized it should vacate its tile so the layout
    // reflows and fills the gap (otherwise a "ghost window" is left
    // behind); when it is unmaximized it should re-join the tiling
    // layout, exactly like a window that was just re-added. Maximize
    // goes through Window::exitQuickTileMode(), which detaches the
    // window from our leaf via Tile::forget(); the engine then prunes
    // the orphaned empty leaf (see removeWindowFromLayouts /
    // LayoutEngine::pruneEmptyLeaves) so the layout reflows.
    //
    // NOTE: same Qt::UniqueConnection caveat as the desktopsChanged
    // connection above - onWindowAdded runs once per window, so the
    // connection is naturally single-shot per window.
    connect(window, &Window::maximizedChanged, this,
            [this, window]() { onWindowMaximizedChanged(window); });

    // Fullscreen, unlike maximize, keeps the window attached to its tile
    // (core KWin does not call exitQuickTileMode() on the fullscreen path,
    // so the tile association restores the geometry on exit). We must NOT
    // touch layout membership here — doing so mid-transition makes the
    // engine reflow and fight the fullscreen moveResize (flicker, artifacts,
    // window never settling into fullscreen). All that is needed is a
    // border/corner-radius re-evaluation: updateBorders() hides the border
    // and squares the corners while fullscreen, and restores them on exit.
    connect(window, &Window::fullScreenChanged, this,
            [this, window]() { onWindowFullScreenChanged(window); });

    // When the decoration is (re-)applied, it overwrites the window's
    // borderRadius. Re-apply our corner radius if the window is tiled —
    // but not while maximized/fullscreen, where corners stay square
    // (configureDecoration() fires during the fullscreen transition,
    // before fullScreenChanged is emitted).
    connect(window, &Window::decorationChanged, this, [this, window]() {
        if (window->tilingState().mode == TilingState::Mode::Tiled
            && !window->isFullScreen() && window->maximizeMode() != MaximizeFull) {
            applyCornerRadius(window);
        }
    });

    // XWayland clients (and some native ones) set WM_CLASS asynchronously,
    // AFTER the window is first mapped. initialMode() below runs once at
    // map time when resourceClass() may still be empty, so an IgnoreClass
    // rule (e.g. xwaylandvideobridge) can be missed and the window gets
    // tiled anyway. Re-evaluate when the class actually arrives.
    connect(window, &Window::windowClassChanged, this,
            [this, window]() { onWindowClassChanged(window); });

    // The xwaylandvideobridge capture surface is documented to be fully
    // transparent, but the boot-time _NET_WM_WINDOW_OPACITY race (and later
    // maximize/active transitions) can leave Window::opacity() stuck at 1.0,
    // presenting the surface as an opaque black box. The one-shot sanitize in
    // onWindowAdded/onWindowClassChanged is not enough on its own because it
    // can be undone after it runs, so re-sanitize whenever the opacity or
    // maximize state changes. isVideoBridgeSurface() is evaluated at signal
    // time (not connection time) so a late-arriving WM_CLASS is still honored,
    // and the guards in sanitizeVideoBridgeSurface() make re-application
    // idempotent. For any other window the predicate is false and this is a
    // no-op, so it is safe to connect for every window.
    connect(window, &Window::opacityChanged, this,
            [this, window]() { sanitizeVideoBridgeSurface(window); });
    connect(window, &Window::maximizedChanged, this,
            [this, window]() { sanitizeVideoBridgeSurface(window); });

    // Don't touch already-managed windows (e.g. on-all-desktops already handled).
    if (window->tilingState().mode != TilingState::Mode::Floating) {
        updateBorders();
        return;
    }

    TilingState::Mode mode = m_rules->initialMode(window);
    // A window rule can force a matched window to float (or to tile).
    // checkFloating() returns the rule's value when a rule applies, otherwise
    // the passed-in default (the tiling layer's own initial mode).
    if (window->supportsWindowRules()) {
        const bool initialFloating = (mode == TilingState::Mode::Floating);
        mode = window->rules()->checkFloating(initialFloating, true) ? TilingState::Mode::Floating : TilingState::Mode::Tiled;
    }
    // A workspace the user marked float-only never tiles, even when the
    // window's rules would otherwise allow it.
    if (mode == TilingState::Mode::Tiled) {
        LogicalOutput *floatOutput = window->output() ? window->output() : m_workspace->activeOutput();
        VirtualDesktop *floatDesktop = window->desktops().isEmpty()
            ? VirtualDesktopManager::self()->currentDesktop(floatOutput)
            : window->desktops().constFirst();
        if (isFloatOnly(floatOutput, floatDesktop)) {
            mode = TilingState::Mode::Floating;
        }
    }
    window->tilingState().mode = mode;

    // The xwaylandvideobridge capture surface is documented to be fully
    // transparent. When its class is known at map time it is floated right
    // away (never reaching onWindowClassChanged), but the
    // _NET_WM_WINDOW_OPACITY race can still leave it opaque. Enforce the
    // intended transparency here too so it never presents as a black box.
    // sanitizeVideoBridgeSurface() no-ops for any other window.
    if (mode == TilingState::Mode::Floating) {
        sanitizeVideoBridgeSurface(window);
    }

    if (mode == TilingState::Mode::Tiled && !window->isMinimized()
        && window->maximizeMode() == MaximizeRestore) {
        // A window that is created already minimized or maximized (rare,
        // but possible e.g. via session restore or window rules) must not
        // be given a tile: it would leave a "ghost" slot. It will re-join
        // the layout on unminimize/unmaximize via the corresponding
        // onWindow*Changed handler.
        LogicalOutput *output = window->output() ? window->output() : m_workspace->activeOutput();
        VirtualDesktop *desktop = window->desktops().isEmpty()
            ? VirtualDesktopManager::self()->currentDesktop(output)
            : window->desktops().constFirst();
        addWindowToLayout(window, output, desktop);
    }

    updateBorders();
}

void TilingController::onWindowRemoved(Window *window)
{
    if (!window) {
        return;
    }
    m_preTileGeometry.remove(window);
    removeWindowFromLayouts(window);
    // Drop the window from the float-only bookkeeping so a re-enable cannot
    // try to re-tile a destroyed window.
    for (auto &windows : m_floatOnlyWindows) {
        windows.removeAll(window);
    }
    updateBorders();
}

void TilingController::onWindowMinimizedChanged(Window *window)
{
    if (!m_enabled || !window) {
        return;
    }

    if (window->isMinimized()) {
        // Detach the window from its tile(s) so the layout reflows and
        // fills the gap. tilingState().mode is deliberately left as
        // Tiled so the window re-joins the layout when unminimized.
        if (layoutEngineForWindow(window)) {
            removeWindowFromLayouts(window);
        }
        updateBorders();
        return;
    }

    // Unminimized: re-join the tiling layout. This mirrors the
    // float->tile path in toggleFloating() / onWindowAdded(): a fresh
    // leaf is created, the window snaps to its tile geometry, and the
    // engine reflows.
    if (shouldTile(window) && !layoutEngineForWindow(window)) {
        LogicalOutput *output = window->output() ? window->output() : m_workspace->activeOutput();
        VirtualDesktop *desktop = window->desktops().isEmpty()
            ? VirtualDesktopManager::self()->currentDesktop(output)
            : window->desktops().constFirst();
        addWindowToLayout(window, output, desktop);
    }
    updateBorders();
}

void TilingController::onWindowMaximizedChanged(Window *window)
{
    if (!m_enabled || !window) {
        return;
    }

    if (window->maximizeMode() != MaximizeRestore) {
        // Maximized: release from tiling so the layout reflows and fills
        // the gap. Maximize already detached the window from its leaf via
        // Window::exitQuickTileMode() -> Tile::forget() (geometry-safe),
        // so removeWindow() is a no-op for the window itself, but the
        // pruneEmptyLeaves() call inside removeWindowFromLayouts destroys
        // the orphaned empty leaf and reflows. tilingState().mode is
        // deliberately left as Tiled so the window re-joins on unmaximize.
        removeWindowFromLayouts(window);
        updateBorders();
        return;
    }

    // Unmaximized: re-join the tiling layout. This mirrors the
    // float->tile path in toggleFloating() / onWindowAdded(): a fresh
    // leaf is created, the window snaps to its tile geometry, and the
    // engine reflows.
    if (shouldTile(window) && !layoutEngineForWindow(window)) {
        LogicalOutput *output = window->output() ? window->output() : m_workspace->activeOutput();
        VirtualDesktop *desktop = window->desktops().isEmpty()
            ? VirtualDesktopManager::self()->currentDesktop(output)
            : window->desktops().constFirst();
        addWindowToLayout(window, output, desktop);
    }
    updateBorders();
}

void TilingController::onWindowFullScreenChanged(Window *window)
{
    if (!m_enabled || !window) {
        return;
    }

    // Fullscreen keeps the window attached to its tile, so layout membership
    // is deliberately left untouched — only the visuals change. updateBorders()
    // hides the border and resets the corner radius while fullscreen, and
    // restores them on exit. It also re-evaluates every other window, since
    // a fullscreen window may cover neighbors whose borders would otherwise
    // leak onto adjacent outputs.
    updateBorders();
}

void TilingController::addWindowToLayout(Window *window, LogicalOutput *output, VirtualDesktop *desktop)
{
    if (!output || !desktop) {
        return;
    }

    // Float-only workspace: the window stays floating. This is also the path a
    // window takes when it is moved onto a float-only workspace.
    if (isFloatOnly(output, desktop)) {
        window->tilingState().mode = TilingState::Mode::Floating;
        window->tilingState().userToggledFloat = false;
        return;
    }

    TileManager *manager = m_workspace->tileManager(output);
    if (!manager) {
        return;
    }

    setupLayoutEngine(manager, desktop, resolveLayoutKind(output, desktop));

    LayoutEngine *engine = manager->layoutEngine(desktop);
    if (!engine) {
        qWarning() << "TilingController: failed to obtain engine for output"
                   << output->name() << "desktop" << desktop->id();
        return;
    }

    // Remember the window's geometry before the engine snaps it to its tile.
    // If we later have to bail an ignored system surface (e.g.
    // xwaylandvideobridge) out of tiling after its WM_CLASS arrives, this
    // lets us restore the pre-snap geometry instead of leaving it stretched
    // over the output as a black box.
    m_preTileGeometry.insert(window, window->moveResizeGeometry());

    engine->addWindow(window);

    // If the window did not end up managed, surface the failure in logs but
    // do NOT silently flip the mode to Floating. The caller (migrateWindow or
    // onWindowAdded) is responsible for the mode, and the next desktop/output
    // change will trigger updateWindowVisibilityAndActivateOnDesktopChange to
    // re-evaluate the tile and snap the geometry correctly.
    if (!layoutEngineForWindow(window)) {
        qWarning() << "TilingController: window" << window->caption()
                   << "was not managed by any layout engine after addWindow; "
                      "leaving tilingState().mode untouched";
    }
}

void TilingController::removeWindowFromLayouts(Window *window)
{
    if (!m_workspace) {
        return;
    }

    for (LogicalOutput *output : m_workspace->outputs()) {
        TileManager *manager = m_workspace->tileManager(output);
        if (!manager) {
            continue;
        }
        for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
            if (LayoutEngine *engine = manager->layoutEngine(desktop)) {
                engine->removeWindow(window);
                // removeWindow only finds the window if it is still in a
                // leaf. Maximize detaches the window via Window::
                // exitQuickTileMode() -> Tile::forget() before this runs,
                // leaving an orphaned empty leaf; prune it so the layout
                // reflows to fill the gap. A no-op for minimize (the leaf
                // was already removed by removeWindow) and for windows that
                // were never in this engine.
                engine->pruneEmptyLeaves();
            }
        }
    }

    // Any active resize context for this window is now stale.
    m_activeResizes.remove(window);
}

void TilingController::onWindowClassChanged(Window *window)
{
    if (!m_enabled || !window) {
        return;
    }

    // Only interested in windows that were tiled before their class was
    // known. A window already Floating needs no correction; an ignored
    // window that is somehow not yet tiled is handled by onWindowAdded.
    if (window->tilingState().mode != TilingState::Mode::Tiled) {
        return;
    }

    // On native Wayland the window's rules are set up after onWindowAdded ran,
    // so re-check a float rule now that the class (and rules) are known. A
    // window-rule float overrides the tiling default.
    if (window->supportsWindowRules() && window->rules()->checkFloating(false, true)) {
        removeWindowFromLayouts(window);
        window->tilingState().mode = TilingState::Mode::Floating;
        auto it = m_preTileGeometry.find(window);
        if (it != m_preTileGeometry.end()) {
            const RectF preTileGeometry = it.value();
            m_preTileGeometry.erase(it);
            if (!preTileGeometry.isEmpty()) {
                window->moveResize(preTileGeometry);
            }
        }
        updateBorders();
        return;
    }

    // The class may only arrive now, after the column was laid out; apply a
    // per-app Emerge width while the column width is still untouched.
    if (auto *engine = qobject_cast<EmergeLayoutEngine *>(layoutEngineForWindow(window))) {
        engine->refreshWindowWidthRule(window);
    }

    // Now that the class has arrived, re-run the ignore rules. If the
    // window is ignored (e.g. xwaylandvideobridge), pull it out of every
    // layout and float it so the layout reflows to fill the freed slot.
    if (!m_rules->isIgnored(window)) {
        return;
    }

    removeWindowFromLayouts(window);
    window->tilingState().mode = TilingState::Mode::Floating;

    // Undo any maximized state and pin the documented-transparent surface to
    // opacity 0. This must run BEFORE restoring the pre-snap geometry below:
    // setMaximize(false, false) applies the geometry-restore rect itself, so
    // the explicit moveResize() afterwards is what actually leaves the window
    // at its intended pre-tile geometry.
    sanitizeVideoBridgeSurface(window);

    // If the window was adopted while its class was still unknown and snapped
    // to a fullscreen tile, restore its pre-snap geometry now. Otherwise it
    // is left floating but still stretched over the output as a black box.
    auto it = m_preTileGeometry.find(window);
    if (it != m_preTileGeometry.end()) {
        const RectF preTileGeometry = it.value();
        m_preTileGeometry.erase(it);
        if (!preTileGeometry.isEmpty()) {
            window->moveResize(preTileGeometry);
        }
    }

    updateBorders();
}

void TilingController::sanitizeVideoBridgeSurface(Window *window)
{
    if (!window) {
        return;
    }
    // Only the xwaylandvideobridge capture surface is documented to be
    // transparent and is touched here. This narrow predicate (NOT isIgnored(),
    // which also matches user IgnoreClass rules and non-client windows) means
    // any other window that lands here — this runs for every window's
    // opacityChanged/maximizedChanged — is left completely untouched.
    if (!m_rules->isVideoBridgeSurface(window)) {
        return;
    }

    // A maximized window is eligible for direct scanout, which bypasses the
    // compositor's opacity blending entirely: even an opacity-0 surface would
    // then scan out as an opaque black box. Restore it to its normal
    // (floating) state so the opacity pin below actually takes effect.
    // setMaximize() is a no-op when the window is not maximized, and it applies
    // the geometry-restore rect, so callers restore the intended geometry
    // afterwards.
    if (window->maximizeMode() != MaximizeRestore) {
        window->setMaximize(false, false);
    }

    // KWin reads _NET_WM_WINDOW_OPACITY once at map time. If the bridge set
    // the property after that read, Window::opacity() is stuck at 1.0 and the
    // invisible-by-design surface is composited as an opaque black box. Force
    // the intended transparency. This is idempotent (Window::setOpacity()
    // early-returns on no change, so calling this from opacityChanged cannot
    // recurse) and safe for capture, because the surface content is read via
    // X11, not via its on-screen pixels.
    if (window->opacity() != 0.0) {
        window->setOpacity(0.0);
    }
}

void TilingController::migrateWindow(Window *window, LogicalOutput *newOutput, VirtualDesktop *newDesktop)
{
    if (!m_workspace || !window || !newOutput || !newDesktop) {
        return;
    }

    // Find whichever engine currently owns this window, if any. The lookup is
    // an O(outputs * desktops) walk; that's acceptable because desktop/output
    // changes are infrequent (single keypresses).
    LogicalOutput *oldOutput = nullptr;
    VirtualDesktop *oldDesktop = nullptr;
    LayoutEngine *oldEngine = layoutEngineForWindow(window, &oldOutput, &oldDesktop);

    // Nothing to do if the window is already in the engine for the destination
    // (output, desktop) pair. Avoids accidental reflows on no-op changes.
    if (oldEngine && oldOutput == newOutput && oldDesktop == newDesktop) {
        return;
    }

    // Release the source: the engine reflows, so the remaining tiled windows
    // on the source desktop fill the freed slot.
    if (oldEngine) {
        oldEngine->removeWindow(window);
    }

    // Join the destination engine. addWindowToLayout creates the engine on
    // the new (output, desktop) if one doesn't exist yet, so the first time
    // a window ever lands on a desktop it still tiles correctly.
    addWindowToLayout(window, newOutput, newDesktop);
}

bool TilingController::shouldTile(const Window *window) const
{
    return window && window->tilingState().mode == TilingState::Mode::Tiled;
}

LayoutEngine *TilingController::activeLayoutEngine() const
{
    if (!m_workspace) {
        return nullptr;
    }

    LogicalOutput *output = m_workspace->activeOutput();
    if (!output) {
        return nullptr;
    }

    TileManager *manager = m_workspace->tileManager(output);
    if (!manager) {
        return nullptr;
    }

    return manager->layoutEngine();
}

EmergeLayoutEngine *TilingController::activeEmergeLayoutEngine() const
{
    return qobject_cast<EmergeLayoutEngine *>(activeLayoutEngine());
}

void TilingController::publishEmergeViewport()
{
    if (!m_dbusInterface) {
        return;
    }

    QString signature = QStringLiteral("none");
    if (EmergeLayoutEngine *engine = activeEmergeLayoutEngine()) {
        const EmergeViewportSnapshot snap = engine->viewportSnapshot();
        signature = QString::number(snap.activeIndex);
        for (int index : snap.columnsLeft) {
            signature += QLatin1Char('L') + QString::number(index);
        }
        for (int index : snap.columnsRight) {
            signature += QLatin1Char('R') + QString::number(index);
        }
    }

    if (signature == m_lastEmergeViewportSignature) {
        return;
    }
    m_lastEmergeViewportSignature = signature;
    Q_EMIT m_dbusInterface->emergeViewportChanged();
}

LayoutEngine *TilingController::layoutEngineForWindow(Window *window, LogicalOutput **output, VirtualDesktop **desktop) const
{
    if (!m_workspace || !window) {
        return nullptr;
    }

    for (LogicalOutput *out : m_workspace->outputs()) {
        TileManager *manager = m_workspace->tileManager(out);
        if (!manager) {
            continue;
        }
        for (VirtualDesktop *desk : VirtualDesktopManager::self()->desktops()) {
            if (LayoutEngine *engine = manager->layoutEngine(desk)) {
                if (engine->windows().contains(window)) {
                    if (output) {
                        *output = out;
                    }
                    if (desktop) {
                        *desktop = desk;
                    }
                    return engine;
                }
            }
        }
    }

    return nullptr;
}

Window *TilingController::activeTiledWindow() const
{
    Window *window = m_workspace ? m_workspace->activeWindow() : nullptr;
    if (window && shouldTile(window)) {
        return window;
    }
    return nullptr;
}

void TilingController::focusLeft()
{
    focusInDirection(LayoutEngine::FocusDirection::Left);
}

void TilingController::focusRight()
{
    focusInDirection(LayoutEngine::FocusDirection::Right);
}

void TilingController::focusUp()
{
    focusInDirection(LayoutEngine::FocusDirection::Up);
}

void TilingController::focusDown()
{
    focusInDirection(LayoutEngine::FocusDirection::Down);
}

void TilingController::focusInDirection(LayoutEngine::FocusDirection direction)
{
    Window *window = activeTiledWindow();
    LayoutEngine *engine = window ? layoutEngineForWindow(window) : activeLayoutEngine();
    if (!engine) {
        return;
    }

    Window *target = engine->windowInDirection(window, direction);
    if (target && m_workspace) {
        m_workspace->activateWindow(target);
    }
}

void TilingController::swapUp()
{
    swapInDirection(LayoutEngine::FocusDirection::Up);
}

void TilingController::swapDown()
{
    swapInDirection(LayoutEngine::FocusDirection::Down);
}

void TilingController::swapLeft()
{
    swapInDirection(LayoutEngine::FocusDirection::Left);
}

void TilingController::swapRight()
{
    swapInDirection(LayoutEngine::FocusDirection::Right);
}

void TilingController::swapInDirection(LayoutEngine::FocusDirection direction)
{
    Window *window = activeTiledWindow();
    if (!window) {
        return;
    }
    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine) {
        return;
    }

    Window *target = engine->windowInDirection(window, direction);
    if (!target || target == window) {
        return;
    }

    const QList<Window *> ws = engine->windows();
    const int fromIdx = ws.indexOf(window);
    const int targetIdx = ws.indexOf(target);
    if (fromIdx < 0 || targetIdx < 0) {
        return;
    }

    engine->moveWindow(window, targetIdx - fromIdx);
}

void TilingController::toggleFloating()
{
    Window *window = activeTiledWindow();
    if (!window) {
        window = m_workspace ? m_workspace->activeWindow() : nullptr;
    }
    if (!window) {
        return;
    }

    TilingState &state = window->tilingState();
    if (state.mode == TilingState::Mode::Tiled) {
        state.mode = TilingState::Mode::Floating;
        state.userToggledFloat = true;
        removeWindowFromLayouts(window);
        // Float at a default size centered under the cursor, respecting min/max.
        constexpr qreal defaultWidth = 800.0;
        constexpr qreal defaultHeight = 600.0;
        const QSizeF min = window->minSize();
        const QSizeF max = window->maxSize();
        qreal w = std::max(defaultWidth, min.width());
        qreal h = std::max(defaultHeight, min.height());
        if (max.width() > 0) {
            w = std::min(w, max.width());
        }
        if (max.height() > 0) {
            h = std::min(h, max.height());
        }
        const QPointF cursorPos = Cursors::self()->mouse()->pos();
        RectF geom(cursorPos.x() - w / 2, cursorPos.y() - h / 2, w, h);
        if (m_workspace) {
            const RectF screenArea = m_workspace->clientArea(PlacementArea, window);
            geom = window->keepInArea(geom, screenArea);
        }
        window->moveResize(geom);
    } else {
        state.mode = TilingState::Mode::Tiled;
        state.userToggledFloat = false;
        LogicalOutput *output = window->output() ? window->output() : m_workspace->activeOutput();
        VirtualDesktop *desktop = window->desktops().isEmpty()
            ? VirtualDesktopManager::self()->currentDesktop(output)
            : window->desktops().constFirst();
        addWindowToLayout(window, output, desktop);
    }

    updateBorders();
}

void TilingController::onInteractiveMoveResizeStarted()
{
    Window *window = qobject_cast<Window *>(sender());
    if (!window) {
        return;
    }
    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine) {
        return;
    }

    if (window->isInteractiveResize()) {
        // Resize path: capture the source leaf and the edge being dragged so
        // endInteractiveResize can convert the final pixel delta into a
        // weight delta on the engine.
        const Gravity gravity = window->interactiveMoveResizeGravity();
        Qt::Edge edge = Qt::RightEdge;
        switch (gravity) {
        case Gravity::Left:
        case Gravity::TopLeft:
        case Gravity::BottomLeft:
            edge = Qt::LeftEdge;
            break;
        case Gravity::Right:
        case Gravity::TopRight:
        case Gravity::BottomRight:
            edge = Qt::RightEdge;
            break;
        case Gravity::Top:
            edge = Qt::TopEdge;
            break;
        case Gravity::Bottom:
            edge = Qt::BottomEdge;
            break;
        case Gravity::None:
            // No specific edge; fall back to right (most common grab).
            edge = Qt::RightEdge;
            break;
        }

        ResizeContext context;
        context.engine = engine;
        context.originalGeometry = window->moveResizeGeometry();
        context.edge = edge;
        m_activeResizes[window] = context;
        return;
    }

    if (!window->isInteractiveMove()) {
        return;
    }

    // Remember where the window came from so we can restore/swap/clean up correctly.
    MoveContext context;
    context.engine = engine;
    context.output = window->output();
    context.desktop = window->desktops().isEmpty()
        ? VirtualDesktopManager::self()->currentDesktop(context.output)
        : window->desktops().constFirst();
    context.originalGeometryRestore = window->geometryRestore();
    m_activeMoves[window] = context;

    // Make the dragged preview smaller (600x800) so it doesn't obscure the
    // whole screen, while still respecting the window's own min/max size.
    constexpr qreal previewWidth = 600.0;
    constexpr qreal previewHeight = 800.0;
    const QSizeF min = window->minSize();
    const QSizeF max = window->maxSize();
    qreal w = std::max(previewWidth, min.width());
    qreal h = std::max(previewHeight, min.height());
    if (max.width() > 0) {
        w = std::min(w, max.width());
    }
    if (max.height() > 0) {
        h = std::min(h, max.height());
    }
    window->setGeometryRestore(RectF(0, 0, w, h));

    engine->beginMoveWindow(window);
}

void TilingController::onInteractiveMoveResizeFinished()
{
    Window *window = qobject_cast<Window *>(sender());
    if (!window) {
        return;
    }

    // Resize path takes precedence over move. If the user both moved and
    // resized in the same gesture, the resize context still applies.
    if (m_activeResizes.contains(window)) {
        const ResizeContext context = m_activeResizes.value(window);
        const RectF finalGeometry = window->moveResizeGeometry();
        endInteractiveResize(window, finalGeometry, context.edge);
        return;
    }

    onWindowMoveFinished(window);
}

void TilingController::onWindowMoveFinished(Window *window)
{
    if (!window || !m_workspace) {
        return;
    }
    // If the user explicitly floated the window, discard any move context.
    if (window->tilingState().mode != TilingState::Mode::Tiled) {
        m_activeMoves.remove(window);
        return;
    }

    auto it = m_activeMoves.find(window);
    if (it != m_activeMoves.end()) {
        MoveContext context = it.value();
        m_activeMoves.erase(it);

        LogicalOutput *currentOutput = window->output() ? window->output() : m_workspace->activeOutput();
        // Resolve the desktop the window is on at release time. During a drag
        // to another workspace (e.g. edge-switching while holding the window)
        // the desktop set changes, but onWindowDesktopsChanged deliberately
        // skips migration while the move is in progress. Resolve it here so
        // the window lands correctly on the destination workspace.
        VirtualDesktop *currentDesktop = window->desktops().isEmpty()
            ? VirtualDesktopManager::self()->currentDesktop(currentOutput)
            : window->desktops().constFirst();
        const bool movedToOtherOutput = context.output && currentOutput && context.output != currentOutput;
        // Only treat a desktop set change as a cross-workspace migration for
        // single-desktop (non on-all-desktops) windows, mirroring the guard in
        // onWindowDesktopsChanged().
        const bool movedToOtherDesktop = (!window->isOnAllDesktops() && window->desktops().size() == 1)
            && context.desktop && currentDesktop && context.desktop != currentDesktop;

        if ((movedToOtherOutput || movedToOtherDesktop) && context.engine) {
            // The window left its original output and/or workspace during the
            // drag. Clean up the empty source leaf and add it to the
            // destination (output, desktop) layout engine, mirroring what
            // onWindowDesktopsChanged()/migrateWindow() would have done if the
            // move had not been in progress.
            context.engine->cancelMoveWindow(window);

            // Resolve the drop target on the destination BEFORE adopting the
            // window: while it is detached it is not part of the destination
            // engine, so the cursor hit-test finds the window underneath
            // instead of the dragged one shadowing it.
            Window *target = nullptr;
            LayoutEngine::DropZone zone = LayoutEngine::DropZone::Swap;
            if (TileManager *manager = m_workspace->tileManager(currentOutput)) {
                if (LayoutEngine *destEngine = manager->layoutEngine(currentDesktop)) {
                    target = windowUnderCursorInEngine(destEngine);
                    zone = target ? dropZoneForTarget(target) : LayoutEngine::DropZone::Swap;
                }
            }

            addWindowToLayout(window, currentOutput, currentDesktop);

            // Place the window where it was dropped instead of appending it to
            // the end of the destination layout.
            if (target) {
                if (LayoutEngine *destEngine = layoutEngineForWindow(window)) {
                    destEngine->insertWindow(window, target, zone);
                }
            }
            window->setGeometryRestore(context.originalGeometryRestore);
            return;
        }

        if (context.engine) {
            Window *target = windowUnderCursorInEngine(context.engine);
            const LayoutEngine::DropZone zone = target ? dropZoneForTarget(target) : LayoutEngine::DropZone::Swap;
            if (context.engine->endMoveWindowOnZone(window, target, zone)) {
                window->setGeometryRestore(context.originalGeometryRestore);
                return;
            }
        }
        // Engine couldn't handle it (e.g. source tile destroyed); fall through.
    }

    // No move context or engine couldn't handle it: fall back to legacy snap-back.
    if (layoutEngineForWindow(window)) {
        return;
    }
    // The window is detached from every engine (e.g. an earlier interactive
    // resize freed it via Tile::unmanage()). Drop any orphaned empty leaf the
    // detach left behind before re-adding, otherwise addWindowToLayout() would
    // create a second leaf and the stale one would linger as a ghost tile.
    removeWindowFromLayouts(window);
    LogicalOutput *output = window->output() ? window->output() : m_workspace->activeOutput();
    VirtualDesktop *desktop = window->desktops().isEmpty()
        ? VirtualDesktopManager::self()->currentDesktop(output)
        : window->desktops().constFirst();
    addWindowToLayout(window, output, desktop);
}

void TilingController::onWindowDesktopsChanged(Window *window)
{
    if (!m_enabled || !m_workspace || !window) {
        return;
    }

    // Floating windows are not part of any layout engine; nothing to migrate.
    if (window->tilingState().mode != TilingState::Mode::Tiled) {
        return;
    }

    // On-all-desktops or multi-desktop windows stay managed by whatever
    // layout engine they were already in; the new desktop set is not a
    // request to re-tile.
    if (window->isOnAllDesktops() || window->desktops().size() != 1) {
        return;
    }

    // During an interactive move or resize the destination output/desktop will be
    // resolved on release (onWindowMoveFinished/endInteractiveResize); don't
    // migrate to an intermediate output/desktop while the user is still dragging.
    if (m_activeMoves.contains(window) || m_activeResizes.contains(window)) {
        return;
    }

    VirtualDesktop *newDesktop = window->desktops().constFirst();
    if (!newDesktop) {
        return;
    }

    LogicalOutput *output = window->output() ? window->output() : m_workspace->activeOutput();
    if (!output) {
        return;
    }

    // Use the shared migration helper so the desktop-change path is
    // identical to the monitor-change path. The helper finds the old
    // engine, releases the window from it (the source reflows), then adds
    // the window to the engine for the destination (output, desktop).
    migrateWindow(window, output, newDesktop);
}

Window *TilingController::windowUnderCursorInEngine(LayoutEngine *engine) const
{
    if (!m_workspace || !engine) {
        return nullptr;
    }
    const QPointF pos = Cursors::self()->mouse()->pos();
    const QList<Window *> &stacking = m_workspace->stackingOrder();
    for (auto it = stacking.rbegin(); it != stacking.rend(); ++it) {
        Window *window = *it;
        if (window->isDeleted()) {
            continue;
        }
        if (!window->isOnCurrentActivity() || !window->isOnCurrentDesktop()
            || window->isMinimized() || window->isHidden() || window->isHiddenByShowDesktop()) {
            continue;
        }
        if (window->hitTest(pos) && engine->windows().contains(window)) {
            return window;
        }
    }
    return nullptr;
}

LayoutEngine::DropZone TilingController::dropZoneForTarget(Window *target) const
{
    if (!target) {
        return LayoutEngine::DropZone::Swap;
    }
    const RectF geometry = target->frameGeometry();
    if (geometry.width() <= 0 || geometry.height() <= 0) {
        return LayoutEngine::DropZone::Swap;
    }
    const QPointF pos = Cursors::self()->mouse()->pos();
    // Cursor offset from the target's centre, normalised to -1..1 on each axis.
    const qreal dx = (pos.x() - geometry.center().x()) / (geometry.width() / 2.0);
    const qreal dy = (pos.y() - geometry.center().y()) / (geometry.height() / 2.0);

    // The middle of the window swaps; the edge the cursor is nearest to wins.
    // A horizontal/vertical tie goes to the horizontal edge so a horizontal
    // strip (Columns/Emerge) reads as left/right naturally.
    constexpr qreal deadZone = 0.5;
    if (qAbs(dx) < deadZone && qAbs(dy) < deadZone) {
        return LayoutEngine::DropZone::Swap;
    }
    if (qAbs(dx) >= qAbs(dy)) {
        return dx < 0.0 ? LayoutEngine::DropZone::InsertLeft : LayoutEngine::DropZone::InsertRight;
    }
    return dy < 0.0 ? LayoutEngine::DropZone::InsertAbove : LayoutEngine::DropZone::InsertBelow;
}

void TilingController::promoteToMaster()
{
    Window *window = activeTiledWindow();
    if (!window) {
        return;
    }

    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine) {
        return;
    }

    engine->moveWindow(window, -engine->windows().indexOf(window));
}

void TilingController::moveWindowNext()
{
    Window *window = activeTiledWindow();
    if (!window) {
        return;
    }
    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine) {
        return;
    }
    engine->moveWindow(window, +1);
}

void TilingController::moveWindowPrevious()
{
    Window *window = activeTiledWindow();
    if (!window) {
        return;
    }
    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine) {
        return;
    }
    engine->moveWindow(window, -1);
}

void TilingController::consume()
{
    Window *window = activeTiledWindow();
    if (!window) {
        return;
    }
    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine) {
        return;
    }
    engine->consumeWindow(window);
    updateBorders();
}

void TilingController::expel()
{
    Window *window = activeTiledWindow();
    if (!window) {
        return;
    }
    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine) {
        return;
    }
    engine->expelWindow(window);
    updateBorders();
}

void TilingController::switchPresetColumnWidth()
{
    LayoutEngine *engine = activeLayoutEngine();
    if (!engine) {
        return;
    }
    engine->switchPresetColumnWidth();
    updateBorders();
}

void TilingController::setLayout(LayoutEngine::LayoutKind kind)
{
    if (!m_workspace) {
        return;
    }

    LogicalOutput *output = m_workspace->activeOutput();
    if (!output) {
        return;
    }

    VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop(output);
    if (!desktop) {
        return;
    }

    setLayoutOn(output, desktop, kind);
    showLayoutNotification(kind);
}

void TilingController::setLayoutOn(LogicalOutput *output, VirtualDesktop *desktop, LayoutEngine::LayoutKind kind)
{
    if (!m_workspace || !output || !desktop) {
        return;
    }

    // If the user disabled this kind in the kcm, fall back to the first
    // enabled layout so setLayout / cycleLayout never silently do nothing.
    if (!isLayoutEnabled(kind)) {
        const QList<LayoutEngine::LayoutKind> enabled = enabledLayoutKinds();
        if (enabled.isEmpty()) {
            return;
        }
        kind = enabled.first();
    }

    TileManager *manager = m_workspace->tileManager(output);
    if (!manager) {
        return;
    }

    LayoutEngine *existing = manager->layoutEngine(desktop);
    if (!existing) {
        // No engine yet — just create one in the requested kind.
        setupLayoutEngine(manager, desktop, kind);
        Q_EMIT m_dbusInterface->layoutChanged(LayoutEngine::layoutKindToString(kind));
        publishEmergeViewport();
        return;
    }

    if (existing->layoutKind() == kind) {
        // Already the desired layout; nothing to do.
        return;
    }

    // Take ownership of the current windows so we can re-add them in the same
    // order once the new engine is in place.
    QList<Window *> carriedWindows = existing->windows();

    // Preserve the primary/master window position across layout switches.
    // Different layouts have different window ordering semantics: CenterTile
    // orders left stack → master column → right stack, while MasterStack
    // expects the master at index 0.  Reorder the carried windows so the old
    // engine's primary window comes first, keeping its master status in the
    // new layout.
    if (Window *primary = existing->primaryWindow()) {
        if (carriedWindows.size() > 1 && carriedWindows.first() != primary) {
            carriedWindows.removeOne(primary);
            carriedWindows.prepend(primary);
        }
    }

    auto engine = createLayoutEngine(kind, manager);
    engine->setMaxColumns(m_columnsMaxColumns);
    if (auto *sc = qobject_cast<EmergeLayoutEngine *>(engine.get())) {
        configureEmergeEngine(sc);
        connect(sc, &LayoutEngine::layoutChanged, this, &TilingController::publishEmergeViewport);
    }
    if (kind == LayoutEngine::LayoutKind::CenterTile) {
        if (auto *ct = qobject_cast<CenterTileLayoutEngine *>(engine.get())) {
            ct->setMasterRatio(m_lastCenterTileRatio);
            connect(ct, &CenterTileLayoutEngine::masterRatioChanged,
                    this, &TilingController::onCenterTileRatioChanged);
        }
    }
    manager->setLayoutEngine(desktop, std::move(engine));

    LayoutEngine *fresh = manager->layoutEngine(desktop);
    if (!fresh) {
        return;
    }

    for (Window *w : carriedWindows) {
        if (!w || w->isDeleted()) {
            continue;
        }
        fresh->addWindow(w);
    }

    Q_EMIT m_dbusInterface->layoutChanged(LayoutEngine::layoutKindToString(kind));
    publishEmergeViewport();
}

void TilingController::reconcileLayoutKinds()
{
    if (!m_enabled || !m_workspace) {
        return;
    }

    for (LogicalOutput *output : m_workspace->outputs()) {
        if (!output) {
            continue;
        }
        TileManager *manager = m_workspace->tileManager(output);
        if (!manager) {
            continue;
        }
        for (VirtualDesktop *desktop : VirtualDesktopManager::self()->desktops()) {
            if (!desktop) {
                continue;
            }
            const QString key = output->name() + QLatin1Char(':') + QString::number(desktop->x11DesktopNumber());

            // Float-only override (e.g. written by the settings page): float
            // the workspace and drop its engine. Clearing the reconciled kind
            // lets a later layout change rebuild the engine.
            if (isFloatOnly(output, desktop)) {
                if (manager->layoutEngine(desktop)) {
                    applyFloatOnly(output, desktop);
                }
                m_reconciledLayoutKinds.remove(key);
                continue;
            }

            // No engine for this (output, desktop): the float-only override
            // was just cleared or tiling was re-enabled globally. Rebuild it
            // and re-tile the windows the toggle floated.
            if (!manager->layoutEngine(desktop)) {
                const LayoutEngine::LayoutKind kind = resolveLayoutKind(output, desktop);
                setLayoutOn(output, desktop, kind);
                m_reconciledLayoutKinds.insert(key, kind);
                clearFloatOnly(output, desktop);
                continue;
            }

            // Only rebuild the engine when the *configured* default actually
            // changed. An unrelated reconfigure (e.g. toggling title bars)
            // must not discard a runtime layout switch made with setLayout()
            // or cycleLayout().
            const LayoutEngine::LayoutKind kind = resolveLayoutKind(output, desktop);
            if (m_reconciledLayoutKinds.contains(key) && m_reconciledLayoutKinds.value(key) == kind) {
                continue;
            }
            m_reconciledLayoutKinds.insert(key, kind);
            setLayoutOn(output, desktop, kind);
        }
    }
}

void TilingController::cycleLayout()
{
    if (!m_workspace) {
        return;
    }
    LogicalOutput *output = m_workspace->activeOutput();
    if (!output) {
        return;
    }
    VirtualDesktop *desktop = VirtualDesktopManager::self()->currentDesktop(output);
    if (!desktop) {
        return;
    }
    TileManager *manager = m_workspace->tileManager(output);
    if (!manager) {
        return;
    }

    const QList<LayoutEngine::LayoutKind> enabled = enabledLayoutKinds();
    if (enabled.size() < 2) {
        // Nothing to cycle through.
        return;
    }

    // Detect the kind of the engine currently attached to this (output, desktop)
    // pair so the cycle picks the *next* one rather than always the first.
    LayoutEngine::LayoutKind currentKind = globalDefaultLayoutKind();
    if (LayoutEngine *current = manager->layoutEngine(desktop)) {
        currentKind = current->layoutKind();
    }

    int currentIndex = enabled.indexOf(currentKind);
    if (currentIndex < 0) {
        currentIndex = 0;
    }
    const int nextIndex = (currentIndex + 1) % enabled.size();
    setLayout(enabled.at(nextIndex));
}

void TilingController::toggleOuterGaps()
{
    if (!m_workspace) {
        return;
    }
    LogicalOutput *output = m_workspace->activeOutput();
    if (!output) {
        return;
    }

    KSharedConfigPtr config = KSharedConfig::openConfig(KWIN_CONFIG);
    // Reparse so edits made from outside the compositor (e.g. the settings
    // page writing the file) are picked up before the presets are read.
    config->reparseConfiguration();
    KConfigGroup tilingGroup(config, QStringLiteral("Tiling"));
    const int wideGap = tilingGroup.readEntry("GapToggleWide", 15);
    const int centeredGap = tilingGroup.readEntry("GapToggleCentered", 390);

    // Two-state flip on the active monitor's outer (left/right) gaps: the
    // per-output override, falling back to the "wide" preset when none is
    // present. Top/bottom gaps are deliberately left untouched.
    KConfigGroup outputGroup(&tilingGroup, QStringLiteral("Output %1").arg(output->name()));
    const int currentGap = outputGroup.readEntry("GapLeft", wideGap);
    const int targetGap = (currentGap == centeredGap) ? wideGap : centeredGap;
    outputGroup.writeEntry("GapLeft", targetGap);
    outputGroup.writeEntry("GapRight", targetGap);
    config->sync();

    applyGapSettingsToOutput(output);
}

void TilingController::showLayoutNotification(LayoutEngine::LayoutKind kind)
{
    // Skip notifications in test mode to avoid DBus warnings in unit tests.
    if (QStandardPaths::isTestModeEnabled()) {
        return;
    }

    const QString text = LayoutEngine::layoutDisplayName(kind);
    // The icon name must be a valid themed icon. "preferences-desktop-tiling"
    // is not part of the standard Breeze set, so we fall back to a generic
    // window-management icon that ships with Plasma.
    const QString iconName = QStringLiteral("kwin");

    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.plasmashell"),
        QStringLiteral("/org/kde/osdService"),
        QStringLiteral("org.kde.osdService"),
        QStringLiteral("showText"));
    message.setArguments({iconName, text});

    QDBusConnection::sessionBus().asyncCall(message);
}

void TilingController::moveWindowToOutput(TilingDirection direction)
{
    Window *window = activeTiledWindow();
    if (!window || !m_workspace) {
        return;
    }

    LogicalOutput *currentOutput = window->output();
    if (!currentOutput) {
        currentOutput = m_workspace->activeOutput();
    }

    Workspace::Direction workspaceDirection = (direction == TilingDirection::West) ? Workspace::DirectionWest : Workspace::DirectionEast;
    LogicalOutput *targetOutput = m_workspace->findOutput(currentOutput, workspaceDirection, true);
    if (!targetOutput || targetOutput == currentOutput) {
        return;
    }

    // Capture the desktop before sendToOutput, because the helper's lookup of
    // the old engine needs to find the engine on (oldOutput, oldDesktop).
    // The window is moved to the same desktop it was on, just on the new
    // output. If the window was on all desktops or had no desktop, fall back
    // to the target output's current desktop.
    VirtualDesktop *desktop = window->desktops().isEmpty()
        ? VirtualDesktopManager::self()->currentDesktop(targetOutput)
        : window->desktops().constFirst();
    if (!desktop) {
        return;
    }

    // Capture the source engine while the window is still attached. Because a
    // tiled window reports requestedQuickTileMode() == Custom, sendToOutput()
    // detaches it from its CustomTile via TileManager::forgetWindow() but
    // leaves the now-empty leaf in the source engine. layoutEngineForWindow()
    // can no longer find the window afterwards, so migrateWindow() would skip
    // removing that leaf and the source reflow would keep reserving a slot for
    // it — leaving a ghost/empty tile on the old output.
    LayoutEngine *sourceEngine = layoutEngineForWindow(window);

    // Move window to target output, preserving desktop membership.
    window->sendToOutput(targetOutput);

    // Prune the orphaned empty leaf so the source layout reflows and fills the
    // gap. removeWindow() is a no-op when sendToOutput() already detached the
    // window, but still handles the case where it did not.
    if (sourceEngine) {
        sourceEngine->removeWindow(window);
        sourceEngine->pruneEmptyLeaves();
    }

    // Use the shared migration helper so monitor moves and desktop moves
    // follow the exact same engine-swap path. The helper is idempotent for
    // no-op moves.
    migrateWindow(window, targetOutput, desktop);
}

void TilingController::growActiveTileSize(Qt::Orientation axis)
{
    if (!m_enabled) {
        return;
    }
    Window *window = activeTiledWindow();
    if (!window) {
        return;
    }
    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine || !engine->supportsPerTileResize()) {
        return;
    }
    engine->adjustTileSize(window, m_resizeStep, axis);
}

void TilingController::shrinkActiveTileSize(Qt::Orientation axis)
{
    if (!m_enabled) {
        return;
    }
    Window *window = activeTiledWindow();
    if (!window) {
        return;
    }
    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine || !engine->supportsPerTileResize()) {
        return;
    }
    engine->adjustTileSize(window, -m_resizeStep, axis);
}

void TilingController::beginInteractiveResize(Window *window)
{
    if (!window || !m_workspace) {
        return;
    }
    LayoutEngine *engine = layoutEngineForWindow(window);
    if (!engine || !engine->supportsPerTileResize()) {
        return;
    }

    ResizeContext context;
    context.engine = engine;
    context.originalGeometry = window->moveResizeGeometry();
    // Default to right edge; the caller (onInteractiveMoveResizeStarted) is
    // expected to update this with the actual grabbed edge before invoking
    // this helper. We set RightEdge as a safe fallback so an uninitialised
    // context still produces a sensible delta calculation.
    context.edge = Qt::RightEdge;
    m_activeResizes[window] = context;
}

void TilingController::endInteractiveResize(Window *window, const RectF &finalGeometry, Qt::Edge edge)
{
    if (!window || !m_workspace) {
        m_activeResizes.remove(window);
        return;
    }
    auto it = m_activeResizes.find(window);
    if (it == m_activeResizes.end()) {
        return;
    }
    ResizeContext context = it.value();
    m_activeResizes.erase(it);

    // An interactive resize can detach the window from its leaf: when the user
    // grabs a handle that would push past the output edge,
    // CustomTile::supportsResizeGravity() returns false and
    // Window::updateInteractiveMoveResize() calls
    // setQuickTileModeAtCurrentPosition(None) -> Tile::unmanage(). That removes
    // the window from its leaf but leaves the now-empty leaf in the engine, so
    // reflow() keeps reserving its slot — and every later move/resize that
    // re-adds the window leaves a ghost tile behind. Prune the orphaned leaf now
    // so the layout reflows to fill the freed space immediately.
    if (!layoutEngineForWindow(window)) {
        removeWindowFromLayouts(window);
        updateBorders();
        return;
    }

    if (!context.engine || !context.engine->supportsPerTileResize()) {
        return;
    }

    const RectF before = context.originalGeometry;
    const RectF after = finalGeometry;
    if (before.size() == QSizeF() || after.size() == QSizeF()) {
        return;
    }

    LogicalOutput *output = window->output() ? window->output() : m_workspace->activeOutput();
    if (!output) {
        return;
    }
    const QSizeF outputSize = output->geometry().size();
    if (outputSize.width() <= 0 || outputSize.height() <= 0) {
        return;
    }

    // Delegate to the engine's edge-aware handler, which knows how to
    // interpret the geometry change based on which column the window is in
    // and which edge was grabbed. The engine handles:
    //   - Correct delta sign for non-master columns (MasterStack, CenterTile)
    //   - Push/pull vertical resize between neighbours (all engines)
    //   - Screen-edge no-ops
    // The reflow inside the engine will call leaf->setRelativeGeometry(...)
    // which in turn calls w->moveResize(windowGeometry()) for every window
    // in the leaf — that is the snap-back to the tiled geometry.
    context.engine->interactiveResizeEnded(window, before, after, edge, outputSize);
}

} // namespace KWin
