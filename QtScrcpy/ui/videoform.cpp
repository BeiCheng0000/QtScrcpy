// #include <QDesktopWidget>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QShortcut>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <QtWidgets/QHBoxLayout>

#if defined(Q_OS_WIN32)
#include <Windows.h>
#endif

#include "audio/audiooutput.h"
#include "config.h"
#include "iconhelper.h"
#include "qyuvopenglwidget.h"
#include "recorder/videorecorder.h"
#include "toolform.h"
#include "mousetap/mousetap.h"
#include "ui_videoform.h"
#include "videoform.h"

#ifdef Q_OS_MACOS
#include "metalvideowindow.h"
#endif

VideoForm::VideoForm(bool framelessWindow, bool skin, bool showToolbar, int decodeMode, QWidget *parent) : QWidget(parent), ui(new Ui::videoForm), m_skin(skin), m_decodeMode(decodeMode)
{
    ui->setupUi(this);
    m_flexResizeTimer.setSingleShot(true);
    m_flexResizeTimer.setInterval(300);
    connect(&m_flexResizeTimer, &QTimer::timeout, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device && device->isFlexDisplay() && !m_pendingDisplaySize.isEmpty()) {
            device->resizeDisplay(m_pendingDisplaySize);
        }
    });
    initUI();
    installShortcut();
    updateShowSize(size());
    bool vertical = size().height() > size().width();
    this->show_toolbar = showToolbar;
    if (m_skin) {
        updateStyleSheet(vertical);
    }
    if (framelessWindow) {
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    }

    // 监听显示器增删：副屏被拔掉时，位于该屏幕上的窗口会变成"幽灵窗口"
    // （坐标仍在已断开的屏幕区域）。这里在屏幕配置变化后做一次越界检查，
    // 必要时把窗口移回主屏。延迟执行是为了等 Qt/系统完成屏幕布局更新。
    //
    // Windows 上 screenRemoved 的触发时机与屏幕布局更新并不完全同步，
    // 因此做多次延迟重试，确保在布局稳定后仍能完成回收。
    connect(qApp, &QGuiApplication::screenAdded, this, [this]() {
        QTimer::singleShot(300, this, [this]() { ensureOnScreen(); });
        QTimer::singleShot(1000, this, [this]() { ensureOnScreen(); });
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this]() {
        QTimer::singleShot(300, this, [this]() { ensureOnScreen(); });
        QTimer::singleShot(1000, this, [this]() { ensureOnScreen(); });
        QTimer::singleShot(2000, this, [this]() { ensureOnScreen(); });
    });
}

VideoForm::~VideoForm()
{
    m_closing = true;
    if (!m_recordAudioKey.isEmpty()) {
        AudioOutput::instance().removePcmListener(m_recordAudioKey);
        m_recordAudioKey.clear();
    }
    if (m_recorder) {
        // Do not notify the destroyed UI while flushing the last packets.
        disconnect(m_recorder, nullptr, this, nullptr);
        m_recorder->stop();
    }
    delete ui;
}

bool VideoForm::isMetalMode() const
{
#ifdef Q_OS_MACOS
    return !m_metalWidget.isNull();
#else
    return false;
#endif
}

QWidget* VideoForm::videoWidget() const
{
#ifdef Q_OS_MACOS
    if (isMetalMode()) {
        return m_metalWidget.data();
    }
#endif
    return m_videoWidget.data();
}

void VideoForm::initUI()
{
    if (m_skin) {
        QPixmap phone;
        if (phone.load(":/res/phone.png")) {
            m_widthHeightRatio = 1.0f * phone.width() / phone.height();
        }

#ifndef Q_OS_MACOS
        // mac下去掉标题栏影响showfullscreen
        // 去掉标题栏
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        // 根据图片构造异形窗口
        setAttribute(Qt::WA_TranslucentBackground);
#endif
    }

#ifdef Q_OS_MACOS
    // Apple Silicon: 使用 VideoToolbox + Metal 渲染
    if (m_decodeMode == 1) {
        m_metalWidget = new MetalVideoWidget();
        ui->keepRatioWidget->setWidget(m_metalWidget);

        // FPS label 作为 Metal widget 的子控件
        m_fpsLabel = new QLabel(m_metalWidget);
    } else
#endif
    {
        // OpenGL 路径（原有逻辑）
        m_videoWidget = new QYUVOpenGLWidget();
        m_videoWidget->hide();
        ui->keepRatioWidget->setWidget(m_videoWidget);

        // FPS label 作为 OpenGL widget 的子控件
        m_fpsLabel = new QLabel(m_videoWidget);
    }

    ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);

    QFont ft;
    ft.setPointSize(15);
    ft.setWeight(QFont::Light);
    ft.setBold(true);
    m_fpsLabel->setFont(ft);
    m_fpsLabel->move(5, 15);
    m_fpsLabel->setMinimumWidth(100);
    m_fpsLabel->setStyleSheet(R"(QLabel {color: #00FF00;})");

    setMouseTracking(true);
    if (m_videoWidget) {
        m_videoWidget->setMouseTracking(true);
    }
    ui->keepRatioWidget->setMouseTracking(true);
}

