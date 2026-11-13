#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPalette>
#ifdef Q_OS_LINUX
#include <QIcon>
#endif
#include <QSurfaceFormat>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTranslator>
#include <QLibraryInfo>
#include <QDateTime>

#include "config.h"
#include "dialog.h"
#include "mousetap/mousetap.h"
#include "singleinstance.h"

static Dialog *g_mainDlg = Q_NULLPTR;
static QtMessageHandler g_oldMessageHandler = Q_NULLPTR;
void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg);
void installTranslator();

static QtMsgType g_msgType = QtInfoMsg;
QtMsgType covertLogLevel(const QString &logLevel);

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName("QtScrcpy");
    QCoreApplication::setApplicationName("QtScrcpy");

    const QString executableDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
#ifdef Q_OS_MACOS
    const QString repositoryRoot = QDir(executableDir).absoluteFilePath("../../../../../../");
#else
    const QString repositoryRoot = QDir(executableDir).absoluteFilePath("../../..");
#endif
    const QDir repositoryDir(repositoryRoot);
    const QString defaultKeymapPath = repositoryDir.exists("keymap")
        ? repositoryDir.filePath("keymap")
        : QDir(executableDir).filePath("keymap");
    const QString defaultConfigPath = QFileInfo(repositoryDir.filePath("config/config.ini")).isFile()
        ? repositoryDir.filePath("config")
        : QDir(executableDir).filePath("config");

    // 将打包的默认配置与可变的用户数据分开。开发构建
    // 使用仓库中的模板，而发布包使用位于可执行文件
    // 旁边的文件。AppImage 的 AppRun 可能会覆盖这些路径。
    if (qgetenv("QTSCRCPY_DEFAULT_KEYMAP_PATH").isEmpty()) {
        qputenv("QTSCRCPY_DEFAULT_KEYMAP_PATH", defaultKeymapPath.toLocal8Bit());
    }
    if (qgetenv("QTSCRCPY_DEFAULT_CONFIG_PATH").isEmpty()) {
        qputenv("QTSCRCPY_DEFAULT_CONFIG_PATH", defaultConfigPath.toLocal8Bit());
    }

    // set env
    // 只在未设置时填充默认值，避免覆盖用户环境变量/高级设置中显式配置的路径。
#ifdef Q_OS_WIN32
    if (qgetenv("QTSCRCPY_ADB_PATH").isEmpty()) {
        qputenv("QTSCRCPY_ADB_PATH", "../../../QtScrcpy/QtScrcpyCore/src/third_party/adb/win/adb.exe");
    }
    if (qgetenv("QTSCRCPY_SERVER_PATH").isEmpty()) {
        qputenv("QTSCRCPY_SERVER_PATH", "../../../QtScrcpy/QtScrcpyCore/src/third_party/scrcpy-server");
    }
#endif

#ifdef Q_OS_MACOS
    if (qgetenv("QTSCRCPY_ADB_PATH").isEmpty()) {
        qputenv("QTSCRCPY_ADB_PATH", "../../../../../../QtScrcpy/QtScrcpyCore/src/third_party/adb/mac/adb");
    }
    if (qgetenv("QTSCRCPY_SERVER_PATH").isEmpty()) {
        qputenv("QTSCRCPY_SERVER_PATH", "../../../../../../QtScrcpy/QtScrcpyCore/src/third_party/scrcpy-server");
    }
#endif

#ifdef Q_OS_LINUX
    // Only set environment variables if they are not already set (e.g., by AppImage AppRun)
    if (qgetenv("QTSCRCPY_ADB_PATH").isEmpty()) {
        qputenv("QTSCRCPY_ADB_PATH", "../../../QtScrcpy/QtScrcpyCore/src/third_party/adb/linux/adb");
    }
    if (qgetenv("QTSCRCPY_SERVER_PATH").isEmpty()) {
        qputenv("QTSCRCPY_SERVER_PATH", "../../../QtScrcpy/QtScrcpyCore/src/third_party/scrcpy-server");
    }
#endif

    g_msgType = covertLogLevel(Config::getInstance().getLogLevel());

    // set on QApplication before
    // bug: config path is error on mac
    int opengl = Config::getInstance().getDesktopOpenGL();
    if (0 == opengl) {
        QApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    } else if (1 == opengl) {
        QApplication::setAttribute(Qt::AA_UseOpenGLES);
    } else if (2 == opengl) {
        QApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    }

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

