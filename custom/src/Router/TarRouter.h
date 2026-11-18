#pragma once

#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QThread>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>

#include "MAVLinkMessageType.h"
#include "RouterConfig.h"

class LinkInterface;
class RouterCore;

Q_DECLARE_LOGGING_CATEGORY(TarRouterLog)

/// Built-in MAVLink router (mavlink-router compatible main.conf). Main-thread facade exposed to QML as
/// the `tarRouter` context property. QGC keeps its normal links (e.g. UDP 14550); every QGC link is
/// bridged into the router, which fans traffic out to the configured endpoints (e.g. MAVSDK on 14540).
/// Routing runs on a dedicated thread so UI stalls never delay traffic between the other endpoints.
class TarRouter : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         enabled         READ enabled        WRITE setEnabled    NOTIFY enabledChanged)
    Q_PROPERTY(bool         running         READ running                            NOTIFY runningChanged)
    Q_PROPERTY(QString      configPath      READ configPath                         CONSTANT)
    Q_PROPERTY(QVariantList endpoints       READ endpoints                          NOTIFY endpointsChanged)
    Q_PROPERTY(QStringList  log             READ log                                NOTIFY logChanged)
    Q_PROPERTY(QStringList  configErrors    READ configErrors                       NOTIFY configErrorsChanged)
    Q_PROPERTY(int          activeEndpoints READ activeEndpoints                    NOTIFY endpointsChanged)
    Q_PROPERTY(QString      exportFolder    READ exportFolder                       CONSTANT)

public:
    explicit TarRouter(QObject *parent = nullptr);
    ~TarRouter() override;

    /// Starts the router if enabled. Call once the QGC core is up.
    void init();
    void shutdown();

    Q_INVOKABLE void reload();
    Q_INVOKABLE void openConfigFile();
    Q_INVOKABLE void openConfigFolder();

    /// Current main.conf as an editable map for the GUI editor:
    /// { tcpServerPort, endpoints: [ { type: "udpServer"|"udpClient"|"tcpClient"|"uart", name, address, port,
    ///   device, baud, flowControl, retryTimeout, allowMsgIdOut, blockMsgIdOut, allowSrcSysOut, blockSrcSysOut } ] }
    Q_INVOKABLE QVariantMap loadConfigForEdit();
    /// Validates, writes main.conf and restarts the router. Returns an empty string on success,
    /// otherwise the validation errors (file left untouched).
    Q_INVOKABLE QString saveConfigFromEdit(const QVariantMap &config);
    Q_INVOKABLE QStringList serialPorts() const;
    /// Saves the active main.conf to file (path or file: URL). Returns an empty string or an error.
    Q_INVOKABLE QString exportConfig(const QString &file);
    /// Validates file as a main.conf; if valid it becomes the active config and the router restarts.
    /// Returns an empty string on success, otherwise the errors (active config untouched).
    Q_INVOKABLE QString importConfig(const QString &file);

    bool enabled() const { return _enabled; }
    void setEnabled(bool enabled);
    bool running() const { return _running; }
    QString configPath() const { return _configPath; }
    QString exportFolder() const;
    QVariantList endpoints() const { return _endpoints; }
    QStringList log() const { return _log; }
    QStringList configErrors() const { return _configErrors; }
    /// Endpoints that received traffic in the last second.
    int activeEndpoints() const;

signals:
    void enabledChanged();
    void runningChanged();
    void endpointsChanged();
    void logChanged();
    void configErrorsChanged();

private:
    void _start();
    void _stop();
    void _ensureConfigFile();
    /// Writes text atomically to the active config path; returns an empty string or an error.
    QString _writeActiveConfig(const QString &text);
    void _onQgcMessage(LinkInterface *link, const mavlink_message_t &message);
    void _writeToQgcLink(quint64 linkKey, const QByteArray &bytes);
    void _appendLog(const QString &message);

    static constexpr int MAX_LOG_LINES = 200;

    QThread _thread;
    RouterCore *_core = nullptr;
    bool _enabled = true;
    bool _running = false;
    QString _configPath;
    QVariantList _endpoints;
    QStringList _log;
    QStringList _configErrors;
    QSet<quint64> _bridgedLinks; ///< QGC links whose disconnect we already watch
};