QRect VideoForm::getGrabCursorRect()
{
    QRect rc;
    QWidget *vw = videoWidget();
#if defined(Q_OS_WIN32)
    rc = QRect(ui->keepRatioWidget->mapToGlobal(vw->pos()), vw->size());
    // high dpi support
    rc.setTopLeft(rc.topLeft() * vw->devicePixelRatioF());
    rc.setBottomRight(rc.bottomRight() * vw->devicePixelRatioF());

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#elif defined(Q_OS_MACOS)
    rc = vw->geometry();
    rc.setTopLeft(ui->keepRatioWidget->mapToGlobal(rc.topLeft()));
    rc.setBottomRight(ui->keepRatioWidget->mapToGlobal(rc.bottomRight()));

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#elif defined(Q_OS_LINUX)
    rc = QRect(ui->keepRatioWidget->mapToGlobal(vw->pos()), vw->size());
    // high dpi support -- taken from the WIN32 section and untested
    rc.setTopLeft(rc.topLeft() * vw->devicePixelRatioF());
    rc.setBottomRight(rc.bottomRight() * vw->devicePixelRatioF());

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#endif
    return rc;
}

const QSize &VideoForm::frameSize()
{
    return m_frameSize;
}

void VideoForm::resizeSquare()
{
    QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        qWarning() << "getScreenRect is empty";
        return;
    }
    resize(screenRect.height(), screenRect.height());
}

void VideoForm::removeBlackRect()
{
    resize(ui->keepRatioWidget->goodSize());
}

void VideoForm::showFPS(bool show)
{
    if (!m_fpsLabel) {
        return;
    }
    m_fpsLabel->setVisible(show);
}

void VideoForm::updateRender(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV)
{
    if (isMetalMode()) {
        // Metal 路径不通过此方法渲染，使用 onFrameMetal
        return;
    }

    if (!m_videoWidget) {
        return;
    }

    if (m_videoWidget->isHidden()) {
        if (m_loadingWidget) {
            m_loadingWidget->close();
        }
        m_videoWidget->show();
    }

    if (!m_flexDisplay) {
        updateShowSize(QSize(width, height));
    } else {
        m_frameSize = QSize(width, height);
    }
    m_videoWidget->setFrameSize(QSize(width, height));
    m_videoWidget->updateTextures(dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
}

void VideoForm::setSerial(const QString &serial)
{
    m_serial = serial;
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    m_flexDisplay = device && device->isFlexDisplay();
    if (m_flexDisplay) {
        ui->keepRatioWidget->setWidthHeightRatio(-1.0f);
    }
}

void VideoForm::showToolForm(bool show)
{
    if (!m_toolForm) {
        m_toolForm = new ToolForm(this, ToolForm::AP_OUTSIDE_RIGHT);
        m_toolForm->setSerial(m_serial);
    }
    m_toolForm->move(pos().x() + geometry().width(), pos().y() + 30);
    m_toolForm->setVisible(show);
}

void VideoForm::moveCenter()
{
    QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        qWarning() << "getScreenRect is empty";
        return;
    }
    // 窗口居中
    move(screenRect.center() - QRect(0, 0, size().width(), size().height()).center());
}

bool VideoForm::isRectOnAnyScreen(const QRect &rect) const
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (!screen) {
            continue;
        }
        // 用 intersects 而非 contains：窗口只要有一部分可见就认为在屏内，
        // 允许用户把窗口放在屏幕边缘（部分露出）的情况。
        if (screen->availableGeometry().intersects(rect)) {
            return true;
        }
    }
    return false;
}

