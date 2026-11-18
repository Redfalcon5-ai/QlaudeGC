#include "RouterEndpoint.h"

#include <algorithm>

#include <QtCore/QDateTime>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QUdpSocket>

#ifndef QGC_NO_SERIAL_LINK
#include <QtSerialPort/QSerialPort>
#endif

namespace {

constexpr qint64 UDP_CLIENT_ALIVE_MS = 5000;
constexpr int UART_RETRY_MS = 2000;

QHostAddress resolveHost(const QString &host)
{
    QHostAddress address(host);
    if (!address.isNull()) {
        return address;
    }
    const QHostInfo info = QHostInfo::fromName(host);
    for (const QHostAddress &candidate : info.addresses()) {
        if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
            return candidate;
        }
    }
    return info.addresses().isEmpty() ? QHostAddress() : info.addresses().first();
}

} // namespace

/*===========================================================================*/

RouterEndpoint::RouterEndpoint(const QString &name, const QString &kind, FrameCallback onFrame, QObject *parent)
    : QObject(parent)
    , _name(name)
    , _kind(kind)
    , _onFrame(std::move(onFrame))
    , _parser([this](const MavFrame &frame) {
        ++_rxMessages;
        _seenSystems.insert(frame.srcSystem);
        _seenComponents.insert(static_cast<quint16>((frame.srcSystem << 8) | frame.srcComponent));
        if (_onFrame) {
            _onFrame(this, frame);
        }
    })
{
}

bool RouterEndpoint::send(const MavFrame &frame)
{
    if ((!_allowOut.isEmpty() && !_allowOut.contains(frame.msgId)) || _blockOut.contains(frame.msgId) ||
        (!_allowSrcSys.isEmpty() && !_allowSrcSys.contains(frame.srcSystem)) || _blockSrcSys.contains(frame.srcSystem)) {
        ++_txFiltered;
        return false;
    }
    if (!isConnected() || !_write(frame.raw)) {
        return false;
    }
    ++_txMessages;
    _sentSystems.insert(frame.srcSystem);
    _txBytes += static_cast<quint64>(frame.raw.size());
    return true;
}

void RouterEndpoint::setOutFilters(const RouterEndpointConfig &config)
{
    _allowOut = config.allowMsgIdOut;
    _blockOut = config.blockMsgIdOut;
    _allowSrcSys = config.allowSrcSysOut;
    _blockSrcSys = config.blockSrcSysOut;
}

bool RouterEndpoint::hasSeenComponent(uint8_t sysId, uint8_t compId) const
{
    return _seenComponents.contains(static_cast<quint16>((sysId << 8) | compId));
}

QString RouterEndpoint::seenSystemsText() const
{
    return _systemsText(_seenSystems);
}

QString RouterEndpoint::sentSystemsText() const
{
    return _systemsText(_sentSystems);
}

QString RouterEndpoint::_systemsText(const QSet<uint8_t> &set)
{
    QList<uint8_t> systems(set.cbegin(), set.cend());
    std::sort(systems.begin(), systems.end());
    QStringList parts;
    for (const uint8_t sys : systems) {
        parts << QString::number(sys);
    }
    return parts.join(QLatin1String(", "));
}

void RouterEndpoint::_receive(const QByteArray &bytes)
{
    _rxBytes += static_cast<quint64>(bytes.size());
    _parser.feed(bytes);
}

/*===========================================================================*/

UdpServerEndpoint::UdpServerEndpoint(const RouterEndpointConfig &config, FrameCallback onFrame, QObject *parent)
    : RouterEndpoint(config.name, QStringLiteral("UDP server"), std::move(onFrame), parent)
    , _config(config)
{
}

void UdpServerEndpoint::open()
{
    _socket = new QUdpSocket(this);
    (void) connect(_socket, &QUdpSocket::readyRead, this, &UdpServerEndpoint::_readPending);
    const QHostAddress bindAddress = resolveHost(_config.address);
    if (!_socket->bind(bindAddress, _config.port)) {
        emit logMessage(QStringLiteral("%1: cannot bind %2:%3: %4")
                            .arg(name(), _config.address)
                            .arg(_config.port)
                            .arg(_socket->errorString()));
    }
}

bool UdpServerEndpoint::isConnected() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return std::any_of(_peerLastSeen.cbegin(), _peerLastSeen.cend(),
                       [now](qint64 seen) { return (now - seen) < PEER_TIMEOUT_MS; });
}