#if (QT_VERSION >= QT_VERSION_CHECK(5,14,0))
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif

    QSurfaceFormat varFormat = QSurfaceFormat::defaultFormat();
    varFormat.setVersion(2, 0);
    varFormat.setProfile(QSurfaceFormat::NoProfile);
    /*
    varFormat.setSamples(4);
    varFormat.setAlphaBufferSize(8);
    varFormat.setBlueBufferSize(8);
    varFormat.setRedBufferSize(8);
    varFormat.setGreenBufferSize(8);
    varFormat.setDepthBufferSize(24);
    */
    QSurfaceFormat::setDefaultFormat(varFormat);

    g_oldMessageHandler = qInstallMessageHandler(myMessageOutput);
    QApplication a(argc, argv);

    // Only one QtScrcpy per user. If one is already running (possibly hidden in
    // the system tray), ask it to show its window and exit.
    SingleInstance singleInstance(QStringLiteral("QtScrcpy"));
    if (!singleInstance.isPrimary()) {
        singleInstance.notifyPrimary();
        return 0;
    }

    // Set application icon for Linux (taskbar icon)
#ifdef Q_OS_LINUX
    // Load icon from Qt resource (logo.png is included in res.qrc)
    QIcon appIcon(":/image/tray/logo.png");
    if (!appIcon.isNull()) {
        a.setWindowIcon(appIcon);
    }
#endif

    // windows下通过qmake VERSION变量或者rc设置版本号和应用名称后，这里可以直接拿到
    // mac下拿到的是CFBundleVersion的值
    qDebug() << a.applicationVersion();
    qDebug() << a.applicationName();

    //update version
    QStringList versionList = QCoreApplication::applicationVersion().split(".");
    if (versionList.size() >= 3) {
        QString version = versionList[0] + "." + versionList[1] + "." + versionList[2];
        a.setApplicationVersion(version);
    }

    installTranslator();
#if defined(Q_OS_WIN32) || defined(Q_OS_MACOS)
    MouseTap::getInstance()->initMouseEventTap();
#endif

    // load style sheet
    QFile file(":/qss/psblack.css");
    if (file.open(QFile::ReadOnly)) {
        QString qss = QLatin1String(file.readAll());
        QPalette palette;
        palette.setColor(QPalette::Window, QColor("#1e1e1e"));
        palette.setColor(QPalette::WindowText, QColor("#ffffff"));
        palette.setColor(QPalette::Base, QColor("#252525"));
        palette.setColor(QPalette::AlternateBase, QColor("#2d2d2d"));
        palette.setColor(QPalette::Text, QColor("#ffffff"));
        palette.setColor(QPalette::Button, QColor("#252525"));
        palette.setColor(QPalette::ButtonText, QColor("#ffffff"));
        palette.setColor(QPalette::Highlight, QColor("#0078d4"));
        palette.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#6b6b6b"));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#6b6b6b"));
        palette.setColor(QPalette::Disabled, QPalette::Base, QColor("#2d2d2d"));
        qApp->setPalette(palette);
        qApp->setStyleSheet(qss);
        file.close();
    }

    qsc::AdbProcess::setAdbPath(Config::getInstance().getAdbPath());

    g_mainDlg = new Dialog {};
    g_mainDlg->show();
    QObject::connect(&singleInstance, &SingleInstance::activateRequested, g_mainDlg, &Dialog::bringToFront);

    int ret = a.exec();
    delete g_mainDlg;

#if defined(Q_OS_WIN32) || defined(Q_OS_MACOS)
    MouseTap::getInstance()->quitMouseEventTap();
#endif
    return ret;
}

// Qt 5.15 的简体中文翻译文件缺失 QWidgetTextControl 上下文：文本编辑框
// 右键菜单（复制/粘贴/全选等）运行时使用 QWidgetTextControl 上下文，而官方
// qt_zh_CN.qm 里这些文案在旧上下文 QTextControl 下，永远无法命中，导致
// 右键菜单始终显示英文。这里用自定义翻译器为该上下文补齐标准文案，
// 译法与官方简体中文翻译保持一致。
class TextControlTranslator : public QTranslator
{
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override
    {
        if (qstrcmp(context, "QWidgetTextControl") == 0) {
            const QLatin1String src(sourceText);
            if (src == QLatin1String("&Copy")) {
                return QString::fromUtf8("复制(&C)");
            }
            if (src == QLatin1String("Cu&t")) {
                return QString::fromUtf8("剪切(&T)");
            }
            if (src == QLatin1String("&Paste")) {
                return QString::fromUtf8("粘贴(&P)");
            }
            if (src == QLatin1String("&Undo")) {
                return QString::fromUtf8("撤消(&U)");
            }
            if (src == QLatin1String("&Redo")) {
                return QString::fromUtf8("恢复(&R)");
            }
            if (src == QLatin1String("Delete")) {
                return QString::fromUtf8("删除");
            }
            if (src == QLatin1String("Select All")) {
                return QString::fromUtf8("选择全部");
            }
            if (src == QLatin1String("Copy & Link Location")) {
                return QString::fromUtf8("复制链接位置(&L)");
            }
        }
        return QTranslator::translate(context, sourceText, disambiguation, n);
    }
};

