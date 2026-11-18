// #include <QDesktopWidget>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QCursor>
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
#include <QVariantAnimation>
#include <QWindow>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#if defined(Q_OS_WIN32)
#include <Windows.h>
#endif

#include "apkpathutils.h"
#include "audio/audiooutput.h"
#include "config.h"
#include "iconhelper.h"
#include "loadingoverlay.h"
#include "qyuvopenglwidget.h"
#include "recorder/videorecorder.h"
#include "toolform.h"
#include "mousetap/mousetap.h"
#include "adbprocess.h"
#include "ui_videoform.h"
#include "videoform.h"

#ifdef Q_OS_MACOS
#include "metalvideowindow.h"
#endif

VideoForm::VideoForm(bool framelessWindow, bool showToolbar, int decodeMode, QWidget *parent) : QWidget(parent), ui(new Ui::videoForm), m_decodeMode(decodeMode)
{
    ui->setupUi(this);
    m_flexResizeTimer.setSingleShot(true);
    m_flexResizeTimer.setInterval(300);
    // 全屏模式：轮询全局光标实现“贴屏幕右缘唤出工具栏，移出即收起”
    m_fullscreenToolbarTimer.setInterval(80);
    connect(&m_fullscreenToolbarTimer, &QTimer::timeout, this, &VideoForm::updateFullscreenToolbar);
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
    Q_UNUSED(vertical);
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
        AudioOutput::instance().removePcmListener(m_serial, m_recordAudioKey);
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
        // 真实帧数据上传完成才撤掉加载遮罩：仅凭“首帧回调”不够，
        // 此时纹理里可能还是零填充数据（渲染为绿色占位屏）。
        connect(m_videoWidget, &QYUVOpenGLWidget::frameUpdated, this, [this]() {
            if (auto *overlay = qobject_cast<LoadingOverlay *>(m_loadingWidget.data())) {
                overlay->fadeOutAndDelete();
            }
        });
    }
    ui->keepRatioWidget->setMouseTracking(true);

    // 连接过程中的加载遮罩（方案A：手机轮廓+数据流，三阶段递进动画）：
    // 首帧画面到达前覆盖画面区域，避免窗口显示成一块空背景。
    // 注意 keepRatioWidget 是 QScrollArea，直接挂成它的子控件在滚动区上显示/置顶不可靠，
    // 因此挂在它的同级父控件上，用其 geometry() 对齐覆盖。
    m_loadingWidget = new LoadingOverlay(ui->keepRatioWidget->parentWidget());
    m_loadingWidget->setObjectName(QStringLiteral("loadingOverlay"));
    // 立即置为可见：窗口显示时遮罩随之出现，不依赖 showEvent 的调用顺序。
    // 几何尺寸由 syncLoadingOverlay 在 showEvent/resizeEvent 中同步。
    syncLoadingOverlay();
    m_loadingWidget->show();
}

void VideoForm::syncLoadingOverlay()
{
    if (m_loadingWidget) {
        m_loadingWidget->setGeometry(ui->keepRatioWidget->geometry());
        m_loadingWidget->raise();
    }
}

QRect VideoForm::getGrabCursorRect()
{
    QRect rc;
    QWidget *vw = videoWidget();
#if defined(Q_OS_WIN32)
    rc = QRect(ui->keepRatioWidget->mapToGlobal(vw->pos()), vw->size());
    // 高 DPI 支持
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
    // 高 DPI 支持——取自 WIN32 分支，未经测试
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
        // 首个回调先显示视频控件，但保持遮罩覆盖：此刻纹理里可能还是
        // 未初始化数据（渲染为绿色占位屏），等 frameUpdated（真实数据
        // 上传完成）后再撤掉遮罩。
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
    syncToolFormGeometry();
    m_toolForm->setVisible(show);
}

void VideoForm::syncToolFormGeometry()
{
    if (!m_toolForm || isFullScreen()) {
        return;
    }
    // 与视频窗口的顶部边缘齐平，并跨越其完整可见高度
    // （frameGeometry 包含标题栏，而 height() 不包含），这样
    // 工具栏按钮就能贴到上下两端而不会出现空白条带。
    m_toolForm->resize(m_toolForm->width(), frameGeometry().height());
    m_toolForm->move(pos().x() + geometry().width(), pos().y());
}