QString UdpServerEndpoint::detail() const
{
    QString text = QStringLiteral("listen %1:%2").arg(_config.address).arg(_config.port);
    if (_socket && (_socket->state() != QAbstractSocket::BoundState)) {
        text += QStringLiteral(" (bind failed)");
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList active;
    for (auto it = _peerLastSeen.cbegin(); it != _peerLastSeen.cend(); ++it) {
        if ((now - it.value()) < PEER_TIMEOUT_MS) {
            active << it.key();
        }
    }
    if (!active.isEmpty()) {
        text += QStringLiteral(" <- ") + active.join(QLatin1String(", "));
    }
    return text;
}

bool UdpServerEndpoint::_write(const QByteArray &bytes)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool sent = false;
    for (auto it = _peers.cbegin(); it != _peers.cend(); ++it) {
        if ((now - _peerLastSeen.value(it.key())) >= PEER_TIMEOUT_MS) {
            continue;
        }
        if (_socket->writeDatagram(bytes, it.value().first, it.value().second) == bytes.size()) {
            sent = true;
        }
    }
    return sent;
}

void UdpServerEndpoint::_readPending()
{
    while (_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<qsizetype>(_socket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 read = _socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        if (read <= 0) {
            continue;
        }
        datagram.resize(static_cast<qsizetype>(read));
        const QString key = QStringLiteral("%1:%2").arg(sender.toString()).arg(senderPort);
        if (!_peers.contains(key)) {
            _peers.insert(key, qMakePair(sender, senderPort));
            emit logMessage(QStringLiteral("%1: new peer %2").arg(name(), key));
        }
        _peerLastSeen.insert(key, QDateTime::currentMSecsSinceEpoch());
        // Datagrams carry whole frames; a fresh parser state avoids splicing peers together.
        _resetParser();
        _receive(datagram);
    }
}

/*===========================================================================*/

UdpClientEndpoint::UdpClientEndpoint(const RouterEndpointConfig &config, FrameCallback onFrame, quint16 localPort,
                                     const QString &kind, QObject *parent)
    : RouterEndpoint(config.name, kind, std::move(onFrame), parent)
    , _config(config)
    , _localPort(localPort)
{
}

void UdpClientEndpoint::open()
{
    _socket = new QUdpSocket(this);
    (void) connect(_socket, &QUdpSocket::readyRead, this, &UdpClientEndpoint::_readPending);
    _target = resolveHost(_config.address);
    if (_target.isNull()) {
        emit logMessage(QStringLiteral("%1: cannot resolve %2").arg(name(), _config.address));
    }
    const QHostAddress bindAddress = _target.isLoopback() ? QHostAddress(QHostAddress::LocalHost)
                                                          : QHostAddress(QHostAddress::AnyIPv4);
    if (!_socket->bind(bindAddress, _localPort)) {
        emit logMessage(QStringLiteral("%1: cannot bind local port %2: %3")
                            .arg(name())
                            .arg(_localPort)
                            .arg(_socket->errorString()));
    }
}

bool UdpClientEndpoint::isConnected() const
{
    // UDP is connectionless: we can always send if the target resolved.
    return _socket && !_target.isNull();
}

QString UdpClientEndpoint::detail() const
{
    QString text = QStringLiteral("send %1:%2").arg(_config.address).arg(_config.port);
    if ((QDateTime::currentMSecsSinceEpoch() - _lastRxMs) < UDP_CLIENT_ALIVE_MS) {
        text += QStringLiteral(" (receiving)");
    }
    return text;
}

bool UdpClientEndpoint::_write(const QByteArray &bytes)
{
    return _socket->writeDatagram(bytes, _target, _config.port) == bytes.size();
}

void UdpClientEndpoint::_readPending()
{
    while (_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<qsizetype>(_socket->pendingDatagramSize()));
        const qint64 read = _socket->readDatagram(datagram.data(), datagram.size());
        if (read <= 0) {
            continue;
        }
        datagram.resize(static_cast<qsizetype>(read));
        _lastRxMs = QDateTime::currentMSecsSinceEpoch();
        _resetParser();
        _receive(datagram);
    }
}

/*===========================================================================*/

TcpEndpoint::TcpEndpoint(const RouterEndpointConfig &config, FrameCallback onFrame, QObject *parent)
    : RouterEndpoint(config.name, QStringLiteral("TCP client"), std::move(onFrame), parent)
    , _config(config)
{
    _retryTimer.setSingleShot(true);
    _retryTimer.setInterval(_config.retryTimeoutSec * 1000);
    (void) connect(&_retryTimer, &QTimer::timeout, this, [this]() {
        _socket->abort();
        _socket->connectToHost(_config.address, _config.port);
    });
}

TcpEndpoint::TcpEndpoint(QTcpSocket *accepted, const QString &name, FrameCallback onFrame, QObject *parent)
    : RouterEndpoint(name, QStringLiteral("TCP server client"), std::move(onFrame), parent)
    , _socket(accepted)
    , _accepted(true)
{
    _socket->setParent(this);
    _peerText = QStringLiteral("%1:%2").arg(_socket->peerAddress().toString()).arg(_socket->peerPort());
}

void TcpEndpoint::open()
{
    if (_accepted) {
        _wireSocket();
        return;
    }
    _socket = new QTcpSocket(this);
    _wireSocket();
    _socket->connectToHost(_config.address, _config.port);
}

