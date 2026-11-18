#include "TarRouter.h"

#include <algorithm>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTime>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

#ifndef QGC_NO_SERIAL_LINK
#include <QtSerialPort/QSerialPortInfo>
#endif

#include "LinkConfiguration.h"
#include "LinkInterface.h"
#include "LinkManager.h"
#include "MAVLinkLib.h"
#include "MAVLinkProtocol.h"
#include "QGCLoggingCategory.h"
#include "RouterCore.h"

QGC_LOGGING_CATEGORY(TarRouterLog, "Custom.TarRouter")

namespace {

constexpr const char *SETTINGS_GROUP = "TarRouter";
constexpr const char *ENABLED_KEY = "enabled";
constexpr const char *DEFAULT_CONFIG_RESOURCE = ":/tar/router/main.conf";

QString typeKey(RouterEndpointConfig::Type type)
{
    switch (type) {
    case RouterEndpointConfig::Type::UdpServer:
        return QStringLiteral("udpServer");
    case RouterEndpointConfig::Type::UdpClient:
        return QStringLiteral("udpClient");
    case RouterEndpointConfig::Type::TcpClient:
        return QStringLiteral("tcpClient");
    case RouterEndpointConfig::Type::Uart:
        return QStringLiteral("uart");
    }
    return QStringLiteral("udpClient");
}

QString idListText(const QSet<uint32_t> &ids)
{
    QList<uint32_t> sorted(ids.cbegin(), ids.cend());
    std::sort(sorted.begin(), sorted.end());
    QStringList parts;
    for (const uint32_t id : sorted) {
        parts << QString::number(id);
    }
    return parts.join(QLatin1String(", "));
}

/// File dialogs may hand back either a plain path or a file: URL.
QString localPath(const QString &file)
{
    const QUrl url(file);
    return (url.isLocalFile()) ? url.toLocalFile() : file.trimmed();
}

QString sectionName(const QString &name)
{
    // Section names end at ']' and are trimmed by the parser; keep them simple.
    QString cleaned = name.trimmed();
    cleaned.replace(QLatin1Char(']'), QLatin1Char('_'));
    cleaned.replace(QLatin1Char('['), QLatin1Char('_'));
    return cleaned;
}

} // namespace

TarRouter::TarRouter(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<RouterConfig>("RouterConfig");

    QSettings settings;
    settings.beginGroup(QLatin1String(SETTINGS_GROUP));
    _enabled = settings.value(QLatin1String(ENABLED_KEY), true).toBool();
    settings.endGroup();

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    _configPath = QDir(dir).filePath(QStringLiteral("router/main.conf"));

    _core = new RouterCore();
    _core->moveToThread(&_thread);
    (void) connect(&_thread, &QThread::finished, _core, &QObject::deleteLater);
    (void) connect(_core, &RouterCore::statsUpdated, this, [this](const QVariantList &endpoints) {
        _endpoints = endpoints;
        emit endpointsChanged();
    });
    (void) connect(_core, &RouterCore::logMessage, this, &TarRouter::_appendLog);
    (void) connect(_core, &RouterCore::writeToQgcLink, this, &TarRouter::_writeToQgcLink);
    _thread.setObjectName(QStringLiteral("TarRouter"));
    _thread.start();
}

TarRouter::~TarRouter()
{
    shutdown();
}

void TarRouter::init()
{
    // Direct connection: serialize on whatever thread MAVLinkProtocol emits on, then hop to the router thread.
    (void) connect(MAVLinkProtocol::instance(), &MAVLinkProtocol::messageReceived, this, &TarRouter::_onQgcMessage,
                   Qt::DirectConnection);
    _ensureConfigFile();
    if (_enabled) {
        _start();
    }
}

void TarRouter::shutdown()
{
    if (!_thread.isRunning()) {
        return;
    }
    (void) disconnect(MAVLinkProtocol::instance(), &MAVLinkProtocol::messageReceived, this, nullptr);
    _stop();
    _thread.quit();
    (void) _thread.wait(3000);
    _core = nullptr;
}

void TarRouter::setEnabled(bool enabled)
{
    if (enabled == _enabled) {
        return;
    }
    _enabled = enabled;
    QSettings settings;
    settings.beginGroup(QLatin1String(SETTINGS_GROUP));
    settings.setValue(QLatin1String(ENABLED_KEY), _enabled);
    settings.endGroup();
    emit enabledChanged();

    if (_enabled) {
        _start();
    } else {
        _stop();
    }
}