#ifdef Q_OS_WIN32
// 通过 Win32 分层窗口样式切换点击穿透。绝不能用 Qt 窗口标志实现
//（改标志会销毁重建原生窗口，重新引发黑闪）；SetWindowLongPtr 原地
// 修改扩展样式，无任何窗口重建。
static void setToolbarInputTransparent(QWidget *widget, bool transparent)
{
    if (!widget || !widget->testAttribute(Qt::WA_WState_Created)) {
        return;
    }
    HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR next = ex;
    if (transparent) {
        next |= (WS_EX_TRANSPARENT | WS_EX_LAYERED);
    } else {
        next &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    }
    if (next != ex) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, next);
    }
}
#else
static void setToolbarInputTransparent(QWidget *, bool)
{
}
#endif

void VideoForm::prepareFullscreenToolbar()
{
    if (!m_toolForm) {
        return;
    }
    m_fsToolbarResident = false;
#ifdef Q_OS_WIN32
    // 预驻留方案：进入全屏的瞬间就把工具栏窗口以全透明显示在屏幕右缘。
    // “新窗口覆盖全屏 OpenGL 画面”引发的 DWM 合成切换一次性发生在这里，
    // 与全屏切换本身重叠，肉眼不可感知；此后唤出/收起只改透明度，
    // 不再有新窗口出现，黑闪失去发生机制。
    // 透明驻留期间开启点击穿透，不吞画面右侧的触摸/点击。
    // 左缘刻意留在屏幕内 1px，避免 MagneticWidget 判定为“吸附在窗口
    // 外侧”而把它挪到屏幕外（move 同时会解除普通模式遗留的吸附状态）。
    const QRect scr = screen()->geometry();
    m_toolForm->resize(m_toolForm->width(), scr.height());
    m_toolForm->move(scr.right() - m_toolForm->width() + 1, scr.top());
    m_toolForm->setWindowOpacity(0.0);
    m_toolForm->show();
    m_toolForm->raise();
    setToolbarInputTransparent(m_toolForm, true);
    m_fsToolbarResident = true;
#endif
}

void VideoForm::updateFullscreenToolbar()
{
    if (!isFullScreen() || !m_toolForm) {
        m_fullscreenToolbarTimer.stop();
        return;
    }

    // 全屏窗口铺满屏幕，全局光标落在右缘 2px 内才触发
    const QPoint cur = QCursor::pos();
    const QRect scr = screen()->geometry();
    if (!scr.contains(cur)) {
        return; // 光标已移到其它显示器，保持当前状态
    }

    if (!m_fullscreenToolbarShown) {
        // 触发条件必须是屏幕最右侧边界：光标 x 落在右缘 2px 内才唤出
        if (cur.x() >= scr.right() - 1) {
            showFullscreenToolbar();
        }
        return;
    }

    // 已显示：光标移出工具栏范围（含少量缓冲）即收起。
    // 工具栏是独立悬浮窗口，geometry() 是全局坐标，与 cur 一致
    const QRect keepArea = m_toolForm->geometry().adjusted(-16, -16, 16, 16);
    if (!keepArea.contains(cur)) {
        hideFullscreenToolbar();
    }
}

void VideoForm::showFullscreenToolbar()
{
    if (!m_toolForm || m_fullscreenToolbarShown) {
        return;
    }
    const QRect scr = screen()->geometry();
    // 同步几何（屏幕分辨率/方向可能在中途变化）
    m_toolForm->resize(m_toolForm->width(), scr.height());
    m_toolForm->move(scr.right() - m_toolForm->width() + 1, scr.top());
    if (!m_fsToolbarResident) {
        // 非驻留平台：透明出现后淡入
        m_toolForm->setWindowOpacity(0.0);
        m_toolForm->show();
        m_toolForm->raise();
    } else {
        m_toolForm->raise();
    }
    setToolbarInputTransparent(m_toolForm, false);
    m_fullscreenToolbarShown = true;
    startFullscreenToolbarFade(m_toolForm->windowOpacity(), 1.0);
}

void VideoForm::hideFullscreenToolbar()
{
    if (!m_toolForm || !m_fullscreenToolbarShown) {
        return;
    }
    m_fullscreenToolbarShown = false;
    startFullscreenToolbarFade(m_toolForm->windowOpacity(), 0.0);
}