void TcpEndpoint::_wireSocket()
{
    _socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    (void) connect(_socket, &QTcpSocket::readyRead, this, [this]() { _receive(_socket->readAll()); });
    (void) connect(_socket, &QTcpSocket::connected, this, [this]() {
        _resetParser();
        emit logMessage(QStringLiteral("%1: connected to %2:%3").arg(name(), _config.address).arg(_config.port));
    });
    (void) connect(_socket, &QTcpSocket::disconnected, this, [this]() {
        if (_accepted) {
            emit logMessage(QStringLiteral("%1: client %2 disconnected").arg(name(), _peerText));
            emit closed(this);
        } else {
            _retryTimer.start();
        }
    });
    (void) connect(_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!_accepted && !_retryTimer.isActive()) {
            _retryTimer.start();
        }
    });
}

bool TcpEndpoint::isConnected() const
{
    return _socket && (_socket->state() == QAbstractSocket::ConnectedState);
}

QString TcpEndpoint::detail() const
{
    if (_accepted) {
        return QStringLiteral("client %1").arg(_peerText);
    }
    return QStringLiteral("connect %1:%2%3")
        .arg(_config.address)
        .arg(_config.port)
        .arg(isConnected() ? QString() : QStringLiteral(" (retrying)"));
}

bool TcpEndpoint::_write(const QByteArray &bytes)
{
    return _socket->write(bytes) == bytes.size();
}

/*===========================================================================*/

QgcLinkEndpoint::QgcLinkEndpoint(quint64 linkKey, const QString &linkName, FrameCallback onFrame, LinkWriter writer,
                                 QObject *parent)
    : RouterEndpoint(linkName, QStringLiteral("QGC link"), std::move(onFrame), parent)
    , _linkKey(linkKey)
    , _writer(std::move(writer))
{
}

QString QgcLinkEndpoint::detail() const
{
    const bool alive = (QDateTime::currentMSecsSinceEpoch() - _lastRxMs) < ALIVE_MS;
    return alive ? QStringLiteral("QGC link (receiving)") : QStringLiteral("QGC link (idle)");
}

void QgcLinkEndpoint::inject(const QByteArray &frame)
{
    _lastRxMs = QDateTime::currentMSecsSinceEpoch();
    _receive(frame);
}

bool QgcLinkEndpoint::_write(const QByteArray &bytes)
{
    _writer(_linkKey, bytes);
    return true;
}

/*===========================================================================*/

#ifndef QGC_NO_SERIAL_LINK
UartEndpoint::UartEndpoint(const RouterEndpointConfig &config, FrameCallback onFrame, QObject *parent)
    : RouterEndpoint(config.name, QStringLiteral("Serial"), std::move(onFrame), parent)
    , _config(config)
{
    _retryTimer.setInterval(UART_RETRY_MS);
    (void) connect(&_retryTimer, &QTimer::timeout, this, &UartEndpoint::_tryOpen);
}

void UartEndpoint::open()
{
    _port = new QSerialPort(this);
    _port->setPortName(_config.device);
    _port->setBaudRate(_config.baud);
    _port->setFlowControl(_config.flowControl ? QSerialPort::HardwareControl : QSerialPort::NoFlowControl);
    (void) connect(_port, &QSerialPort::readyRead, this, [this]() { _receive(_port->readAll()); });
    (void) connect(_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if ((error == QSerialPort::NoError) || !_port->isOpen()) {
            return;
        }
        if ((error == QSerialPort::ResourceError) || (error == QSerialPort::PermissionError)) {
            emit logMessage(QStringLiteral("%1: %2 lost (%3), retrying").arg(name(), _config.device, _port->errorString()));
            _port->close();
            _retryTimer.start();
        }
    });
    _tryOpen();
    if (!_port->isOpen()) {
        _retryTimer.start();
    }
}

void UartEndpoint::_tryOpen()
{
    if (_port->isOpen()) {
        _retryTimer.stop();
        return;
    }
    if (_port->open(QIODevice::ReadWrite)) {
        _retryTimer.stop();
        _lastError.clear();
        _resetParser();
        emit logMessage(QStringLiteral("%1: opened %2 @ %3").arg(name(), _config.device).arg(_config.baud));
        return;
    }
    const QString error = _port->errorString();
    if (error != _lastError) {
        _lastError = error;
        emit logMessage(QStringLiteral("%1: cannot open %2: %3").arg(name(), _config.device, error));
    }
}

bool UartEndpoint::isConnected() const
{
    return _port && _port->isOpen();
}

QString UartEndpoint::detail() const
{
    QString text = QStringLiteral("%1 @ %2").arg(_config.device).arg(_config.baud);
    if (!isConnected()) {
        text += _lastError.isEmpty() ? QStringLiteral(" (closed)") : QStringLiteral(" (%1)").arg(_lastError);
    }
    return text;
}

bool UartEndpoint::_write(const QByteArray &bytes)
{
    return _port->write(bytes) == bytes.size();
}
#endif
