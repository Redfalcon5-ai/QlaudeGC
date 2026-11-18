#pragma once

#include <functional>

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>

#include "MavlinkFrameParser.h"
#include "RouterConfig.h"

class QSerialPort;
class QTcpSocket;
class QUdpSocket;

/// Base class for one router endpoint. Lives on the router thread.
class RouterEndpoint : public QObject
{
    Q_OBJECT

public:
    using FrameCallback = std::function<void(RouterEndpoint *, const MavFrame &)>;

    RouterEndpoint(const QString &name, const QString &kind, FrameCallback onFrame, QObject *parent = nullptr);

    virtual void open() = 0;
    [[nodiscard]] virtual bool isConnected() const = 0;
    [[nodiscard]] virtual QString detail() const = 0;

    /// Applies out filters, then writes. Returns true if the frame was written.
    bool send(const MavFrame &frame);

    /// Message-id and source-system (vehicle) filters applied to frames leaving through this endpoint.
    void setOutFilters(const RouterEndpointConfig &config);
    [[nodiscard]] bool hasSeenSystem(uint8_t sysId) const { return _seenSystems.contains(sysId); }
    [[nodiscard]] bool hasSeenComponent(uint8_t sysId, uint8_t compId) const;

    [[nodiscard]] const QString &name() const { return _name; }
    [[nodiscard]] const QString &kind() const { return _kind; }
    [[nodiscard]] QString seenSystemsText() const;
    /// Source systems this endpoint has actually forwarded (e.g. which vehicles a MAVSDK port receives).
    [[nodiscard]] QString sentSystemsText() const;
    [[nodiscard]] quint64 rxMessages() const { return _rxMessages; }
    [[nodiscard]] quint64 txMessages() const { return _txMessages; }
    [[nodiscard]] quint64 rxBytes() const { return _rxBytes; }
    [[nodiscard]] quint64 txBytes() const { return _txBytes; }
    [[nodiscard]] quint64 crcErrors() const { return _parser.crcErrors(); }
    [[nodiscard]] quint64 txFiltered() const { return _txFiltered; }

signals:
    void logMessage(const QString &message);

protected:
    /// Feed received bytes; frames are delivered to the router callback.
    void _receive(const QByteArray &bytes);
    void _resetParser() { _parser.reset(); }
    static QString _systemsText(const QSet<uint8_t> &set);
    virtual bool _write(const QByteArray &bytes) = 0;

private:
    QString _name;
    QString _kind;
    FrameCallback _onFrame;
    MavlinkFrameParser _parser;
    QSet<uint8_t> _seenSystems;
    QSet<quint16> _seenComponents; // (sys << 8) | comp
    QSet<uint32_t> _allowOut;
    QSet<uint32_t> _blockOut;
    QSet<uint32_t> _allowSrcSys;
    QSet<uint32_t> _blockSrcSys;
    QSet<uint8_t> _sentSystems;
    quint64 _rxMessages = 0;
    quint64 _txMessages = 0;
    quint64 _rxBytes = 0;
    quint64 _txBytes = 0;
    quint64 _txFiltered = 0;
};

/// UDP server: binds Address:Port, sends to every peer heard from in the last PEER_TIMEOUT_MS.
class UdpServerEndpoint : public RouterEndpoint
{
    Q_OBJECT

public:
    UdpServerEndpoint(const RouterEndpointConfig &config, FrameCallback onFrame, QObject *parent = nullptr);

    void open() override;
    bool isConnected() const override;
    QString detail() const override;

protected:
    bool _write(const QByteArray &bytes) override;

private:
    void _readPending();

    static constexpr qint64 PEER_TIMEOUT_MS = 15000;

    RouterEndpointConfig _config;
    QUdpSocket *_socket = nullptr;
    QHash<QString, qint64> _peerLastSeen; // "addr:port" -> msecs since epoch
    QHash<QString, QPair<QHostAddress, quint16>> _peers;
};

/// UDP client ("Normal" mode): sends to Address:Port from an ephemeral (or given) local port.
class UdpClientEndpoint : public RouterEndpoint
{
    Q_OBJECT

public:
    UdpClientEndpoint(const RouterEndpointConfig &config, FrameCallback onFrame, quint16 localPort = 0,
                      const QString &kind = QStringLiteral("UDP client"), QObject *parent = nullptr);

    void open() override;
    bool isConnected() const override;
    QString detail() const override;

protected:
    bool _write(const QByteArray &bytes) override;

private:
    void _readPending();

    RouterEndpointConfig _config;
    quint16 _localPort = 0;
    QHostAddress _target;
    QUdpSocket *_socket = nullptr;
    qint64 _lastRxMs = 0;
};

/// TCP client with automatic reconnect, or a wrapped accepted connection from the TCP server.
class TcpEndpoint : public RouterEndpoint
{
    Q_OBJECT

public:
    /// Outgoing client from a [TcpEndpoint] section.
    TcpEndpoint(const RouterEndpointConfig &config, FrameCallback onFrame, QObject *parent = nullptr);
    /// Accepted server connection (takes ownership of socket).
    TcpEndpoint(QTcpSocket *accepted, const QString &name, FrameCallback onFrame, QObject *parent = nullptr);

    void open() override;
    bool isConnected() const override;
    QString detail() const override;
    [[nodiscard]] bool isAccepted() const { return _accepted; }

signals:
    void closed(TcpEndpoint *endpoint);

protected:
    bool _write(const QByteArray &bytes) override;

private:
    void _wireSocket();

    RouterEndpointConfig _config;
    QTcpSocket *_socket = nullptr;
    QTimer _retryTimer;
    bool _accepted = false;
    QString _peerText;
};

/// One of QGC's own links, bridged into the router. Inbound frames are injected by the main thread
/// (from MAVLinkProtocol::messageReceived); outbound frames are handed back to the main thread to be
/// written with LinkInterface::writeBytesThreadSafe.
class QgcLinkEndpoint : public RouterEndpoint
{
    Q_OBJECT

public:
    using LinkWriter = std::function<void(quint64, const QByteArray &)>;

    QgcLinkEndpoint(quint64 linkKey, const QString &linkName, FrameCallback onFrame, LinkWriter writer,
                    QObject *parent = nullptr);

    void open() override { }
    bool isConnected() const override { return true; }
    QString detail() const override;
    void inject(const QByteArray &frame);

protected:
    bool _write(const QByteArray &bytes) override;

private:
    static constexpr qint64 ALIVE_MS = 5000;

    quint64 _linkKey = 0;
    LinkWriter _writer;
    qint64 _lastRxMs = 0;
};

#ifndef QGC_NO_SERIAL_LINK
/// Serial port; reopened every 2 s while unavailable (radio unplugged, etc.).
class UartEndpoint : public RouterEndpoint
{
    Q_OBJECT

public:
    UartEndpoint(const RouterEndpointConfig &config, FrameCallback onFrame, QObject *parent = nullptr);

    void open() override;
    bool isConnected() const override;
    QString detail() const override;

protected:
    bool _write(const QByteArray &bytes) override;

private:
    void _tryOpen();

    RouterEndpointConfig _config;
    QSerialPort *_port = nullptr;
    QTimer _retryTimer;
    QString _lastError;
};
#endif