void VideoForm::ensureOnScreen()
{
    // 全屏状态下由系统接管，不必干预
    if (isFullScreen()) {
        return;
    }

    // 使用 frameGeometry 而非 geometry：前者包含窗口边框，
    // 更能反映窗口真实占用的屏幕区域。
    const QRect curRect = frameGeometry();
    if (isRectOnAnyScreen(curRect)) {
        return;
    }

    // 当前窗口已完全落在所有屏幕之外（典型场景：副屏被拔掉后
    // 窗口坐标仍停留在已断开的屏幕区域）。移回主屏并居中。
    //
    // 注意：此处不能直接调用 moveCenter()。因为 getScreenRect() 会优先
    // 取窗口当前所属的 QScreen，而窗口此时正位于已失效的屏幕区域，
    // 可能得到错误的目标位置。因此这里显式取主屏的可用区域来计算。
    qWarning() << "window is off-screen, moving back to primary screen. rect =" << curRect;

    QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary) {
        qWarning() << "no primary screen available, skip off-screen recovery";
        return;
    }

    const QRect avail = primary->availableGeometry();
    if (avail.isEmpty()) {
        return;
    }

    const QPoint target = avail.center() - QRect(0, 0, size().width(), size().height()).center();
    move(target);
    qWarning() << "window recovered to" << target;
}

void VideoForm::installShortcut()
{
    QShortcut *shortcut = nullptr;

    // record screen
    shortcut = new QShortcut(QKeySequence("Ctrl+Alt+r"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        if (isRecording()) {
            stopRecord();
        } else {
            startRecord();
        }
    });

    // switchFullScreen
    shortcut = new QShortcut(QKeySequence("Ctrl+f"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        switchFullScreen();
    });

    // resizeSquare
    shortcut = new QShortcut(QKeySequence("Ctrl+g"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { resizeSquare(); });

    // removeBlackRect
    shortcut = new QShortcut(QKeySequence("Ctrl+w"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { removeBlackRect(); });

    // postGoHome
    shortcut = new QShortcut(QKeySequence("Ctrl+h"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoHome();
    });

    // postGoBack
    shortcut = new QShortcut(QKeySequence("Ctrl+b"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoBack();
    });

    // postAppSwitch
    shortcut = new QShortcut(QKeySequence("Ctrl+s"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postAppSwitch();
    });

    // postGoMenu
    shortcut = new QShortcut(QKeySequence("Ctrl+m"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoMenu();
    });

    // postVolumeUp
    shortcut = new QShortcut(QKeySequence("Ctrl+up"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeUp();
    });

    // postVolumeDown
    shortcut = new QShortcut(QKeySequence("Ctrl+down"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeDown();
    });

    // postPower
    shortcut = new QShortcut(QKeySequence("Ctrl+p"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postPower();
    });

    shortcut = new QShortcut(QKeySequence("Ctrl+o"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->setDisplayPower(false);
    });

    // expandNotificationPanel
    shortcut = new QShortcut(QKeySequence("Ctrl+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->expandNotificationPanel();
    });

    shortcut = new QShortcut(QKeySequence("Ctrl+Alt+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            device->expandSettingsPanel();
        }
    });

    shortcut = new QShortcut(QKeySequence("Ctrl+r"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            device->rotateDevice();
        }
    });

    // collapsePanel
    shortcut = new QShortcut(QKeySequence("Ctrl+Shift+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->collapsePanel();
    });

    // copy
    shortcut = new QShortcut(QKeySequence("Ctrl+c"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCopy();
    });

    // cut
    shortcut = new QShortcut(QKeySequence("Ctrl+x"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCut();
    });

    // clipboardPaste
    shortcut = new QShortcut(QKeySequence("Ctrl+v"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->setDeviceClipboard();
    });

    // setDeviceClipboard
    shortcut = new QShortcut(QKeySequence("Ctrl+Shift+v"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->clipboardPaste();
    });
}

QRect VideoForm::getScreenRect()
{
    QRect screenRect;
    QScreen *screen = QGuiApplication::primaryScreen();
    QWidget *win = window();
    if (win) {
        QWindow *winHandle = win->windowHandle();
        if (winHandle) {
            screen = winHandle->screen();
        }
    }

    if (screen) {
        screenRect = screen->availableGeometry();
    }
    return screenRect;
}

void VideoForm::updateStyleSheet(bool vertical)
{
    if (vertical) {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/image/videoform/phone-v.png) 150px 65px 85px 65px;
                     border-width: 150px 65px 85px 65px;
                 }
                 )");
    } else {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/image/videoform/phone-h.png) 65px 85px 65px 150px;
                     border-width: 65px 85px 65px 150px;
                 }
                 )");
    }
    layout()->setContentsMargins(getMargins(vertical));
}

QMargins VideoForm::getMargins(bool vertical)
{
    QMargins margins;
    if (vertical) {
        margins = QMargins(10, 68, 12, 62);
    } else {
        margins = QMargins(68, 12, 62, 10);
    }
    return margins;
}