void TarRouter::reload()
{
    _ensureConfigFile();
    if (_enabled) {
        _start();
    }
}

void TarRouter::openConfigFile()
{
    _ensureConfigFile();
    (void) QDesktopServices::openUrl(QUrl::fromLocalFile(_configPath));
}

void TarRouter::openConfigFolder()
{
    _ensureConfigFile();
    (void) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(_configPath).absolutePath()));
}

QVariantMap TarRouter::loadConfigForEdit()
{
    _ensureConfigFile();
    RouterConfig config;
    QStringList errors;
    QStringList warnings;
    (void) RouterConfig::load(_configPath, config, errors, warnings);

    QVariantList endpoints;
    for (const RouterEndpointConfig &ep : std::as_const(config.endpoints)) {
        QVariantMap map;
        map[QStringLiteral("type")] = typeKey(ep.type);
        map[QStringLiteral("name")] = ep.name;
        map[QStringLiteral("address")] = ep.address;
        map[QStringLiteral("port")] = ep.port;
        map[QStringLiteral("device")] = ep.device;
        map[QStringLiteral("baud")] = ep.baud;
        map[QStringLiteral("flowControl")] = ep.flowControl;
        map[QStringLiteral("retryTimeout")] = ep.retryTimeoutSec;
        map[QStringLiteral("allowMsgIdOut")] = idListText(ep.allowMsgIdOut);
        map[QStringLiteral("blockMsgIdOut")] = idListText(ep.blockMsgIdOut);
        map[QStringLiteral("allowSrcSysOut")] = idListText(ep.allowSrcSysOut);
        map[QStringLiteral("blockSrcSysOut")] = idListText(ep.blockSrcSysOut);
        endpoints.append(map);
    }

    QVariantMap result;
    result[QStringLiteral("tcpServerPort")] = config.tcpServerPort;
    result[QStringLiteral("endpoints")] = endpoints;
    result[QStringLiteral("loadErrors")] = errors;
    return result;
}

QString TarRouter::saveConfigFromEdit(const QVariantMap &edited)
{
    // Build main.conf text from the edited map, then validate it with the same parser the router uses.
    RouterConfig config;
    bool ok = false;
    const uint tcpPort = edited.value(QStringLiteral("tcpServerPort")).toUInt(&ok);
    QStringList problems;
    if (!ok || (tcpPort > 65535)) {
        problems << tr("TCP server port must be 0-65535");
    }
    config.tcpServerPort = static_cast<quint16>(tcpPort);

    QSet<QString> names;
    const QVariantList endpoints = edited.value(QStringLiteral("endpoints")).toList();
    for (int i = 0; i < endpoints.size(); ++i) {
        const QVariantMap map = endpoints.at(i).toMap();
        RouterEndpointConfig ep;
        ep.name = sectionName(map.value(QStringLiteral("name")).toString());
        if (ep.name.isEmpty()) {
            ep.name = QStringLiteral("endpoint%1").arg(i + 1);
        }
        if (names.contains(ep.name.toLower())) {
            problems << tr("Endpoint name '%1' is used twice").arg(ep.name);
        }
        names.insert(ep.name.toLower());

        const QString type = map.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("udpServer")) {
            ep.type = RouterEndpointConfig::Type::UdpServer;
        } else if (type == QLatin1String("tcpClient")) {
            ep.type = RouterEndpointConfig::Type::TcpClient;
        } else if (type == QLatin1String("uart")) {
            ep.type = RouterEndpointConfig::Type::Uart;
        } else {
            ep.type = RouterEndpointConfig::Type::UdpClient;
        }
        ep.address = map.value(QStringLiteral("address")).toString().trimmed();
        ep.port = static_cast<quint16>(map.value(QStringLiteral("port")).toUInt());
        ep.device = map.value(QStringLiteral("device")).toString().trimmed();
        ep.baud = map.value(QStringLiteral("baud")).toInt();
        ep.flowControl = map.value(QStringLiteral("flowControl")).toBool();
        ep.retryTimeoutSec = qMax(1, map.value(QStringLiteral("retryTimeout")).toInt());
        // Id lists are validated by the round-trip parse below.
        // Id lists; ranges are validated by the round-trip parse below.
        const struct
        {
            const char *key;
            QSet<uint32_t> *ids;
            QString what;
        } lists[] = {
            {"allowMsgIdOut", &ep.allowMsgIdOut, tr("message id")},
            {"blockMsgIdOut", &ep.blockMsgIdOut, tr("message id")},
            {"allowSrcSysOut", &ep.allowSrcSysOut, tr("vehicle (system) id")},
            {"blockSrcSysOut", &ep.blockSrcSysOut, tr("vehicle (system) id")},
        };
        for (const auto &list : lists) {
            const QStringList parts =
                map.value(QString::fromLatin1(list.key)).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                bool idOk = false;
                const uint id = part.trimmed().toUInt(&idOk);
                if (!idOk) {
                    problems << tr("%1: '%2' is not a %3").arg(ep.name, part.trimmed(), list.what);
                    continue;
                }
                list.ids->insert(id);
            }
        }
        config.endpoints.append(ep);
    }

    const QString text = config.toText();
    RouterConfig check;
    QStringList warnings;
    (void) RouterConfig::parse(text, check, problems, warnings);
    if (!problems.isEmpty()) {
        return problems.join(QLatin1Char('\n'));
    }

    const QString error = _writeActiveConfig(text);
    if (!error.isEmpty()) {
        return error;
    }
    _appendLog(QStringLiteral("configuration saved (%1 endpoint(s))").arg(config.endpoints.size()));
    if (_enabled) {
        _start();
    }
    return QString();
}