void installTranslator()
{
    static QTranslator translator;
    static QTranslator qtTranslator;
    static TextControlTranslator textControlTranslator;
    // 默认简体中文：旧配置中可能保存了 "Auto"（跟随系统区域），系统为英文时
    // 界面会显示英文。这里把默认语言固定为中文，仅当用户在设置里显式选择
    // 其他语言时才切换。
    QLocale::Language language = QLocale::Chinese;

    if (Config::getInstance().getLanguage() == "en_US") {
        language = QLocale::English;
    } else if (Config::getInstance().getLanguage() == "ja_JP") {
        language = QLocale::Japanese;
    } else if (Config::getInstance().getLanguage() == "ko_KR") {
        language = QLocale::Korean;
    }

    QString languagePath = ":/i18n/";
    QString qtLang;
    switch (language) {
    case QLocale::Chinese:
        languagePath += "zh_CN.qm";
        qtLang = "qt_zh_CN";
        break;
    case QLocale::Japanese:
        languagePath += "ja_JP.qm";
        qtLang = "qt_ja";
        break;
    case QLocale::Korean:
        languagePath += "ko_KR.qm";
        qtLang = "qt_ko";
        break;
    case QLocale::English:
    default:
        languagePath += "en_US.qm";
        qtLang = "qt_en";
        break;
    }

    auto loaded = translator.load(languagePath);
    if (!loaded) {
        qWarning() << "Failed to load translation file:" << languagePath;
    }
    qApp->installTranslator(&translator);

    // 加载 Qt 自带的基础翻译（如输入框右键菜单的“复制/粘贴/撤销”等标准控件文字）
    // 优先从应用资源加载（已打包 qt_zh_CN.qm，不依赖 Qt 安装路径），失败时回退到
    // Qt 安装目录下的翻译文件夹。
    bool qtTrLoaded = false;
    if (language == QLocale::Chinese) {
        qtTrLoaded = qtTranslator.load(QStringLiteral(":/i18n/qt_zh_CN.qm"));
    }
    if (!qtTrLoaded) {
        const QString qtTrPath = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
        qtTrLoaded = qtTranslator.load(qtLang, qtTrPath);
    }
    if (!qtTrLoaded) {
        qWarning() << "Failed to load Qt base translation:" << qtLang;
    }
    qApp->installTranslator(&qtTranslator);

    // 中文界面下补齐 Qt 缺失的标准文本控件右键菜单文案
    if (language == QLocale::Chinese) {
        qApp->installTranslator(&textControlTranslator);
    }
}

QtMsgType covertLogLevel(const QString &logLevel)
{
    if ("debug" == logLevel) {
        return QtDebugMsg;
    }

    if ("info" == logLevel) {
        return QtInfoMsg;
    }

    if ("warn" == logLevel) {
        return QtWarningMsg;
    }

    if ("error" == logLevel) {
        return QtCriticalMsg;
    }

#ifdef QT_NO_DEBUG
    return QtInfoMsg;
#else
    return QtDebugMsg;
#endif
}

void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QString outputMsg;
    
#ifdef ENABLE_DETAILED_LOGS
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    
    if (context.file && context.line > 0) {
        QString fileName = QString::fromUtf8(context.file);

        int lastSlash = fileName.lastIndexOf('/');
        if (lastSlash >= 0) {
            fileName = fileName.mid(lastSlash + 1);
        }
        lastSlash = fileName.lastIndexOf('\\');
        if (lastSlash >= 0) {
            fileName = fileName.mid(lastSlash + 1);
        }
        
        outputMsg = QString("[ %1 %2: %3 ] %4").arg(timestamp).arg(fileName).arg(context.line).arg(msg);
    } else {
        outputMsg = QString("[%1] %2").arg(timestamp).arg(msg);
    }

    switch (type) {
    case QtDebugMsg:
        outputMsg.prepend("[debug] ");
        break;
    case QtInfoMsg:
        outputMsg.prepend("[info] ");
        break;
    case QtWarningMsg:
        outputMsg.prepend("[warring] ");
        break;
    case QtCriticalMsg:
        outputMsg.prepend("[critical] ");
        break;
    case QtFatalMsg:
        outputMsg.prepend("[fatal] ");
        break;
    }

    fprintf(stderr, "%s\n", outputMsg.toUtf8().constData());
#else
    outputMsg = msg;
    if (g_oldMessageHandler) {
        g_oldMessageHandler(type, context, outputMsg);
    }
#endif

    // Is Qt log level higher than warning?
    float fLogLevel = g_msgType;
    if (QtInfoMsg == g_msgType) {
        fLogLevel = QtDebugMsg + 0.5f;
    }
    float fLogLevel2 = type;
    if (QtInfoMsg == type) {
        fLogLevel2 = QtDebugMsg + 0.5f;
    }

    if (fLogLevel <= fLogLevel2) {
        if (g_mainDlg && g_mainDlg->isVisible() && !g_mainDlg->filterLog(outputMsg)) {
            g_mainDlg->outLog(outputMsg);
        }
    }

    if (QtFatalMsg == type) {
        //abort();
    }
}