QSize VideoForm::fittedWindowSize(bool fillScreen)
{
    if (m_frameSize.isEmpty() || m_widthHeightRatio <= 0.0f) {
        return QSize();
    }
    const QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        return QSize();
    }

    const bool vertical = m_widthHeightRatio < 1.0f;
    // Skin margins are drawn as the phone frame; they are part of the window
    // but not part of the picture, so exclude them from the available area.
    const QMargins skin = m_skin ? getMargins(vertical) : QMargins();
    const int availW = screenRect.width() - skin.left() - skin.right();
    const int availH = screenRect.height() - skin.top() - skin.bottom();
    if (availW <= 0 || availH <= 0) {
        return QSize();
    }

    QSize videoSize;
    if (fillScreen) {
        // "Maximize" should grow the picture, not the black borders: take the
        // largest rectangle with the device aspect ratio that still fits the
        // screen's available geometry.
        const int maxW = qMax(1, availW - 20);
        const int maxH = qMax(1, availH - 20);
        int w = maxW;
        int h = static_cast<int>(w / m_widthHeightRatio + 0.5);
        if (h > maxH) {
            h = maxH;
            w = static_cast<int>(h * m_widthHeightRatio + 0.5);
        }
        videoSize = QSize(w, h);
    } else if (vertical) {
        const int h = qMin(m_frameSize.height(), screenRect.height() - 200);
        videoSize = QSize(static_cast<int>(h * m_widthHeightRatio + 0.5), h);
    } else {
        const int w = qMin(m_frameSize.width(), screenRect.width() / 2);
        videoSize = QSize(w, static_cast<int>(w / m_widthHeightRatio + 0.5));
    }

    QSize windowSize(videoSize.width() + skin.left() + skin.right(),
                     videoSize.height() + skin.top() + skin.bottom());
    windowSize.setWidth(qMin(windowSize.width(), screenRect.width()));
    windowSize.setHeight(qMin(windowSize.height(), screenRect.height()));
    return windowSize;
}

bool VideoForm::hasExcessiveLetterbox() const
{
    const QSize container = ui->keepRatioWidget->size();
    const QSize video = ui->keepRatioWidget->goodSize();
    if (container.isEmpty() || video.isEmpty()) {
        return false;
    }
    const qreal fill = qreal(video.width()) * video.height() /
                       (qreal(container.width()) * container.height());
    // The phone skin occupies the margins, so a healthy window is nearly
    // 100% picture; anything below 70% is a visible empty band.
    return fill < 0.70;
}

void VideoForm::fitWindowToFrame(bool fillScreen)
{
    if (isMinimized() || isFullScreen()) {
        return;
    }
    const bool wasMaximized = isMaximized();
    if (wasMaximized) {
        showNormal();
    }
    const QSize fitted = fittedWindowSize(fillScreen);
    if (fitted.isEmpty()) {
        return;
    }

    // resizeEvent used to pin a minimum size derived from the maximized
    // video; clear it so the window may shrink back to the fitted size.
    setMinimumSize(0, 0);
    if (!wasMaximized && fitted == size()) {
        return;
    }
    resize(fitted);
    if (m_skin) {
        updateStyleSheet(m_widthHeightRatio < 1.0f);
    }
    moveCenter();
}

void VideoForm::updateShowSize(const QSize &newSize)
{
    // Also called from the frame callbacks: a same-size call must stay a
    // no-op, otherwise the window would fight any manual resize by the user.
    if (m_frameSize == newSize) {
        return;
    }
    m_frameSize = newSize;

    m_widthHeightRatio = 1.0f * newSize.width() / newSize.height();
    ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);

    if (isFullScreen() && qsc::IDeviceManage::getInstance().getDevice(m_serial)) {
        switchFullScreen();
    }

    if (isMaximized()) {
        showNormal();
    }
    fitWindowToFrame(false);
}

void VideoForm::onVideoSessionChanged(const QSize &size, bool clientResized)
{
    if (m_flexDisplay) {
        m_frameSize = size;
        m_preventAutoResize = clientResized;
        ui->keepRatioWidget->setWidthHeightRatio(-1.0f);
        return;
    }
    // clientResized is only meaningful for flex display. Normal display
    // rotations must retain the longstanding auto-resize behavior.
    m_preventAutoResize = false;
    updateShowSize(size);
}

