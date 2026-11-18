#include "RouterConfig.h"

#include <algorithm>

#include <QtCore/QFile>
#include <QtCore/QHash>

namespace {

struct Section
{
    QString type;               // lower-cased section type, e.g. "udpendpoint"
    QString name;
    int line = 0;
    QHash<QString, QString> keys; // lower-cased key -> value
};

QSet<uint32_t> parseIdList(const QString &value, bool &ok)
{
    QSet<uint32_t> ids;
    ok = true;
    for (const QString &part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool partOk = false;
        const uint id = part.trimmed().toUInt(&partOk);
        if (!partOk) {
            ok = false;
            continue;
        }
        ids.insert(id);
    }
    return ids;
}

bool parseBool(const QString &value)
{
    const QString v = value.trimmed().toLower();
    return (v == QLatin1String("true")) || (v == QLatin1String("1")) || (v == QLatin1String("yes")) ||
           (v == QLatin1String("on"));
}

bool parsePort(const QString &value, quint16 &port)
{
    bool ok = false;
    const uint p = value.trimmed().toUInt(&ok);
    if (!ok || (p > 65535)) {
        return false;
    }
    port = static_cast<quint16>(p);
    return true;
}

} // namespace

bool RouterConfig::parse(const QString &text, RouterConfig &config, QStringList &errors, QStringList &warnings)
{
    config = RouterConfig();
    QList<Section> sections;
    Section *current = nullptr;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'))) {
            continue;
        }
        if (line.startsWith(QLatin1Char('['))) {
            if (!line.endsWith(QLatin1Char(']'))) {
                errors << QStringLiteral("line %1: malformed section header").arg(i + 1);
                current = nullptr;
                continue;
            }
            const QString inner = line.mid(1, line.size() - 2).trimmed();
            const qsizetype space = inner.indexOf(QLatin1Char(' '));
            Section section;
            section.type = ((space < 0) ? inner : inner.left(space)).toLower();
            section.name = (space < 0) ? QString() : inner.mid(space + 1).trimmed();
            section.line = i + 1;
            sections.append(section);
            current = &sections.last();
            continue;
        }
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if ((eq <= 0) || !current) {
            warnings << QStringLiteral("line %1: ignored '%2'").arg(i + 1).arg(line);
            continue;
        }
        QString value = line.mid(eq + 1).trimmed();
        // Allow trailing comments ("Port = 14550  # drone")
        const qsizetype hash = value.indexOf(QLatin1String(" #"));
        if (hash >= 0) {
            value = value.left(hash).trimmed();
        }
        current->keys.insert(line.left(eq).trimmed().toLower(), value);
    }

    for (const Section &s : sections) {
        const auto key = [&s](const char *k) { return s.keys.value(QString::fromLatin1(k)); };
        const auto has = [&s](const char *k) { return s.keys.contains(QString::fromLatin1(k)); };

        if (s.type == QLatin1String("general")) {
            if (has("tcpserverport") && !parsePort(key("tcpserverport"), config.tcpServerPort)) {
                errors << QStringLiteral("[General] bad TcpServerPort '%1'").arg(key("tcpserverport"));
            }
            for (auto it = s.keys.cbegin(); it != s.keys.cend(); ++it) {
                if (it.key() != QLatin1String("tcpserverport")) {
                    warnings << QStringLiteral("[General] key '%1' not supported, ignored").arg(it.key());
                }
            }
            continue;
        }

        RouterEndpointConfig ep;
        ep.name = s.name.isEmpty() ? QStringLiteral("%1@%2").arg(s.type).arg(s.line) : s.name;
        const QString where = QStringLiteral("[%1 %2]").arg(s.type, ep.name);

        if (s.type == QLatin1String("udpendpoint")) {
            const QString mode = key("mode").toLower();
            if (mode == QLatin1String("server")) {
                ep.type = RouterEndpointConfig::Type::UdpServer;
            } else if (mode.isEmpty() || (mode == QLatin1String("normal")) || (mode == QLatin1String("client"))) {
                ep.type = RouterEndpointConfig::Type::UdpClient;
            } else {
                errors << QStringLiteral("%1 unsupported Mode '%2' (use Server or Normal)").arg(where, key("mode"));
                continue;
            }
            ep.address = has("address") ? key("address")
                                        : ((ep.type == RouterEndpointConfig::Type::UdpServer) ? QStringLiteral("0.0.0.0")
                                                                                              : QString());
            if (ep.address.isEmpty()) {
                errors << QStringLiteral("%1 needs Address").arg(where);
                continue;
            }
            if (!has("port") || !parsePort(key("port"), ep.port) || (ep.port == 0)) {
                errors << QStringLiteral("%1 needs a valid Port").arg(where);
                continue;
            }
        } else if (s.type == QLatin1String("tcpendpoint")) {
            ep.type = RouterEndpointConfig::Type::TcpClient;
            ep.address = key("address");
            if (ep.address.isEmpty() || !has("port") || !parsePort(key("port"), ep.port) || (ep.port == 0)) {
                errors << QStringLiteral("%1 needs Address and a valid Port").arg(where);
                continue;
            }
            if (has("retrytimeout")) {
                ep.retryTimeoutSec = qMax(1, key("retrytimeout").toInt());
            }
        } else if (s.type == QLatin1String("uartendpoint")) {
            ep.type = RouterEndpointConfig::Type::Uart;
            ep.device = key("device");
            if (ep.device.isEmpty()) {
                errors << QStringLiteral("%1 needs Device").arg(where);
                continue;
            }
            if (has("baud")) {
                // mavlink-router accepts a list to auto-detect; we use the first rate.
                bool ok = false;
                ep.baud = key("baud").split(QLatin1Char(',')).first().trimmed().toInt(&ok);
                if (!ok || (ep.baud <= 0)) {
                    errors << QStringLiteral("%1 bad Baud '%2'").arg(where, key("baud"));
                    continue;
                }
                if (key("baud").contains(QLatin1Char(','))) {
                    warnings << QStringLiteral("%1 baud auto-detect not supported, using %2").arg(where).arg(ep.baud);
                }
            }
            ep.flowControl = has("flowcontrol") && parseBool(key("flowcontrol"));
        } else {
            errors << QStringLiteral("line %1: unknown section type '%2'").arg(s.line).arg(s.type);
            continue;
        }

        bool ok = true;
        if (has("allowmsgidout")) {
            ep.allowMsgIdOut = parseIdList(key("allowmsgidout"), ok);
            if (!ok) {
                warnings << QStringLiteral("%1 AllowMsgIdOut has non-numeric entries").arg(where);
            }
        }
        const auto parseSysList = [&](const char *k, QSet<uint32_t> &target, const char *label) {
            if (!has(k)) {
                return;
            }
            bool listOk = true;
            target = parseIdList(key(k), listOk);
            if (!listOk) {
                warnings << QStringLiteral("%1 %2 has non-numeric entries").arg(where, QLatin1String(label));
            }
            for (const uint32_t id : std::as_const(target)) {
                if ((id == 0) || (id > 255)) {
                    errors << QStringLiteral("%1 %2: system id %3 out of range 1-255").arg(where, QLatin1String(label)).arg(id);
                }
            }
        };
        parseSysList("allowsrcsysout", ep.allowSrcSysOut, "AllowSrcSysOut");
        parseSysList("blocksrcsysout", ep.blockSrcSysOut, "BlockSrcSysOut");
        if (has("blockmsgidout")) {
            ep.blockMsgIdOut = parseIdList(key("blockmsgidout"), ok);
            if (!ok) {
                warnings << QStringLiteral("%1 BlockMsgIdOut has non-numeric entries").arg(where);
            }
        }
        config.endpoints.append(ep);
    }

    return errors.isEmpty();
}

