#ifndef KEYMAP_H
#define KEYMAP_H
#include <QJsonObject>
#include <QMetaEnum>
#include <QMultiHash>
#include <QObject>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QVector>

#include "keycodes.h"

#define MAX_DELAY_CLICK_NODES 50

class KeyMap : public QObject
{
    Q_OBJECT
public:
    enum KeyMapType
    {
        KMT_INVALID = -1,
        KMT_CLICK = 0,
        KMT_CLICK_TWICE,
        KMT_CLICK_MULTI,
        KMT_STEER_WHEEL,
        KMT_DRAG,
        KMT_MOUSE_MOVE,
        KMT_ANDROID_KEY
    };
    Q_ENUM(KeyMapType)

    enum ActionType
    {
        AT_INVALID = -1,
        AT_KEY = 0,
        AT_MOUSE = 1,
    };
    Q_ENUM(ActionType)

    struct DelayClickNode
    {
        int delay = 0;
        QPointF pos = QPointF(0, 0);
    };

    struct KeyNode
    {
        ActionType type = AT_INVALID;
        int key = Qt::Key_unknown;
        QPointF pos = QPointF(0, 0);                           // 普通按键
        QPointF extendPos = QPointF(0, 0);                     // 用于拖拽
        double extendOffset = 0.0;                             // 用于方向盘
        DelayClickNode delayClickNodes[MAX_DELAY_CLICK_NODES]; // 用于多点点击
        int delayClickNodesCount = 0;
        AndroidKeycode androidKey = AKEYCODE_UNKNOWN;          // 用于按键

        KeyNode(
            ActionType type = AT_INVALID,
            int key = Qt::Key_unknown,
            QPointF pos = QPointF(0, 0),
            QPointF extendPos = QPointF(0, 0),
            double extendOffset = 0.0,
            AndroidKeycode androidKey = AKEYCODE_UNKNOWN)
            : type(type), key(key), pos(pos), extendPos(extendPos), extendOffset(extendOffset), androidKey(androidKey)
        {
        }
    };

    struct KeyMapNode
    {
        KeyMapType type = KMT_INVALID;
        union DATA
        {
            struct
            {
                KeyNode keyNode;
                bool switchMap = false;
            } click;
            struct
            {
                KeyNode keyNode;
            } clickTwice;
            struct
            {
                KeyNode keyNode;
            } clickMulti;
            struct
            {
                QPointF centerPos = { 0.0, 0.0 };
                KeyNode left, right, up, down;
            } steerWheel;
            struct
            {
                KeyNode keyNode;
                quint32 startDelay = 0;      // 开始拖拽前的延迟
                float dragSpeed = 1.0;       // 拖拽速度（0-1，1 为最快）
            } drag;
            struct
            {
                QPointF startPos   = { 0.0, 0.0 };
                QPointF speedRatio = { 1.0, 1.0 };
                KeyNode smallEyes;
            } mouseMove;
            struct
            {
                KeyNode keyNode;
            } androidKey;
            DATA() {}
            ~DATA() {}
        } data;

        KeyMapNode() {}
        ~KeyMapNode() {}
    };

    KeyMap(QObject *parent = Q_NULLPTR);
    virtual ~KeyMap();

    void loadKeyMap(const QString &json);
    const KeyMap::KeyMapNode &getKeyMapNode(int key);
    const KeyMap::KeyMapNode &getKeyMapNodeKey(int key);
    const KeyMap::KeyMapNode &getKeyMapNodeMouse(int key);
    bool isSwitchOnKeyboard();
    int getSwitchKey();

    bool isValidMouseMoveMap();
    bool isValidSteerWheelMap();
    const KeyMap::KeyMapNode &getMouseMoveMap();

private:
    // 建立从按键/事件到 keyMapNode 的反向映射
    void makeReverseMap();

    // 基础项的安全检查
    bool checkItemString(const QJsonObject &node, const QString &name);
    bool checkItemDouble(const QJsonObject &node, const QString &name);
    bool checkItemBool(const QJsonObject &node, const QString &name);
    bool checkItemObject(const QJsonObject &node, const QString &name);
    bool checkItemPos(const QJsonObject &node, const QString &name);

    // KeyMapNode 的安全检查
    bool checkForClick(const QJsonObject &node);
    bool checkForClickMulti(const QJsonObject &node);
    bool checkForDelayClickNode(const QJsonObject &node);
    bool checkForClickTwice(const QJsonObject &node);
    bool checkForSteerWhell(const QJsonObject &node);
    bool checkForDrag(const QJsonObject &node);
    bool checkForAndroidKey(const QJsonObject &node);

    // 从 json 对象中获取按键映射
    QString getItemString(const QJsonObject &node, const QString &name);
    double getItemDouble(const QJsonObject &node, const QString &name);
    bool getItemBool(const QJsonObject &node, const QString &name);
    QJsonObject getItemObject(const QJsonObject &node, const QString &name);
    QPointF getItemPos(const QJsonObject &node, const QString &name);
    QPair<ActionType, int> getItemKey(const QJsonObject &node, const QString &name);
    KeyMapType getItemKeyMapType(const QJsonObject &node, const QString &name);

private:
    static QString s_keyMapPath;

    QVector<KeyMapNode> m_keyMapNodes;
    KeyNode m_switchKey = { AT_KEY, Qt::Key_QuoteLeft };

    // 仅用于返回
    KeyMapNode m_invalidNode;

    // 方向盘索引
    int m_idxSteerWheel = -1;

    // 鼠标移动索引
    int m_idxMouseMove = -1;

    // 按键/鼠标事件名称到索引的映射
    QMetaEnum m_metaEnumKey = QMetaEnum::fromType<Qt::Key>();
    QMetaEnum m_metaEnumMouseButtons = QMetaEnum::fromType<Qt::MouseButtons>();
    QMetaEnum m_metaEnumKeyMapType = QMetaEnum::fromType<KeyMap::KeyMapType>();
    // 按键/鼠标事件的反向映射
    QMultiHash<int, KeyMapNode *> m_rmapKey;
    QMultiHash<int, KeyMapNode *> m_rmapMouse;
};

#endif // KEYMAP_H