void VideoForm::switchFullScreen()
{
    if (isFullScreen()) {
        // 横屏全屏铺满全屏，恢复时，恢复保持宽高比
        if (m_widthHeightRatio > 1.0f) {
            ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);
        }

        showNormal();
        // back to normal size.
        resize(m_normalSize);
        // fullscreen window will move (0,0). qt bug?
        move(m_fullScreenBeforePos);

#ifdef Q_OS_MACOS
        //setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        //show();
#endif
        if (m_skin) {
            updateStyleSheet(m_frameSize.height() > m_frameSize.width());
        }
        showToolForm(this->show_toolbar);
#ifdef Q_OS_WIN32
        ::SetThreadExecutionState(ES_CONTINUOUS);
#endif
    } else {
        // 横屏全屏铺满全屏，不保持宽高比
        if (m_widthHeightRatio > 1.0f) {
            ui->keepRatioWidget->setWidthHeightRatio(-1.0f);
        }

        // record current size before fullscreen, it will be used to rollback size after exit fullscreen.
        m_normalSize = size();

        m_fullScreenBeforePos = pos();
        // 这种临时增加标题栏再全屏的方案会导致收不到mousemove事件，导致setmousetrack失效
        // mac fullscreen must show title bar
#ifdef Q_OS_MACOS
        //setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
#endif
        showToolForm(false);
        if (m_skin) {
            layout()->setContentsMargins(0, 0, 0, 0);
        }
        showFullScreen();

        // 全屏状态禁止电脑休眠、息屏
#ifdef Q_OS_WIN32
        ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif
    }
}

bool VideoForm::isHost()
{
    if (!m_toolForm) {
        return false;
    }
    return m_toolForm->isHost();
}

void VideoForm::updateFPS(quint32 fps)
{
    if (!m_fpsLabel) {
        return;
    }
    m_fpsLabel->setText(QString("FPS:%1").arg(fps));
}

void VideoForm::grabCursor(bool grab)
{
    QRect rc = getGrabCursorRect();
    MouseTap::getInstance()->enableMouseEventTap(rc, grab);
}

bool VideoForm::isRecording() const
{
    return m_recorder && m_recorder->isRecording();
}

bool VideoForm::isRecordingSupported() const
{
    // The Metal path delivers CVPixelBufferRef instead of YUV planes.
    return !isMetalMode();
}

QString VideoForm::buildRecordFilePath() const
{
    UserBootConfig config = Config::getInstance().getUserBootConfig();
    QString dir = config.recordPath.trimmed();
    if (dir.isEmpty()) {
        QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (documents.isEmpty()) {
            documents = QDir::homePath();
        }
        dir = documents + "/QtScrcpy";
    }
    if (!QDir().mkpath(dir)) {
        qWarning() << "cannot create record directory" << dir;
        return QString();
    }

    QString serial = m_serial;
    serial.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    if (serial.isEmpty()) {
        serial = QStringLiteral("device");
    }

    const QString base =
        QStringLiteral("QtScrcpy_%1_%2").arg(serial, QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString filePath = dir + "/" + base + ".mp4";
    int index = 1;
    while (QFileInfo::exists(filePath)) {
        filePath = dir + "/" + base + "_" + QString::number(index++) + ".mp4";
    }
    return filePath;
}

bool VideoForm::startRecord()
{
    if (isRecording()) {
        return true;
    }
    if (!isRecordingSupported()) {
        QMessageBox::warning(this, tr("record screen"),
                             tr("Recording is not supported with the Metal decoder, please switch the decode mode."));
        return false;
    }
    if (m_frameSize.isEmpty()) {
        QMessageBox::warning(this, tr("record screen"), tr("No video frame is available yet."));
        return false;
    }

    const QString filePath = buildRecordFilePath();
    if (filePath.isEmpty()) {
        QMessageBox::warning(this, tr("record screen"), tr("Cannot create the recording directory."));
        return false;
    }

    UserBootConfig config = Config::getInstance().getUserBootConfig();
    bool withAudio = config.recordAudio;
    if (withAudio && !AudioOutput::instance().isRunning()) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("record screen"),
            tr("Recording sound starts the sndcpy audio bridge, the device screen may switch to sndcpy for a moment.\n"
               "Record with sound?\n\nChoose No to record video only."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            withAudio = AudioOutput::instance().start(m_serial, 28200);
            if (!withAudio) {
                QMessageBox::warning(this, tr("record screen"),
                                     tr("Cannot start the audio bridge, recording video only."));
            }
        } else {
            withAudio = false;
        }
    }

    VideoRecorder *recorder = new VideoRecorder(this);
    connect(recorder, &VideoRecorder::finished, this, &VideoForm::onRecordingFinished);
    connect(recorder, &VideoRecorder::errorOccurred, this, &VideoForm::onRecordingError);

    const bool ok = recorder->start(filePath, m_frameSize.width(), m_frameSize.height(), config.recordFps,
                                   config.recordVideoBitrate, withAudio ? 48000 : 0, 2, config.recordAudioBitrate);
    if (!ok) {
        // errorOccurred() already reported the reason through onRecordingError()
        return false;
    }

    m_recorder = recorder;
    if (withAudio) {
        m_recordAudioKey = QStringLiteral("record:") + m_serial;
        AudioOutput::instance().addPcmListener(m_recordAudioKey, [recorder](const char *data, int size) {
            recorder->pushAudioData(data, size);
        });
    }

    qInfo() << "recording started:" << filePath << "with audio:" << withAudio;
    emit recordingStateChanged();
    return true;
}

