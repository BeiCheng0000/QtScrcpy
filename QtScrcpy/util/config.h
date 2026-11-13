#ifndef CONFIG_H
#define CONFIG_H

#include <QObject>
#include <QMap>
#include <QPointer>
#include <QRect>
#include <QVariant>

struct UserBootConfig
{
    QString recordPath = "";
    bool recordAudio = true;
    int recordVideoBitrate = 8000000;
    int recordFps = 30;
    int recordAudioBitrate = 128000;
    bool autoStartAudio    = true; // 设备连接后立刻启动 sndcpy 音频桥接
    // 12 Mbps：原画（1200×2000 级别）分辨率下 2 Mbps 会明显模糊；
    // USB/Wi-Fi 带宽都足以承载，且不增加编码/传输延迟。
    quint32 bitRate = 12000000;
    int maxSizeIndex = 0;
    int recordFormatIndex = 0;
    int lockOrientationIndex = 0;
    bool recordScreen     = false;
    bool recordBackground = false;
    bool reverseConnect   = true;
    bool showFPS          = false;
    bool windowOnTop      = false;
    bool autoOffScreen    = false;
    bool framelessWindow  = false;
    bool keepAlive        = false;
    bool simpleMode       = false;
    bool autoUpdateDevice = true;
    bool showToolbar      = true;
    bool quitOnClose      = false; // 主窗口关闭时直接退出，而非隐藏到系统托盘
    int decodeMode        = 0;  // 0=FFmpeg OpenGL (默认), 1=VideoToolbox Metal (Apple Silicon)
    int codecModeIndex    = 0;
    int presetLevel       = 1;  // 当前选中编码预设的优化档位
    int videoSource       = 0;  // 0=display, 1=camera
    int cameraFacing      = 0;  // 0=back, 1=front
    bool advancedDisplay = false;
    int displayMode = 0;
    QString displayId;
    QString newDisplay;
    QString crop;
    bool flexDisplay = false;
    QString displayImePolicy;
    bool vdSystemDecorations = true;
    bool vdDestroyContent = true;
    bool keepActive = false;
    QString startApp;
};

class QSettings;
class Config : public QObject
{
    Q_OBJECT
public:

    static Config &getInstance();

    // 配置
    QString getLanguage();
    QString getTitle();
    int getMaxFps();
    int getDesktopOpenGL();
    int getSkin();
    int getRenderExpiredFrames();
    QString getPushFilePath();
    QString getServerPath();
    QString getAdbPath();
    QString getLogLevel();
    QString getCodecOptions();
    QString getCodecName();
    QString getConfigDirectory();
    bool updateCommonConfig(const QMap<QString, QVariant> &values);
    QStringList getConnectedGroups();

    // 用户数据：通用
    void setUserBootConfig(const UserBootConfig &config);
    UserBootConfig getUserBootConfig();
    void setTrayMessageShown(bool shown);
    bool getTrayMessageShown();

    // 用户数据：设备
    void setNickName(const QString &serial, const QString &name);
    QString getNickName(const QString &serial);
    void setRect(const QString &serial, const QRect &rc);
    QRect getRect(const QString &serial);

    void deleteGroup(const QString &serial);

    // IP history methods
    void saveIpHistory(const QString &ip);
    QStringList getIpHistory(); 
    void clearIpHistory();

    // 端口历史记录方法
    void savePortHistory(const QString &port);
    QStringList getPortHistory(); 
    void clearPortHistory();

private:
    explicit Config(QObject *parent = nullptr);
    const QString &getConfigPath();
    QString getDefaultConfigPath() const;
    void initializeConfig();
    // 旧版本 2 Mbps 默认码率的一次性迁移
    void migrateLegacyVideoQuality();

private:
    static QString s_configPath;
    QPointer<QSettings> m_settings;
    QPointer<QSettings> m_userData;
};

#endif // CONFIG_H