void VideoForm::exitFullscreenToolbar()
{
    if (m_toolbarFade) {
        m_toolbarFade->stop();
    }
    if (m_toolForm) {
        setToolbarInputTransparent(m_toolForm, false);
        m_toolForm->setWindowOpacity(1.0);
        m_toolForm->hide();
    }
    m_fsToolbarResident = false;
    m_fullscreenToolbarShown = false;
}

void VideoForm::startFullscreenToolbarFade(double from, double to)
{
    if (!m_toolForm) {
        return;
    }
    if (!m_toolbarFade) {
        m_toolbarFade = new QVariantAnimation(m_toolForm);
        m_toolbarFade->setDuration(120);
        connect(m_toolbarFade, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            if (m_toolForm && isFullScreen()) {
                m_toolForm->setWindowOpacity(v.toDouble());
            }
        });
        connect(m_toolbarFade, &QVariantAnimation::finished, this, [this]() {
            if (!m_toolForm || !isFullScreen()) {
                return;
            }
            if (m_fullscreenToolbarShown) {
                // 渐显完成：工具栏可交互
                setToolbarInputTransparent(m_toolForm, false);
                m_toolForm->setWindowOpacity(1.0);
            } else if (m_fsToolbarResident) {
                // 渐隐完成：回到全透明点击穿透驻留（窗口不隐藏、不重建）
                m_toolForm->setWindowOpacity(0.0);
                setToolbarInputTransparent(m_toolForm, true);
            } else {
                m_toolForm->setWindowOpacity(0.0);
                m_toolForm->hide();
            }
        });
    }
    m_toolbarFade->stop();
    m_toolbarFade->setStartValue(from);
    m_toolbarFade->setEndValue(to);
    m_toolbarFade->start();
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

    // 录制屏幕
    shortcut = new QShortcut(QKeySequence("Ctrl+Alt+r"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        if (isRecording()) {
            stopRecord();
        } else {
            startRecord();
        }
    });

    // 切换全屏
    shortcut = new QShortcut(QKeySequence("Ctrl+f"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        switchFullScreen();
    });

    // 缩放到正方形
    shortcut = new QShortcut(QKeySequence("Ctrl+g"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { resizeSquare(); });

    // 去除黑边
    shortcut = new QShortcut(QKeySequence("Ctrl+w"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { removeBlackRect(); });

    // 返回桌面（Home）
    shortcut = new QShortcut(QKeySequence("Ctrl+h"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoHome();
    });

    // 返回键
    shortcut = new QShortcut(QKeySequence("Ctrl+b"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoBack();
    });

    // 应用切换
    shortcut = new QShortcut(QKeySequence("Ctrl+s"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postAppSwitch();
    });

    // 菜单键
    shortcut = new QShortcut(QKeySequence("Ctrl+m"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoMenu();
    });

    // 音量+
    shortcut = new QShortcut(QKeySequence("Ctrl+up"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeUp();
    });

    // 音量-
    shortcut = new QShortcut(QKeySequence("Ctrl+down"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeDown();
    });

    // 电源键
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

    // 展开通知面板
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
            device->startActivity(QStringLiteral("android.settings.SETTINGS"));
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

    // 收起面板
    shortcut = new QShortcut(QKeySequence("Ctrl+Shift+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->collapsePanel();
    });

    // 复制
    shortcut = new QShortcut(QKeySequence("Ctrl+c"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCopy();
    });

    // 剪切
    shortcut = new QShortcut(QKeySequence("Ctrl+x"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCut();
    });

    // 剪贴板粘贴
    shortcut = new QShortcut(QKeySequence("Ctrl+v"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->setDeviceClipboard();
    });

    // 设置设备剪贴板
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

// 皮肤（手机边框）功能已废弃：原 updateStyleSheet/getMargins 已删除

QSize VideoForm::fittedWindowSize(bool fillScreen)
{
    if (m_frameSize.isEmpty() || m_widthHeightRatio <= 0.0f) {
        return QSize();
    }
    const QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        return QSize();
    }

    // 无皮肤边框：可用区域即整个窗口
    const int availW = screenRect.width();
    const int availH = screenRect.height();
    if (availW <= 0 || availH <= 0) {
        return QSize();
    }

    QSize videoSize;
    if (fillScreen) {
        // “最大化”应当放大画面而非黑边：取与设备宽高比一致、
        // 且仍能在屏幕可用区域内放下的
        // 最大矩形。
        const int maxW = qMax(1, availW - 20);
        const int maxH = qMax(1, availH - 20);
        int w = maxW;
        int h = static_cast<int>(w / m_widthHeightRatio + 0.5);
        if (h > maxH) {
            h = maxH;
            w = static_cast<int>(h * m_widthHeightRatio + 0.5);
        }
        videoSize = QSize(w, h);
    } else {
        const bool vertical = m_widthHeightRatio < 1.0f;
        if (vertical) {
            const int h = qMin(m_frameSize.height(), screenRect.height() - 200);
            videoSize = QSize(static_cast<int>(h * m_widthHeightRatio + 0.5), h);
        } else {
            const int w = qMin(m_frameSize.width(), screenRect.width() / 2);
            videoSize = QSize(w, static_cast<int>(w / m_widthHeightRatio + 0.5));
        }
    }

    // 无皮肤边框：窗口尺寸即画面尺寸
    QSize windowSize(videoSize.width(), videoSize.height());
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
    // 手机外框占据了边距，因此一个正常的窗口应几乎
    // 100% 都是画面；低于 70% 就会出现可见的空白条带。
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

    // resizeEvent 曾会把最小尺寸固定为最大化时
    // 的视频尺寸；此处清空它，让窗口能缩回到适配尺寸。
    setMinimumSize(0, 0);
    if (!wasMaximized && fitted == size()) {
        return;
    }
    resize(fitted);
    moveCenter();
}

void VideoForm::updateShowSize(const QSize &newSize)
{
    // 帧回调也会调用本函数：相同尺寸调用必须保持
    // 为无操作，否则窗口会与用户的手动缩放相互冲突。
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
    // 视频通道建立：加载动画推进到阶段2（通道建立）
    if (auto *overlay = qobject_cast<LoadingOverlay *>(m_loadingWidget.data())) {
        overlay->advancePhase(1);
    }
    if (m_flexDisplay) {
        m_frameSize = size;
        m_preventAutoResize = clientResized;
        ui->keepRatioWidget->setWidthHeightRatio(-1.0f);
        return;
    }
    // clientResized 仅在 flex 显示模式下有意义。普通显示
    // 的旋转必须保留长期以来的自动缩放行为。
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
        // 恢复为正常尺寸。
        resize(m_normalSize);
        // 全屏窗口会移动到 (0,0)。是 Qt 的 bug 吗？
        move(m_fullScreenBeforePos);

#ifdef Q_OS_MACOS
        //setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        //show();
#endif
        // 退出全屏：停止边缘唤出轮询，恢复普通模式的工具栏行为
        m_fullscreenToolbarTimer.stop();
        exitFullscreenToolbar();
        showToolForm(this->show_toolbar);
#ifdef Q_OS_WIN32
        ::SetThreadExecutionState(ES_CONTINUOUS);
#endif
    } else {
        // 横屏全屏铺满全屏，不保持宽高比
        if (m_widthHeightRatio > 1.0f) {
            ui->keepRatioWidget->setWidthHeightRatio(-1.0f);
        }

        // 进入全屏前记录当前尺寸，退出全屏后用于还原尺寸。
        m_normalSize = size();

        m_fullScreenBeforePos = pos();
        // 这种临时增加标题栏再全屏的方案会导致收不到mousemove事件，导致setmousetrack失效
        // mac fullscreen must show title bar
#ifdef Q_OS_MACOS
        //setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
#endif
        showToolForm(false);
        showFullScreen();

        // 预驻留工具栏窗口（全透明），并启用边缘唤出轮询
        prepareFullscreenToolbar();
        m_fullscreenToolbarTimer.start();

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

void VideoForm::setHost(bool host)
{
    if (m_toolForm) {
        m_toolForm->setHost(host);
    }
}

void VideoForm::updateFPS(quint32 fps, quint32 skipped)
{
    if (!m_fpsLabel || !m_fpsLabel->isVisible()) {
        // 标签隐藏（未开"显示 FPS"）时不做任何更新
        return;
    }
    // skipped = 每秒被新帧覆盖而未渲染的帧数，用于判断瓶颈在解码/渲染侧
    m_fpsLabel->setText(QString("FPS:%1 SKIP:%2").arg(fps).arg(skipped));
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
    // Metal 路径提供的是 CVPixelBufferRef 而非 YUV 平面数据。
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
    if (withAudio && !AudioOutput::instance().isRunning(m_serial)) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("record screen"),
            tr("Recording sound starts the sndcpy audio bridge, the device screen may switch to sndcpy for a moment.\n"
               "Record with sound?\n\nChoose No to record video only."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            withAudio = AudioOutput::instance().start(m_serial);
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
        // errorOccurred() 已通过 onRecordingError() 上报了原因
        return false;
    }

    m_recorder = recorder;
    if (withAudio) {
        m_recordAudioKey = QStringLiteral("record:") + m_serial;
        AudioOutput::instance().addPcmListener(m_serial, m_recordAudioKey, [recorder](const char *data, int size) {
            recorder->pushAudioData(data, size);
        });
    }

    // 鼠标轨迹叠加进视频 + 操作轨迹落盘（events.json 与视频时间轴对齐）
    if (config.recordMouseTrack) {
        m_trackRecorder->startSession(filePath + QStringLiteral(".events.json"));
        QSharedPointer<MouseTrackRecorder> track = m_trackRecorder;
        recorder->setOverlayPainter([track](QImage &img, qint64 ptsMs, double scale) {
            track->paintAt(img, ptsMs, scale);
        });
    }

    qInfo() << "recording started:" << filePath << "with audio:" << withAudio;
    m_recordingClockEmitted = false;
    emit recordingStateChanged();
    return true;
}

void VideoForm::stopRecord()
{
    if (!m_recorder) {
        return;
    }
    if (!m_recordAudioKey.isEmpty()) {
        // 在停止前先移除接收端，避免套接字线程向
        // 正在拆除的录制器推送数据。
        AudioOutput::instance().removePcmListener(m_serial, m_recordAudioKey);
        m_recordAudioKey.clear();
    }
    // stop() is idempotent and also joins a worker that already exited after
    // an encoding error.
    m_recorder->setOverlayPainter(nullptr);
    m_trackRecorder->endSession();
    m_recorder->stop();
    emit recordingStateChanged();
}

void VideoForm::onRecordingFinished(const QString &filePath, qint64 durationMs, qint64 bytes, bool hasAudio)
{
    qInfo() << "recording saved:" << filePath << "duration(ms):" << durationMs << "bytes:" << bytes
            << "audio:" << hasAudio;
    m_trackRecorder->endSession();

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
    m_trackRecorder->endSession();

    if (auto *recorder = qobject_cast<VideoRecorder *>(sender())) {
        recorder->deleteLater();
    }
    if (!m_recordAudioKey.isEmpty()) {
        AudioOutput::instance().removePcmListener(m_serial, m_recordAudioKey);
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
    // 首个视频帧回调到达：加载动画推进到阶段3（首帧解码）。
    // advancePhase 只前进不回退，后续帧重复调用无副作用。
    if (auto *overlay = qobject_cast<LoadingOverlay *>(m_loadingWidget.data())) {
        overlay->advancePhase(2);
    }
    // P1 打点优化：先推帧打点再渲染。pts = 帧到达时刻，渲染（OpenGL/软件
    // 上传，可达数十毫秒）不计入视频时间轴，消除 UI 繁忙时逐帧滞后累积。
    if (m_recorder && m_recorder->isRecording()) {
        m_recorder->pushVideoFrame(dataY, dataU, dataV, width, height, linesizeY, linesizeU, linesizeV);
        // 首个进入录制器的帧即视频 pts 0 点：广播给工具栏，把录制计时对齐到
        // 视频实际内容起点（点击→首帧的空窗不计入视频）
        if (!m_recordingClockEmitted) {
            m_recordingClockEmitted = true;
            emit recordingClockStarted();
        }
    }
    updateRender(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
}

void VideoForm::onFrameMetal(void *cvPixelBuffer, int width, int height)
{
#ifdef Q_OS_MACOS
    if (!m_metalWidget || !cvPixelBuffer) {
        return;
    }

    if (m_metalFirstFrame) {
        m_metalFirstFrame = false;
        // Metal 模式的退场时机（OpenGL 模式走 frameUpdated 信号退场）
        if (auto *overlay = qobject_cast<LoadingOverlay *>(m_loadingWidget.data())) {
            overlay->fadeOutAndDelete();
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
    QWidget *pressVw = videoWidget();
    if (event->button() == Qt::MiddleButton) {
        if (pressVw && pressVw->geometry().contains(event->pos())) {
            trackMouse(pressVw->mapFrom(this, event->pos()), pressVw->size(), QEvent::MouseButtonPress,
                       Qt::MiddleButton);
        }
        if (device && !device->isCurrentCustomKeymap()) {
            device->postGoHome();
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        if (pressVw && pressVw->geometry().contains(event->pos())) {
            trackMouse(pressVw->mapFrom(this, event->pos()), pressVw->size(), QEvent::MouseButtonPress,
                       Qt::RightButton);
        }
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
        trackMouse(mappedPos, vw->size(), newEvent.type(), newEvent.button());
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

        // 本地范围校验
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
        trackMouse(local, vw->size(), newEvent.type(), newEvent.button());
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
        trackMouse(mappedPos, vw->size(), newEvent.type(), newEvent.button());
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
        trackMouse(mappedPos, vw->size(), newEvent.type(), newEvent.button());
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
        trackWheel(pos, vw->size(), event->angleDelta().y());
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
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this); // 运行时切换语言
        return;
    }
    if (m_stateAdjusting || event->type() != QEvent::WindowStateChange) {
        return;
    }
    if (isFullScreen() || isMinimized()) {
        return;  // 全屏/最小化由各自的路径处理。
    }
    const auto *stateEvent = static_cast<QWindowStateChangeEvent *>(event);
    const bool wasMax = stateEvent->oldState() & Qt::WindowMaximized;
    const bool isMax = windowState() & Qt::WindowMaximized;

    if (isMax && !m_smartMaximized) {
        // 首次点击“最大化”：记下待恢复的尺寸，再把画面放大到
        // 屏幕能容纳的最大尺寸，而不是留下大片黑边。窗口仍保持
        // 普通状态，因此系统的“最大化/还原”按钮仍能作为真正的
        // 开关使用。
        m_restoredGeometry = normalGeometry();
        m_smartMaximized = true;
        m_stateAdjusting = true;
        showNormal();
        fitWindowToFrame(true);
        m_stateAdjusting = false;
    } else if (isMax && m_smartMaximized) {
        // 已放大状态下再次点击“最大化”：恢复点击前的
        // 尺寸，使按钮表现像真正的“最大化/还原”开关。
        m_smartMaximized = false;
        m_stateAdjusting = true;
        showNormal();
        if (m_restoredGeometry.isValid()) {
            setGeometry(m_restoredGeometry);
        } else {
            fitWindowToFrame(false);
        }
        m_stateAdjusting = false;
    } else if (wasMax) {
        // 通过“还原”按钮离开真正的“最大化”状态。
        m_smartMaximized = false;
        m_stateAdjusting = true;
        if (m_restoredGeometry.isValid()) {
            setGeometry(m_restoredGeometry);
        }
        m_stateAdjusting = false;
    } else if (hasExcessiveLetterbox()) {
        fitWindowToFrame(false);
    }

    // 上面应用的几何变化（showNormal/setGeometry）可能会向 resizeEvent
    // 上报一个过期的 frameGeometry，使工具栏停留在上一次的
    // 高度。待事件循环稳定新窗口几何后再重新同步一次。
    QTimer::singleShot(0, this, [this]() { syncToolFormGeometry(); });
}

void VideoForm::showEvent(QShowEvent *event)
{
    Q_UNUSED(event)
    // 首帧到达前显示连接加载遮罩
    if (m_loadingWidget) {
        syncLoadingOverlay();
        m_loadingWidget->show();
        // 15 秒仍未出画时更新文案，把"无画面"的常见原因说清楚：
        // 设备屏幕熄灭、USB 调试授权未确认都会导致服务端连上但不出帧
        QTimer::singleShot(15000, m_loadingWidget, [this]() {
            if (auto *overlay = qobject_cast<LoadingOverlay *>(m_loadingWidget.data())) {
                if (overlay->isVisible()) {
                    overlay->setNoFrameText();
                }
            }
        });
    }
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
        // “智能最大化”会话遗留的几何尺寸可能几乎和屏幕一样大。Dialog 用
        // setGeometry 原样恢复它，于是窗口以放大状态启动，而开关标志却被
        // 重置：首次点击“最大化”就会像没反应。首次显示时把这类
        // 遗留尺寸缩回正常的适配尺寸。
        if (!isMaximized() && !isFullScreen() && !m_closing) {
            const QSize fitted = fittedWindowSize(false);
            const bool oversized = !fitted.isEmpty() &&
                (width() > fitted.width() * 1.25 || height() > fitted.height() * 1.25);
            if (hasExcessiveLetterbox() || oversized) {
                fitWindowToFrame(false);
            }
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
    // 遮罩跟随窗口大小变化，始终覆盖画面区域
    syncLoadingOverlay();
    // APK 安装提示浮层保持居中
    syncInstallTip();
    if (m_toolForm && !isFullScreen()) {
        m_toolForm->resize(m_toolForm->width(), frameGeometry().height());
    }
    if (m_flexDisplay) {
        m_pendingDisplaySize = ui->keepRatioWidget->size();
        if (!m_pendingDisplaySize.isEmpty()) {
            m_flexResizeTimer.start();
        }
        return;
    }

    // KeepRatioWidget 会把画面缩放到任意可用空间，因此
    // 不需要最小窗口尺寸。旧逻辑把最小尺寸固定为*上一次*（最大化时）的
    // 视频尺寸，导致窗口无法缩回——这正是
    // 最大化/最小化后大片黑边的根源。
}

void VideoForm::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event)
    // 先冲刷正在进行的录制：完成对话框会在窗口消失前
    // 告诉用户文件保存到了哪里。
    if (m_recorder) {
        stopRecord();
    }
    m_closing = true;
    // 用户主动关窗口：在断开链路（→ deviceDisconnected）发出前先标记，
    // 主窗口据此跳过自动重连
    emit windowClosedByUser(m_serial);

    // sndcpy 桥接与本设备绑定：关闭其窗口也必须
    // 停止音频播放（并拆除 forward/捕获活动）。
    AudioOutput::instance().stopForDevice(m_serial);

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    // 绝不持久化放大后的几何尺寸。“智能最大化”让窗口保持
    // 普通状态，因此单靠 isMaximized() 检查会保存放大后的
    // 尺寸，下次连接时窗口就会以放大状态启动，而开关标志却已
    // 重置——导致“最大化/还原”按钮看起来失效。
    QRect savedRect = isMaximized() ? normalGeometry() : geometry();
    if (m_smartMaximized && m_restoredGeometry.isValid()) {
        savedRect = m_restoredGeometry;
    }
    Config::getInstance().setRect(device->getSerial(), savedRect);
    device->disconnectDevice();
}

void VideoForm::installApk(const QString &apkPath)
{
    const QFileInfo info(apkPath);
    if (!info.exists()) {
        return;
    }
    showInstallTip(tr("正在安装 %1 …").arg(info.fileName()), 0);
    const QString name = info.fileName();
    installOne(apkPath, [this, name](bool ok, const QString &reason) {
        if (ok) {
            showInstallTip(tr("安装成功 %1").arg(name), 4000);
        } else {
            showInstallTip(tr("安装失败 %1：%2").arg(name, reason), 10000);
        }
    });
}

// 单个 apk 安装：独立 adb 进程 + 超时兜底，-r 覆盖安装保留数据。
// done 回调返回 (是否成功, 失败原因)；调用方负责提示与队列推进。
void VideoForm::installOne(const QString &apkPath, const std::function<void(bool, const QString &)> &done)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        done(false, tr("设备未连接"));
        return;
    }
    const QString path = ApkPathUtils::toAdbPath(QFileInfo(apkPath).absoluteFilePath());
    auto *adb = new qsc::AdbProcess(this);
    QPointer<VideoForm> guard = this;
    adb->executeWithTimeout(m_serial, QStringList() << "install" << "-r" << path, 180000,
                            [guard, adb, done](bool ok, bool timedOut, const QString &out) {
        QString reason;
        bool success = false;
        const QString trimmed = out.trimmed();
        if (!ok) {
            reason = timedOut ? tr("超时（3 分钟），请确认设备连接") : trimmed.section('\n', -1);
        } else if (!trimmed.contains("Success", Qt::CaseInsensitive)) {
            // adb install 失败时退出码仍可能为 0，必须以输出为准
            reason = trimmed.section('\n', -1);
        } else {
            success = true;
        }
        if (guard) {
            done(success, reason);
        }
        adb->deleteLater();
    });
}

void VideoForm::installApkBatch(const QStringList &paths)
{
    if (m_installBusy || paths.isEmpty()) {
        return;
    }
    m_installQueue = paths;
    m_installOk = 0;
    m_installFail = 0;
    m_installCancelled = false;
    m_installBusy = true;
    installNextApk();
}

void VideoForm::installNextApk()
{
    if (m_installCancelled || m_installQueue.isEmpty()) {
        m_installBusy = false;
        if (!m_installCancelled) {
            showInstallTip(tr("批量安装完成：成功 %1，失败 %2").arg(m_installOk).arg(m_installFail), 6000);
        }
        emit apkInstallFinished(m_installOk, m_installFail);
        return;
    }
    const QString path = m_installQueue.takeFirst();
    emit apkInstallProgress(path, tr("安装中"));
    installOne(path, [this, path](bool ok, const QString &reason) {
        if (ok) {
            ++m_installOk;
            emit apkInstallProgress(path, tr("成功"));
        } else {
            ++m_installFail;
            emit apkInstallProgress(path, tr("失败：") + reason);
        }
        installNextApk();
    });
}

void VideoForm::cancelInstallBatch()
{
    m_installCancelled = true;
    m_installQueue.clear();
}

void VideoForm::showInstallTip(const QString &text, int autoHideMs)
{
    if (!m_installTipLabel) {
        auto *label = new QLabel(this);
        label->setObjectName("installTipLabel");
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(
            "QLabel#installTipLabel{background:rgba(0,0,0,170);color:#ffffff;padding:10px 16px;"
            "border-radius:8px;font-size:14px;}");
        m_installTipLabel = label;
        m_installTipTimer.setSingleShot(true);
        connect(&m_installTipTimer, &QTimer::timeout, label, &QLabel::hide);
    }
    m_installTipLabel->setText(text);
    syncInstallTip();
    m_installTipLabel->show();
    m_installTipLabel->raise();
    if (autoHideMs > 0) {
        m_installTipTimer.start(autoHideMs);
    } else {
        m_installTipTimer.stop();
    }
}

void VideoForm::syncInstallTip()
{
    if (!m_installTipLabel) {
        return;
    }
    const QSize hint = m_installTipLabel->sizeHint();
    m_installTipLabel->resize(hint);
    m_installTipLabel->move((width() - hint.width()) / 2, (height() - hint.height()) / 2);
}

// 录制时的鼠标轨迹记录：事件坐标已 mapFrom 到视频控件，
// MouseTrackRecorder 内部再换算到设备画面坐标（与触控转发同一公式）
void VideoForm::trackMouse(const QPointF &posInVideo, const QSize &videoSize, QEvent::Type type, Qt::MouseButton button)
{
    if (m_trackRecorder && m_trackRecorder->isActive() && !videoSize.isEmpty()) {
        m_trackRecorder->onMouseEvent(posInVideo, videoSize, m_frameSize, type, button);
    }
}

void VideoForm::trackWheel(const QPointF &posInVideo, const QSize &videoSize, int angleDeltaY)
{
    if (m_trackRecorder && m_trackRecorder->isActive() && !videoSize.isEmpty()) {
        m_trackRecorder->onWheelEvent(posInVideo, videoSize, m_frameSize, angleDeltaY);
    }
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

        if (fileInfo.isFile() && fileInfo.suffix().compare(QLatin1String("apk"), Qt::CaseInsensitive) == 0) {
            installApk(file);
            continue;
        }
        emit device->pushFileRequest(file, Config::getInstance().getPushFilePath() + fileInfo.fileName());
    }
}