void VideoForm::stopRecord()
{
    if (!m_recorder) {
        return;
    }
    if (!m_recordAudioKey.isEmpty()) {
        // Remove the sink before stopping so the socket thread cannot push into
        // a recorder that is being torn down.
        AudioOutput::instance().removePcmListener(m_recordAudioKey);
        m_recordAudioKey.clear();
    }
    // stop() is idempotent and also joins a worker that already exited after
    // an encoding error.
    m_recorder->stop();
    emit recordingStateChanged();
}

void VideoForm::onRecordingFinished(const QString &filePath, qint64 durationMs, qint64 bytes, bool hasAudio)
{
    qInfo() << "recording saved:" << filePath << "duration(ms):" << durationMs << "bytes:" << bytes
            << "audio:" << hasAudio;

    if (auto *recorder = qobject_cast<VideoRecorder *>(sender())) {
        recorder->deleteLater();
    }
    emit recordingStateChanged();

    if (m_closing) {
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(tr("recording finished"));
    box.setText(tr("Saved to:\n%1").arg(QDir::toNativeSeparators(filePath)));
    box.setInformativeText(tr("Duration: %1 s\nSize: %2 MB\nAudio: %3")
                               .arg(durationMs / 1000)
                               .arg(bytes / 1024.0 / 1024.0, 0, 'f', 1)
                               .arg(hasAudio ? tr("yes") : tr("no")));
    QPushButton *openButton = box.addButton(tr("open folder"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Ok);
    box.exec();
    if (box.clickedButton() == openButton) {
        openRecordFolder(filePath);
    }
}

void VideoForm::onRecordingError(const QString &error)
{
    qWarning() << "recording failed:" << error;

    if (auto *recorder = qobject_cast<VideoRecorder *>(sender())) {
        recorder->deleteLater();
    }
    if (!m_recordAudioKey.isEmpty()) {
        AudioOutput::instance().removePcmListener(m_recordAudioKey);
        m_recordAudioKey.clear();
    }
    emit recordingStateChanged();

    if (m_closing) {
        return;
    }
    QMessageBox::warning(this, tr("record screen"), tr("Recording failed: %1").arg(error));
}

void VideoForm::openRecordFolder(const QString &filePath) const
{
    const QString dir = QFileInfo(filePath).absolutePath();
#if defined(Q_OS_WIN32)
    if (!QProcess::startDetached(QStringLiteral("explorer.exe"),
                                 QStringList{QStringLiteral("/select,%1").arg(QDir::toNativeSeparators(filePath))})) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    }
#elif defined(Q_OS_MACOS)
    if (!QProcess::startDetached(QStringLiteral("open"), QStringList{QStringLiteral("-R"), filePath})) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    }
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
}

void VideoForm::onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV, int linesizeY, int linesizeU, int linesizeV)
{
    updateRender(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
    if (m_recorder && m_recorder->isRecording()) {
        m_recorder->pushVideoFrame(dataY, dataU, dataV, width, height, linesizeY, linesizeU, linesizeV);
    }
}

void VideoForm::onFrameMetal(void *cvPixelBuffer, int width, int height)
{
#ifdef Q_OS_MACOS
    if (!m_metalWidget || !cvPixelBuffer) {
        return;
    }

    if (m_metalFirstFrame) {
        m_metalFirstFrame = false;
        if (m_loadingWidget) {
            m_loadingWidget->close();
        }
        ui->keepRatioWidget->updateGeometry();
    }

    updateShowSize(QSize(width, height));
    m_metalWidget->renderFrame((CVPixelBufferRef)cvPixelBuffer, width, height);
#else
    Q_UNUSED(cvPixelBuffer);
    Q_UNUSED(width);
    Q_UNUSED(height);
#endif
}

void VideoForm::staysOnTop(bool top)
{
    bool needShow = false;
    if (isVisible()) {
        needShow = true;
    }
    setWindowFlag(Qt::WindowStaysOnTopHint, top);
    if (m_toolForm) {
        m_toolForm->setWindowFlag(Qt::WindowStaysOnTopHint, top);
    }
    if (needShow) {
        show();
    }
}

void VideoForm::mousePressEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (event->button() == Qt::MiddleButton) {
        if (device && !device->isCurrentCustomKeymap()) {
            device->postGoHome();
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        if (device && !device->isCurrentCustomKeymap()) {
            device->postGoBack();
            return;
        }
    }

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif

    QWidget *vw = videoWidget();
    if (vw && vw->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
        QPointF mappedPos = vw->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_frameSize, vw->size());

        // debug keymap pos
        if (event->button() == Qt::LeftButton) {
            qreal x = localPos.x() / vw->size().width();
            qreal y = localPos.y() / vw->size().height();
            QString posTip = QString(R"("pos": {"x": %1, "y": %2})").arg(x).arg(y);
            qInfo() << posTip.toStdString().c_str();
        }
    } else {
        if (event->button() == Qt::LeftButton) {
            m_dragPosition = globalPos.toPoint() - frameGeometry().topLeft();
            event->accept();
        }
    }
}