QString TarRouter::exportFolder() const
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

QString TarRouter::exportConfig(const QString &file)
{
    _ensureConfigFile();
    const QString target = localPath(file);
    if (target.isEmpty()) {
        return tr("No file selected");
    }
    QFile source(_configPath);
    if (!source.open(QIODevice::ReadOnly)) {
        return tr("Cannot read %1: %2").arg(_configPath, source.errorString());
    }
    const QByteArray contents = source.readAll();

    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly) || (out.write(contents) < 0) || !out.commit()) {
        return tr("Cannot write %1: %2").arg(target, out.errorString());
    }
    _appendLog(QStringLiteral("exported active config to %1").arg(target));
    return QString();
}

QString TarRouter::importConfig(const QString &file)
{
    const QString source = localPath(file);
    if (source.isEmpty()) {
        return tr("No file selected");
    }
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return tr("Cannot read %1: %2").arg(source, in.errorString());
    }
    const QString text = QString::fromUtf8(in.readAll());

    RouterConfig config;
    QStringList errors;
    QStringList warnings;
    if (!RouterConfig::parse(text, config, errors, warnings)) {
        return tr("%1 is not a valid router config:\n%2").arg(QFileInfo(source).fileName(), errors.join(QLatin1Char('\n')));
    }
    if (config.endpoints.isEmpty() && (config.tcpServerPort == 0)) {
        return tr("%1 has no endpoints and no TCP server - nothing to route").arg(QFileInfo(source).fileName());
    }

    // Keep the imported file verbatim (comments included) as the active config.
    const QString error = _writeActiveConfig(text);
    if (!error.isEmpty()) {
        return error;
    }
    for (const QString &warning : std::as_const(warnings)) {
        _appendLog(QStringLiteral("import warning: ") + warning);
    }
    _appendLog(QStringLiteral("imported %1 (%2 endpoint(s))").arg(source).arg(config.endpoints.size()));
    if (_enabled) {
        _start();
    }
    return QString();
}

QString TarRouter::_writeActiveConfig(const QString &text)
{
    (void) QDir().mkpath(QFileInfo(_configPath).absolutePath());
    QSaveFile file(_configPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || (file.write(text.toUtf8()) < 0) || !file.commit()) {
        return tr("Cannot write %1: %2").arg(_configPath, file.errorString());
    }
    return QString();
}

QStringList TarRouter::serialPorts() const
{
    QStringList ports;
#ifndef QGC_NO_SERIAL_LINK
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        ports << info.portName();
    }
#endif
    return ports;
}

int TarRouter::activeEndpoints() const
{
    return static_cast<int>(std::count_if(_endpoints.cbegin(), _endpoints.cend(), [](const QVariant &value) {
        return value.toMap().value(QStringLiteral("rxRate")).toULongLong() > 0;
    }));
}