bool RouterConfig::load(const QString &path, RouterConfig &config, QStringList &errors, QStringList &warnings)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errors << QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
        return false;
    }
    return parse(QString::fromUtf8(file.readAll()), config, errors, warnings);
}

namespace {

QString idListText(const QSet<uint32_t> &ids)
{
    QList<uint32_t> sorted(ids.cbegin(), ids.cend());
    std::sort(sorted.begin(), sorted.end());
    QStringList parts;
    for (const uint32_t id : sorted) {
        parts << QString::number(id);
    }
    return parts.join(QLatin1Char(','));
}

} // namespace

QString RouterConfig::toText() const
{
    QString out;
    out += QStringLiteral("# QlaudeGC built-in MAVLink router (mavlink-router main.conf format).\n"
                          "# Written by the router configuration editor. QGroundControl's own links are\n"
                          "# bridged into the router automatically and are not listed here.\n\n");
    out += QStringLiteral("[General]\nTcpServerPort = %1\n").arg(tcpServerPort);

    for (const RouterEndpointConfig &ep : endpoints) {
        out += QLatin1Char('\n');
        switch (ep.type) {
        case RouterEndpointConfig::Type::UdpServer:
        case RouterEndpointConfig::Type::UdpClient:
            out += QStringLiteral("[UdpEndpoint %1]\nMode = %2\nAddress = %3\nPort = %4\n")
                       .arg(ep.name,
                            (ep.type == RouterEndpointConfig::Type::UdpServer) ? QStringLiteral("Server")
                                                                               : QStringLiteral("Normal"),
                            ep.address)
                       .arg(ep.port);
            break;
        case RouterEndpointConfig::Type::TcpClient:
            out += QStringLiteral("[TcpEndpoint %1]\nAddress = %2\nPort = %3\nRetryTimeout = %4\n")
                       .arg(ep.name, ep.address)
                       .arg(ep.port)
                       .arg(ep.retryTimeoutSec);
            break;
        case RouterEndpointConfig::Type::Uart:
            out += QStringLiteral("[UartEndpoint %1]\nDevice = %2\nBaud = %3\nFlowControl = %4\n")
                       .arg(ep.name, ep.device)
                       .arg(ep.baud)
                       .arg(ep.flowControl ? QStringLiteral("true") : QStringLiteral("false"));
            break;
        }
        if (!ep.allowMsgIdOut.isEmpty()) {
            out += QStringLiteral("AllowMsgIdOut = %1\n").arg(idListText(ep.allowMsgIdOut));
        }
        if (!ep.allowSrcSysOut.isEmpty()) {
            out += QStringLiteral("AllowSrcSysOut = %1").arg(idListText(ep.allowSrcSysOut)) + QLatin1Char('\n');
        }
        if (!ep.blockSrcSysOut.isEmpty()) {
            out += QStringLiteral("BlockSrcSysOut = %1").arg(idListText(ep.blockSrcSysOut)) + QLatin1Char('\n');
        }
        if (!ep.blockMsgIdOut.isEmpty()) {
            out += QStringLiteral("BlockMsgIdOut = %1\n").arg(idListText(ep.blockMsgIdOut));
        }
    }
    return out;
}
