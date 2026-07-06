/*
    KWin - the KDE window manager
    SPDX-FileCopyrightText: 2026 KWin Tiling Fork

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "core/output.h"
#include "pointer_input.h"
#include "tiles/autogridlayoutengine.h"
#include "tiles/customtile.h"
#include "tiles/layoutengine.h"
#include "tiles/tilemanager.h"
#include "tiling/tilingcontroller.h"
#include "tiling/tilingstate.h"
#include "virtualdesktops.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <KConfigGroup>
#include <KSharedConfig>

namespace KWin
{

/**
 * Integration tests for the AutoGrid layout engine.
 *
 * AutoGrid places windows in the smallest near-square grid frame that holds
 * them (2x2, 3x2, 3x3, 4x3, ...). While N is below the frame's cell count
 * the first window absorbs the unused cells by spanning the top of the left
 * column; at perfect grids every cell equalizes. N=4 is ordered clockwise
 * TL / TR / BR / BL (intentional, inherited from the original layout).
 *
 * The tests verify:
 *   - The LayoutKind round-trips through the string helpers.
 *   - setLayout(AutoGrid) creates the engine.
 *   - gridGeometries() matches the documented slot table for N=1..12.
 *   - Live window geometries match the grid for N=1..4.
 */
class TilingAutoGridTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testLayoutKindRegistered();
    void testSetLayoutCreatesEngine();
    void testGridGeometriesTable();
    void testSingleWindowTakesFullScreen();
    void testTwoWindowsSplitHalves();
    void testThreeWindowsLeftColumnSpans();
    void testFourWindowsQuartered();

private:
    void createTiledWindow(std::unique_ptr<KWayland::Client::Surface> *surface,
                           std::unique_ptr<Test::XdgToplevel> *shellSurface,
                           Window **window);

    std::unique_ptr<KWayland::Client::Surface> m_surface;
    std::unique_ptr<KWayland::Client::Surface> m_surface2;
    std::unique_ptr<KWayland::Client::Surface> m_surface3;
    std::unique_ptr<KWayland::Client::Surface> m_surface4;
    std::unique_ptr<Test::XdgToplevel> m_shellSurface;
    std::unique_ptr<Test::XdgToplevel> m_shellSurface2;
    std::unique_ptr<Test::XdgToplevel> m_shellSurface3;
    std::unique_ptr<Test::XdgToplevel> m_shellSurface4;
    Window *m_window = nullptr;
    Window *m_window2 = nullptr;
    Window *m_window3 = nullptr;
    Window *m_window4 = nullptr;

    LogicalOutput *m_output = nullptr;
    TileManager *m_tileManager = nullptr;
    VirtualDesktop *m_desktop = nullptr;
};

void TilingAutoGridTest::initTestCase()
{
    qRegisterMetaType<KWin::Window *>();
    QVERIFY(waylandServer()->init(qAppName()));

    kwinApp()->start();
    Test::setOutputConfig({
        Rect(0, 0, 1280, 1024),
    });
    setenv("QT_QPA_PLATFORM", "wayland", true);
}

void TilingAutoGridTest::init()
{
    QVERIFY(Test::setupWaylandConnection());

    workspace()->setActiveOutput(QPoint(640, 512));
    input()->pointer()->warp(QPoint(640, 512));

    m_output = workspace()->activeOutput();
    m_tileManager = workspace()->tileManager(m_output);
    QVERIFY(m_tileManager);

    VirtualDesktopManager::self()->setCount(1);
    m_desktop = VirtualDesktopManager::self()->desktops().at(0);
    VirtualDesktopManager::self()->setCurrent(m_desktop);

    TilingController *controller = workspace()->tilingController();
    QVERIFY(controller);
    controller->setLayout(LayoutEngine::LayoutKind::AutoGrid);
}

void TilingAutoGridTest::cleanup()
{
    m_surface.reset();
    m_surface2.reset();
    m_surface3.reset();
    m_surface4.reset();
    m_shellSurface.reset();
    m_shellSurface2.reset();
    m_shellSurface3.reset();
    m_shellSurface4.reset();
    m_window = nullptr;
    m_window2 = nullptr;
    m_window3 = nullptr;
    m_window4 = nullptr;

    Test::destroyWaylandConnection();
}