void TarRouter::_start()
{
    if (!_core) {
        return;
    }

    RouterConfig config;
    QStringList errors;
    QStringList warnings;
    const bool ok = RouterConfig::load(_configPath, config, errors, warnings);
    for (const QString &warning : std::as_const(warnings)) {
        _appendLog(QStringLiteral("config warning: ") + warning);
    }
    for (const QString &error : std::as_const(errors)) {
        _appendLog(QStringLiteral("config error: ") + error);
    }
    _configErrors = errors;
    emit configErrorsChanged();
    if (!ok && config.endpoints.isEmpty()) {
        _stop();
        return;
    }

    _bridgedLinks.clear();
    (void) QMetaObject::invokeMethod(_core, "start", Qt::QueuedConnection, Q_ARG(RouterConfig, config));
    _appendLog(QStringLiteral("router started from %1 (%2 endpoint(s))").arg(_configPath).arg(config.endpoints.size()));
    if (!_running) {
        _running = true;
        emit runningChanged();
    }
}

void TarRouter::_stop()
{
    if (_core) {
        (void) QMetaObject::invokeMethod(_core, "stop", Qt::BlockingQueuedConnection);
    }
    _bridgedLinks.clear();
    _endpoints.clear();
    emit endpointsChanged();
    if (_running) {
        _running = false;
        _appendLog(QStringLiteral("router stopped"));
        emit runningChanged();
    }
}

void TarRouter::_ensureConfigFile()
{
    if (QFile::exists(_configPath)) {
        return;
    }
    (void) QDir().mkpath(QFileInfo(_configPath).absolutePath());
    if (QFile::copy(QString::fromLatin1(DEFAULT_CONFIG_RESOURCE), _configPath)) {
        // Resource copies are read-only.
        (void) QFile::setPermissions(_configPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser |
                                                      QFile::WriteUser);
        _appendLog(QStringLiteral("wrote default config %1").arg(_configPath));
    } else {
        qCWarning(TarRouterLog) << "cannot write default config to" << _configPath;
    }
}

void TarRouter::_onQgcMessage(LinkInterface *link, const mavlink_message_t &message)
{
    if (!_running || !link || !_core) {
        return;
    }

    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    const uint16_t len = mavlink_msg_to_send_buffer(buffer, &message);
    const QByteArray frame(reinterpret_cast<const char *>(buffer), len);
    const quint64 key = reinterpret_cast<quintptr>(link);

    QString name = QStringLiteral("QGC link");
    const SharedLinkConfigurationPtr config = link->linkConfiguration();
    if (config) {
        name = QStringLiteral("QGC: ") + config->name();
    }

    if (!_bridgedLinks.contains(key)) {
        // First message on this link: watch for it going away (hop to our thread for the bookkeeping).
        _bridgedLinks.insert(key);
        const auto drop = [this, key]() {
            _bridgedLinks.remove(key);
            if (_core) {
                (void) QMetaObject::invokeMethod(_core, "removeQgcLink", Qt::QueuedConnection, Q_ARG(quint64, key));
            }
        };
        (void) connect(link, &LinkInterface::disconnected, this, drop, Qt::QueuedConnection);
        (void) connect(link, &QObject::destroyed, this, drop, Qt::QueuedConnection);
    }

    (void) QMetaObject::invokeMethod(_core, "injectQgcLinkFrame", Qt::QueuedConnection, Q_ARG(quint64, key),
                                     Q_ARG(QString, name), Q_ARG(QByteArray, frame));
}

void TarRouter::_writeToQgcLink(quint64 linkKey, const QByteArray &bytes)
{
    // Only write to a link LinkManager still owns; the key may refer to a link that just closed.
    for (const SharedLinkInterfacePtr &link : LinkManager::instance()->links()) {
        if (link && (reinterpret_cast<quintptr>(link.get()) == linkKey)) {
            link->writeBytesThreadSafe(bytes.constData(), static_cast<int>(bytes.size()));
            return;
        }
    }
}

void TarRouter::_appendLog(const QString &message)
{
    qCDebug(TarRouterLog) << message;
    _log.append(QTime::currentTime().toString(QStringLiteral("HH:mm:ss ")) + message);
    while (_log.size() > MAX_LOG_LINES) {
        _log.removeFirst();
    }
    emit logChanged();
}