void VideoForm::mouseReleaseEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (m_dragPosition.isNull()) {
        if (!device) {
            return;
        }
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
        QWidget *vw = videoWidget();
        if (!vw) {
            return;
        }

        // local check
        QPointF local = vw->mapFrom(this, localPos.toPoint());
        if (local.x() < 0) {
            local.setX(0);
        }
        if (local.x() > vw->width()) {
            local.setX(vw->width());
        }
        if (local.y() < 0) {
            local.setY(0);
        }
        if (local.y() > vw->height()) {
            local.setY(vw->height());
        }
        QMouseEvent newEvent(event->type(), local, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_frameSize, vw->size());
    } else {
        m_dragPosition = QPoint(0, 0);
    }
}

void VideoForm::mouseMoveEvent(QMouseEvent *event)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    QWidget *vw = videoWidget();
    if (vw && vw->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
        QPointF mappedPos = vw->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_frameSize, vw->size());
    } else if (!m_dragPosition.isNull()) {
        if (event->buttons() & Qt::LeftButton) {
            move(globalPos.toPoint() - m_dragPosition);
            event->accept();
        }
    }
}

void VideoForm::mouseDoubleClickEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    QWidget *vw = videoWidget();
    if (event->button() == Qt::LeftButton && vw && !vw->geometry().contains(event->pos())) {
        if (!isMaximized()) {
            removeBlackRect();
        }
    }

    if (event->button() == Qt::RightButton && device && !device->isCurrentCustomKeymap()) {
        emit device->postBackOrScreenOn(event->type() == QEvent::MouseButtonPress);
    }

    if (vw && vw->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
        QPointF mappedPos = vw->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_frameSize, vw->size());
    }
}