void TilingAutoGridTest::createTiledWindow(std::unique_ptr<KWayland::Client::Surface> *surface,
                                           std::unique_ptr<Test::XdgToplevel> *shellSurface,
                                           Window **window)
{
    *surface = Test::createSurface();
    QVERIFY(*surface);
    *shellSurface = Test::createXdgToplevelSurface(surface->get());
    QVERIFY(*shellSurface);

    *window = Test::renderAndWaitForShown(surface->get(), QSize(400, 300), Qt::blue);
    QVERIFY(*window);

    // Wait for the window to be actually tiled by the controller.
    QTRY_VERIFY((*window)->tilingState().mode == TilingState::Mode::Tiled);
}

void TilingAutoGridTest::testLayoutKindRegistered()
{
    const QString name = LayoutEngine::layoutKindToString(LayoutEngine::LayoutKind::AutoGrid);
    QCOMPARE(name, QStringLiteral("AutoGrid"));
    QCOMPARE(LayoutEngine::layoutKindFromString(QStringLiteral("AutoGrid")),
             LayoutEngine::LayoutKind::AutoGrid);
    QCOMPARE(LayoutEngine::layoutDisplayName(LayoutEngine::LayoutKind::AutoGrid),
             QStringLiteral("Auto Grid"));
}

void TilingAutoGridTest::testSetLayoutCreatesEngine()
{
    TilingController *controller = workspace()->tilingController();
    QVERIFY(controller);

    controller->setLayout(LayoutEngine::LayoutKind::AutoGrid);
    LayoutEngine *engine = m_tileManager->layoutEngine(m_desktop);
    QVERIFY(engine);
    QCOMPARE(engine->layoutKind(), LayoutEngine::LayoutKind::AutoGrid);
}

