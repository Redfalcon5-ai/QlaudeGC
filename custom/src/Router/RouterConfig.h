#pragma once

#include <QtCore/QList>
#include <QtCore/QMetaType>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

/// One endpoint section of a mavlink-router style main.conf.
struct RouterEndpointConfig
{
    enum class Type { UdpServer, UdpClient, TcpClient, Uart };

    Type type = Type::UdpServer;
    QString name;
    QString address;            ///< UDP/TCP address (server: bind address)
    quint16 port = 0;
    QString device;             ///< UART device, e.g. COM5 or /dev/ttyUSB0
    qint32 baud = 57600;
    bool flowControl = false;
    int retryTimeoutSec = 5;    ///< TCP client reconnect interval
    QSet<uint32_t> allowMsgIdOut; ///< Empty = allow all
    QSet<uint32_t> blockMsgIdOut;
    QSet<uint32_t> allowSrcSysOut; ///< Only forward frames from these system ids (vehicles). Empty = all
    QSet<uint32_t> blockSrcSysOut;
};

/// Parsed router configuration. The file format is mavlink-router's main.conf (INI with
/// [General], [UdpEndpoint <name>], [TcpEndpoint <name>], [UartEndpoint <name>] sections);
/// keys and section types are case-insensitive. QGC's own links are bridged in automatically.
struct RouterConfig
{
    quint16 tcpServerPort = 5760; ///< 0 disables the TCP server
    QList<RouterEndpointConfig> endpoints;

    /// Parses text; returns false and fills errors for unusable sections. Warnings do not fail.
    static bool parse(const QString &text, RouterConfig &config, QStringList &errors, QStringList &warnings);
    static bool load(const QString &path, RouterConfig &config, QStringList &errors, QStringList &warnings);
    /// Serializes back to main.conf text (round-trips through parse()).
    [[nodiscard]] QString toText() const;
};

Q_DECLARE_METATYPE(RouterConfig)
