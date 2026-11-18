// Standalone build of the TAR router: same engine and main.conf as the one built into QlaudeGC.
// Usage: tar_router_cli <main.conf> [--stats]

#include <cstdio>

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include "MAVLinkLib.h"
#include "RouterConfig.h"
#include "RouterCore.h"

// MAVLinkMessageType.h declares these for QGC's channel handling; the router never parses with channels.
mavlink_status_t m_mavlink_status[MAVLINK_COMM_NUM_BUFFERS];
mavlink_status_t *mavlink_get_channel_status(uint8_t chan)
{
    return &m_mavlink_status[chan];
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: tar_router_cli <main.conf> [--stats]\n"
                             "       tar_router_cli --check <main.conf>   validate only (same rules as QGC Import)\n");
        return 2;
    }
    if ((args.at(1) == QLatin1String("--check")) && (args.size() >= 3)) {
        RouterConfig config;
        QStringList errors;
        QStringList warnings;
        const bool valid = RouterConfig::load(args.at(2), config, errors, warnings);
        for (const QString &w : warnings) {
            std::printf("warning: %s\n", qPrintable(w));
        }
        for (const QString &e : errors) {
            std::printf("error: %s\n", qPrintable(e));
        }
        std::printf("%s: %s, %d endpoint(s), TCP server port %d\n", qPrintable(args.at(2)), valid ? "valid" : "INVALID",
                    static_cast<int>(config.endpoints.size()), config.tcpServerPort);
        return valid ? 0 : 1;
    }
    const bool printStats = args.contains(QStringLiteral("--stats"));

    RouterConfig config;
    QStringList errors;
    QStringList warnings;
    const bool ok = RouterConfig::load(args.at(1), config, errors, warnings);
    for (const QString &w : warnings) {
        std::fprintf(stderr, "warning: %s\n", qPrintable(w));
    }
    for (const QString &e : errors) {
        std::fprintf(stderr, "error: %s\n", qPrintable(e));
    }
    if (!ok && config.endpoints.isEmpty()) {
        return 1;
    }

    RouterCore core;
    QObject::connect(&core, &RouterCore::logMessage, [](const QString &message) {
        std::printf("%s\n", qPrintable(message));
        std::fflush(stdout);
    });
    if (printStats) {
        QObject::connect(&core, &RouterCore::statsUpdated, [](const QVariantList &endpoints) {
            for (const QVariant &value : endpoints) {
                const QVariantMap m = value.toMap();
                std::printf("[stats] %-12s %-6s rx %5llu/s tx %5llu/s  rxTotal %llu txTotal %llu crc %llu  sys [%s]  %s\n",
                            qPrintable(m.value(QStringLiteral("name")).toString()),
                            m.value(QStringLiteral("connected")).toBool() ? "up" : "down",
                            m.value(QStringLiteral("rxRate")).toULongLong(),
                            m.value(QStringLiteral("txRate")).toULongLong(),
                            m.value(QStringLiteral("rxMessages")).toULongLong(),
                            m.value(QStringLiteral("txMessages")).toULongLong(),
                            m.value(QStringLiteral("crcErrors")).toULongLong(),
                            qPrintable(m.value(QStringLiteral("systems")).toString()),
                            qPrintable(m.value(QStringLiteral("detail")).toString()));
            }
            std::fflush(stdout);
        });
    }
    core.start(config);
    return app.exec();
}