void TilingAutoGridTest::testGridGeometriesTable()
{
    auto expectRect = [](const RectF &got, qreal x, qreal y, qreal w, qreal h, int n, int slot) {
        const bool ok = qFuzzyCompare(1.0 + got.x(), 1.0 + x)
            && qFuzzyCompare(1.0 + got.y(), 1.0 + y)
            && qFuzzyCompare(got.width(), w)
            && qFuzzyCompare(got.height(), h);
        QVERIFY2(ok, qPrintable(QStringLiteral("N=%1 slot %2: got (%3, %4, %5, %6), want (%7, %8, %9, %10)")
                                    .arg(n).arg(slot)
                                    .arg(got.x()).arg(got.y()).arg(got.width()).arg(got.height())
                                    .arg(x).arg(y).arg(w).arg(h)));
    };
    const qreal third = 1.0 / 3.0;

    // Every N covers the full area with non-overlapping slots; spot-check
    // the signature shapes from the original layout's documented table.

    // N=1: full area.
    QList<RectF> g = AutoGridLayoutEngine::gridGeometries(1);
    QCOMPARE(g.count(), 1);
    expectRect(g[0], 0, 0, 1, 1, 1, 0);

    // N=2: left half / right half.
    g = AutoGridLayoutEngine::gridGeometries(2);
    QCOMPARE(g.count(), 2);
    expectRect(g[0], 0, 0, 0.5, 1, 2, 0);
    expectRect(g[1], 0.5, 0, 0.5, 1, 2, 1);

    // N=3: 2x2 frame, W1 spans the full left column.
    g = AutoGridLayoutEngine::gridGeometries(3);
    QCOMPARE(g.count(), 3);
    expectRect(g[0], 0, 0, 0.5, 1, 3, 0);
    expectRect(g[1], 0.5, 0, 0.5, 0.5, 3, 1);
    expectRect(g[2], 0.5, 0.5, 0.5, 0.5, 3, 2);

    // N=4: quartered, clockwise TL / TR / BR / BL.
    g = AutoGridLayoutEngine::gridGeometries(4);
    QCOMPARE(g.count(), 4);
    expectRect(g[0], 0, 0, 0.5, 0.5, 4, 0);
    expectRect(g[1], 0.5, 0, 0.5, 0.5, 4, 1);
    expectRect(g[2], 0.5, 0.5, 0.5, 0.5, 4, 2);
    expectRect(g[3], 0, 0.5, 0.5, 0.5, 4, 3);

    // N=5: 3x2 frame, W1 spans the full left column, W2..W5 fill the right 2x2.
    g = AutoGridLayoutEngine::gridGeometries(5);
    QCOMPARE(g.count(), 5);
    expectRect(g[0], 0, 0, third, 1, 5, 0);
    expectRect(g[1], third, 0, third, 0.5, 5, 1);
    expectRect(g[2], 2 * third, 0, third, 0.5, 5, 2);
    expectRect(g[3], third, 0.5, third, 0.5, 5, 3);
    expectRect(g[4], 2 * third, 0.5, third, 0.5, 5, 4);

    // N=6: perfect 3x2, row-major.
    g = AutoGridLayoutEngine::gridGeometries(6);
    QCOMPARE(g.count(), 6);
    expectRect(g[0], 0, 0, third, 0.5, 6, 0);
    expectRect(g[3], 0, 0.5, third, 0.5, 6, 3);
    expectRect(g[5], 2 * third, 0.5, third, 0.5, 6, 5);

    // N=7: 3x3 frame, W1 spans the full left column.
    g = AutoGridLayoutEngine::gridGeometries(7);
    QCOMPARE(g.count(), 7);
    expectRect(g[0], 0, 0, third, 1, 7, 0);
    expectRect(g[1], third, 0, third, third, 7, 1);
    expectRect(g[6], 2 * third, 2 * third, third, third, 7, 6);

    // N=8: 3x3 frame, W1 spans the top 2 rows of the left column; the bottom
    // row is a normal 3-cell strip.
    g = AutoGridLayoutEngine::gridGeometries(8);
    QCOMPARE(g.count(), 8);
    expectRect(g[0], 0, 0, third, 2 * third, 8, 0);
    expectRect(g[5], 0, 2 * third, third, third, 8, 5);
    expectRect(g[7], 2 * third, 2 * third, third, third, 8, 7);

    // N=9: perfect 3x3, row-major.
    g = AutoGridLayoutEngine::gridGeometries(9);
    QCOMPARE(g.count(), 9);
    expectRect(g[0], 0, 0, third, third, 9, 0);
    expectRect(g[4], third, third, third, third, 9, 4);
    expectRect(g[8], 2 * third, 2 * third, third, third, 9, 8);

    // N=10: 4x3 frame, W1 spans the full left column.
    g = AutoGridLayoutEngine::gridGeometries(10);
    QCOMPARE(g.count(), 10);
    expectRect(g[0], 0, 0, 0.25, 1, 10, 0);
    expectRect(g[1], 0.25, 0, 0.25, third, 10, 1);
    expectRect(g[9], 0.75, 2 * third, 0.25, third, 10, 9);

    // N=11: 4x3 frame, W1 spans the top 2 rows of the left column.
    g = AutoGridLayoutEngine::gridGeometries(11);
    QCOMPARE(g.count(), 11);
    expectRect(g[0], 0, 0, 0.25, 2 * third, 11, 0);
    expectRect(g[7], 0, 2 * third, 0.25, third, 11, 7);

    // N=12: perfect 4x3, row-major.
    g = AutoGridLayoutEngine::gridGeometries(12);
    QCOMPARE(g.count(), 12);
    expectRect(g[0], 0, 0, 0.25, third, 12, 0);
    expectRect(g[11], 0.75, 2 * third, 0.25, third, 12, 11);

    // Past the original cap of 12, the frame progression keeps going: N=13
    // is a 4x4 frame with W1 spanning the full left column.
    g = AutoGridLayoutEngine::gridGeometries(13);
    QCOMPARE(g.count(), 13);
    expectRect(g[0], 0, 0, 0.25, 1, 13, 0);
}

void TilingAutoGridTest::testSingleWindowTakesFullScreen()
{
    createTiledWindow(&m_surface, &m_shellSurface, &m_window);
    QVERIFY(m_window);

    const QRectF g = m_window->moveResizeGeometry();
    QVERIFY2(qFuzzyCompare(g.width(), qreal(1280)),
             qPrintable(QStringLiteral("Single-window width %1 != 1280").arg(g.width())));
    QVERIFY2(qFuzzyCompare(g.height(), qreal(1024)),
             qPrintable(QStringLiteral("Single-window height %1 != 1024").arg(g.height())));
}