void VideoForm::wheelEvent(QWheelEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    QWidget *vw = videoWidget();
    if (!vw) {
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    if (vw->geometry().contains(event->position().toPoint())) {
        if (!device) {
            return;
        }
        QPointF pos = vw->mapFrom(this, event->position().toPoint());
        QWheelEvent wheelEvent(
            pos, event->globalPosition(), event->pixelDelta(), event->angleDelta(), event->buttons(), event->modifiers(), event->phase(), event->inverted());
#else
    if (vw->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
        QPointF pos = vw->mapFrom(this, event->pos());

        QWheelEvent wheelEvent(
            pos, event->globalPosF(), event->pixelDelta(), event->angleDelta(), event->delta(), event->orientation(),
            event->buttons(), event->modifiers(), event->phase(), event->source(), event->inverted());
#endif
        emit device->wheelEvent(&wheelEvent, m_frameSize, vw->size());
    }
}

void VideoForm::keyPressEvent(QKeyEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    if (Qt::Key_Escape == event->key() && !event->isAutoRepeat() && isFullScreen()) {
        switchFullScreen();
    }

    QWidget *vw = videoWidget();
    QSize widgetSize = vw ? vw->size() : m_frameSize;
    emit device->keyEvent(event, m_frameSize, widgetSize);
}

void VideoForm::keyReleaseEvent(QKeyEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    QWidget *vw = videoWidget();
    QSize widgetSize = vw ? vw->size() : m_frameSize;
    emit device->keyEvent(event, m_frameSize, widgetSize);
}

void VideoForm::paintEvent(QPaintEvent *paint)
{
    Q_UNUSED(paint)
    QStyleOption opt;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    opt.init(this);
#else
    opt.initFrom(this);
#endif
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void VideoForm::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (m_stateAdjusting || event->type() != QEvent::WindowStateChange) {
        return;
    }
    if (isFullScreen() || isMinimized()) {
        return;  // Full screen / minimize are handled by their own paths.
    }
    const auto *stateEvent = static_cast<QWindowStateChangeEvent *>(event);
    const bool wasMax = stateEvent->oldState() & Qt::WindowMaximized;
    const bool isMax = windowState() & Qt::WindowMaximized;

    if (isMax && !m_smartMaximized) {
        // First maximize click: remember the geometry to restore, then grow the
        // picture to the largest size the screen can hold instead of leaving
        // huge black bands. The window stays in the normal state, so the native
        // maximize/restore button keeps working as a real toggle.
        m_restoredGeometry = normalGeometry();
        m_smartMaximized = true;
        m_stateAdjusting = true;
        showNormal();
        fitWindowToFrame(true);
        m_stateAdjusting = false;
    } else if (isMax && m_smartMaximized) {
        // Second maximize click while already grown: restore the pre-maximize
        // geometry so the button behaves like a real maximize/restore toggle.
        m_smartMaximized = false;
        m_stateAdjusting = true;
        showNormal();
        if (m_restoredGeometry.isValid()) {
            setGeometry(m_restoredGeometry);
        } else {
            fitWindowToFrame(false);
        }
        if (m_skin) {
            updateStyleSheet(m_widthHeightRatio < 1.0f);
        }
        m_stateAdjusting = false;
    } else if (wasMax) {
        // Leaving a real maximized state via the restore button.
        m_smartMaximized = false;
        m_stateAdjusting = true;
        if (m_restoredGeometry.isValid()) {
            setGeometry(m_restoredGeometry);
        }
        if (m_skin) {
            updateStyleSheet(m_widthHeightRatio < 1.0f);
        }
        m_stateAdjusting = false;
    } else if (hasExcessiveLetterbox()) {
        fitWindowToFrame(false);
    }
}

void VideoForm::showEvent(QShowEvent *event)
{
    Q_UNUSED(event)
    // 窗口每次显示时先做一次越界检查，处理"上次退出时窗口位于副屏、
    // 本次启动副屏已不存在"导致窗口出现在屏幕外的情况。
    //
    // Dialog::onDeviceConnected 会先 setGeometry(savedRect) 再 show()，
    // 因此这里能捕获到从配置中恢复出来的屏外坐标。
    ensureOnScreen();
    // 窗口显示后 Qt 仍可能根据尺寸/边框策略再做一次位置调整，
    // 故稍后复查一次，确保最终位置落在屏内。
    QTimer::singleShot(300, this, [this]() {
        ensureOnScreen();
        // A geometry saved from a maximized session can be far larger than the
        // picture; shrink it back instead of showing huge black bars.
        if (!isMaximized() && !isFullScreen() && !m_closing && hasExcessiveLetterbox()) {
            fitWindowToFrame(false);
        }
    });
    if (!isFullScreen() && this->show_toolbar) {
        QTimer::singleShot(500, this, [this](){
            showToolForm(this->show_toolbar);
        });
    }
}

void VideoForm::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event)
    if (m_flexDisplay) {
        m_pendingDisplaySize = ui->keepRatioWidget->size();
        if (!m_pendingDisplaySize.isEmpty()) {
            m_flexResizeTimer.start();
        }
        return;
    }

    // KeepRatioWidget scales the picture to whatever space is available, so
    // no minimum window size is needed. The old logic pinned a minimum derived
    // from the *previous* (maximized) video size and prevented the window from
    // shrinking back -- that was the source of the huge black borders after
    // maximize/minimize.
}

void VideoForm::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event)
    // Flush a running recording first: the completion dialog tells the user
    // where the file was saved before the window goes away.
    if (m_recorder) {
        stopRecord();
    }
    m_closing = true;

    // The sndcpy bridge is bound to this device: closing its window must also
    // stop audio playback (and tear down the forward/capture activity).
    AudioOutput::instance().stopForDevice(m_serial);

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    // Never persist the maximized geometry: restoring it on the next connect
    // would recreate an oversized window with large letterbox areas.
    const QRect savedRect = isMaximized() ? normalGeometry() : geometry();
    Config::getInstance().setRect(device->getSerial(), savedRect);
    device->disconnectDevice();
}

void VideoForm::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

void VideoForm::dragMoveEvent(QDragMoveEvent *event)
{
    Q_UNUSED(event)
}

void VideoForm::dragLeaveEvent(QDragLeaveEvent *event)
{
    Q_UNUSED(event)
}

void VideoForm::dropEvent(QDropEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    const QMimeData *qm = event->mimeData();
    QList<QUrl> urls = qm->urls();

    for (const QUrl &url : urls) {
        QString file = url.toLocalFile();
        QFileInfo fileInfo(file);

        if (!fileInfo.exists()) {
            QMessageBox::warning(this, "QtScrcpy", tr("file does not exist"), QMessageBox::Ok);
            continue;
        }

        if (fileInfo.isFile() && fileInfo.suffix() == "apk") {
            emit device->installApkRequest(file);
            continue;
        }
        emit device->pushFileRequest(file, Config::getInstance().getPushFilePath() + fileInfo.fileName());
    }
}