void TilingAutoGridTest::testTwoWindowsSplitHalves()
{
    createTiledWindow(&m_surface, &m_shellSurface, &m_window);
    createTiledWindow(&m_surface2, &m_shellSurface2, &m_window2);
    QVERIFY(m_window && m_window2);

    const QRectF g1 = m_window->moveResizeGeometry();
    const QRectF g2 = m_window2->moveResizeGeometry();
    // Left half / right half, both full height.
    QVERIFY2(qFuzzyCompare(g1.width(), qreal(640)),
             qPrintable(QStringLiteral("First width %1 != 640").arg(g1.width())));
    QVERIFY2(qFuzzyCompare(g2.width(), qreal(640)),
             qPrintable(QStringLiteral("Second width %1 != 640").arg(g2.width())));
    QVERIFY2(qFuzzyCompare(g1.height(), qreal(1024)) && qFuzzyCompare(g2.height(), qreal(1024)),
             qPrintable(QStringLiteral("Heights %1 / %2 != 1024").arg(g1.height()).arg(g2.height())));
    QVERIFY2(g1.left() < g2.left(),
             qPrintable(QStringLiteral("First window %1 not left of second %2").arg(g1.left()).arg(g2.left())));
}

void TilingAutoGridTest::testThreeWindowsLeftColumnSpans()
{
    createTiledWindow(&m_surface, &m_shellSurface, &m_window);
    createTiledWindow(&m_surface2, &m_shellSurface2, &m_window2);
    createTiledWindow(&m_surface3, &m_shellSurface3, &m_window3);
    QVERIFY(m_window && m_window2 && m_window3);

    const QRectF g1 = m_window->moveResizeGeometry();
    const QRectF g2 = m_window2->moveResizeGeometry();
    const QRectF g3 = m_window3->moveResizeGeometry();
    // W1 spans the full left column; W2 / W3 stack in the right column.
    QVERIFY2(qFuzzyCompare(g1.height(), qreal(1024)),
             qPrintable(QStringLiteral("W1 height %1 != 1024").arg(g1.height())));
    QVERIFY2(qFuzzyCompare(g2.height(), qreal(512)) && qFuzzyCompare(g3.height(), qreal(512)),
             qPrintable(QStringLiteral("W2/W3 heights %1 / %2 != 512").arg(g2.height()).arg(g3.height())));
    QVERIFY2(g2.top() < g3.top(),
             qPrintable(QStringLiteral("W2 %1 not above W3 %2").arg(g2.top()).arg(g3.top())));
}

void TilingAutoGridTest::testFourWindowsQuartered()
{
    createTiledWindow(&m_surface, &m_shellSurface, &m_window);
    createTiledWindow(&m_surface2, &m_shellSurface2, &m_window2);
    createTiledWindow(&m_surface3, &m_shellSurface3, &m_window3);
    createTiledWindow(&m_surface4, &m_shellSurface4, &m_window4);
    QVERIFY(m_window && m_window2 && m_window3 && m_window4);

    const QRectF g1 = m_window->moveResizeGeometry();
    const QRectF g2 = m_window2->moveResizeGeometry();
    const QRectF g3 = m_window3->moveResizeGeometry();
    const QRectF g4 = m_window4->moveResizeGeometry();

    // All quarters.
    for (const QRectF &g : {g1, g2, g3, g4}) {
        QVERIFY2(qFuzzyCompare(g.width(), qreal(640)) && qFuzzyCompare(g.height(), qreal(512)),
                 qPrintable(QStringLiteral("Quarter is (%1 x %2), want 640 x 512").arg(g.width()).arg(g.height())));
    }
    // Clockwise order: TL, TR, BR, BL.
    QVERIFY(g1.top() < g3.top() && g1.left() < g2.left());
    QVERIFY(g2.top() < g3.top() && g2.left() > g1.left());
    QVERIFY(g3.top() > g2.top() && g3.left() > g4.left());
    QVERIFY(g4.top() > g1.top() && g4.left() < g3.left());
}

} // namespace KWin

WAYLANDTEST_MAIN(KWin::TilingAutoGridTest)
#include "tiling_autogrid_test.moc"
