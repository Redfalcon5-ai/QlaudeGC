// Generic access to QGC's object tree (Q_PROPERTY / Q_INVOKABLE reflection) and Comm Link management
// for the Claude sidebar. Every change goes through the same approval card as parameter changes.

#include "TarMcpServer.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QMetaEnum>
#include <QtCore/QMetaMethod>
#include <QtCore/QMetaProperty>
#include <QtCore/QPointF>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtGui/QColor>
#include <QtPositioning/QGeoCoordinate>
#include <QtNetwork/QTcpSocket>
#include <QtQml/QQmlApplicationEngine>

#include "ADSBVehicleManager.h"
#include "Fact.h"
#include "FactGroup.h"
#include "GeoFenceManager.h"
#include "JoystickManager.h"
#include "LinkConfiguration.h"
#include "LinkManager.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "PositionManager.h"
#include "QGCCorePlugin.h"
#include "QmlObjectListModel.h"
#include "RallyPointManager.h"
#include "SettingsManager.h"
#include "Vehicle.h"
#include "VideoManager.h"
#ifndef QGC_NO_SERIAL_LINK
#include <QtSerialPort/QSerialPortInfo>
#endif

namespace {

constexpr int kMaxListItems = 60;

QString jsonText(const QJsonValue &v)
{
    if (v.isObject()) {
        return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented));
    }
    if (v.isArray()) {
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented));
    }
    return v.toVariant().toString();
}

QString shortText(const QJsonValue &v)
{
    QString s = v.isObject() || v.isArray()
                    ? QString::fromUtf8(v.isObject() ? QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)
                                                     : QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact))
                    : v.toVariant().toString();
    if (s.size() > 200) {
        s = s.left(197) + QStringLiteral("...");
    }
    return s;
}

/// Read-only by name: these calls run without an approval card.
bool isReadOnlyMethodName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral(
        "^(get|is|has|find|contains|count|minimum|maximum|version|load|to|from|vehicleClass|altitudeFrame|linesIntersect|"
        "expandedToolbarIndicatorSource|indexOf|value|format|convert|lookup)"));
    return re.match(name).hasMatch();
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Value conversion
// ---------------------------------------------------------------------------------------------------------

QJsonValue TarMcpServer::_toJson(const QVariant &value, int depth)
{
    if (!value.isValid() || value.isNull()) {
        return QJsonValue::Null;
    }
    const QMetaType mt = value.metaType();
    if (mt.id() == QMetaType::QVariant) {
        return _toJson(value.value<QVariant>(), depth);
    }
    if (mt.flags() & QMetaType::PointerToQObject) {
        QObject *obj = value.value<QObject *>();
        if (!obj) {
            return QJsonValue::Null;
        }
        if (auto *list = qobject_cast<QmlObjectListModel *>(obj)) {
            return QJsonObject{{QStringLiteral("list"), QString::fromLatin1(obj->metaObject()->className())}, {QStringLiteral("count"), list->count()}};
        }
        if (auto *fact = qobject_cast<Fact *>(obj)) {
            QJsonObject o{{QStringLiteral("fact"), fact->rawValueString()}};
            if (!fact->rawUnits().isEmpty()) {
                o.insert(QStringLiteral("units"), fact->rawUnits());
            }
            return o;
        }
        return QJsonObject{{QStringLiteral("object"), QString::fromLatin1(obj->metaObject()->className())}};
    }
    if (mt.id() == QMetaType::fromType<QGeoCoordinate>().id()) {
        const QGeoCoordinate c = value.value<QGeoCoordinate>();
        if (!c.isValid()) {
            return QStringLiteral("(invalid coordinate)");
        }
        QJsonObject o{{QStringLiteral("lat"), c.latitude()}, {QStringLiteral("lon"), c.longitude()}};
        if (!qIsNaN(c.altitude())) {
            o.insert(QStringLiteral("alt"), c.altitude());
        }
        return o;
    }
    if (mt.flags() & QMetaType::IsEnumeration) {
        const QMetaObject *mo = mt.metaObject();
        const int v = value.toInt();
        if (mo) {
            QString name = QString::fromLatin1(mt.name());
            name = name.mid(name.lastIndexOf(QLatin1Char(':')) + 1);
            const int idx = mo->indexOfEnumerator(name.toLatin1().constData());
            if (idx >= 0) {
                const char *key = mo->enumerator(idx).valueToKey(v);
                if (key) {
                    return QString::fromLatin1(key);
                }
            }
        }
        return v;
    }
    switch (mt.id()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int: case QMetaType::UInt: case QMetaType::Short: case QMetaType::UShort:
    case QMetaType::Char: case QMetaType::SChar: case QMetaType::UChar:
    case QMetaType::Long: case QMetaType::ULong: case QMetaType::LongLong: case QMetaType::ULongLong:
        return value.toLongLong();
    case QMetaType::Double: case QMetaType::Float:
        return qIsFinite(value.toDouble()) ? QJsonValue(value.toDouble()) : QJsonValue(value.toString());
    case QMetaType::QString:
        return value.toString();
    case QMetaType::QUrl:
        return value.toUrl().toString();
    case QMetaType::QColor:
        return value.value<QColor>().name(QColor::HexArgb);
    case QMetaType::QPointF: case QMetaType::QPoint: {
        const QPointF p = value.toPointF();
        return QJsonObject{{QStringLiteral("x"), p.x()}, {QStringLiteral("y"), p.y()}};
    }
    default:
        break;
    }
    if (depth < 3 && value.canConvert<QVariantList>() && mt.id() != QMetaType::QString && mt.id() != QMetaType::QByteArray) {
        const QVariantList list = value.toList();
        QJsonArray arr;
        for (int i = 0; i < list.size() && i < kMaxListItems; i++) {
            arr.append(_toJson(list.at(i), depth + 1));
        }
        if (list.size() > kMaxListItems) {
            arr.append(QStringLiteral("... %1 more").arg(list.size() - kMaxListItems));
        }
        return arr;
    }
    if (depth < 3 && value.canConvert<QVariantMap>()) {
        const QVariantMap map = value.toMap();
        QJsonObject o;
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            o.insert(it.key(), _toJson(it.value(), depth + 1));
        }
        return o;
    }
    if (value.canConvert<QString>()) {
        return value.toString();
    }
    return QStringLiteral("<%1>").arg(QString::fromLatin1(mt.name()));
}

bool TarMcpServer::_fromJson(const QJsonValue &json, const QMetaType &type, QVariant &out, QString &error)
{
    if (type.id() == QMetaType::QVariant) {
        const QVariant inner = json.toVariant();
        out = QVariant(type, &inner);
        return true;
    }
    if (type.id() == QMetaType::fromType<QGeoCoordinate>().id()) {
        const QJsonObject o = json.toObject();
        if (!o.contains(QStringLiteral("lat")) || !o.contains(QStringLiteral("lon"))) {
            error = QStringLiteral("a coordinate must be {\"lat\":..,\"lon\":..,\"alt\":..}");
            return false;
        }
        QGeoCoordinate c(o.value(QStringLiteral("lat")).toDouble(), o.value(QStringLiteral("lon")).toDouble());
        if (o.contains(QStringLiteral("alt"))) {
            c.setAltitude(o.value(QStringLiteral("alt")).toDouble());
        }
        out = QVariant::fromValue(c);
        return true;
    }
    if (type.flags() & QMetaType::PointerToQObject) {
        QString resolveError;
        QObject *obj = json.isNull() ? nullptr : _resolvePath(json.toString(), resolveError);
        if (!json.isNull() && !obj) {
            error = resolveError;
            return false;
        }
        if (obj && type.metaObject() && !obj->metaObject()->inherits(type.metaObject())) {
            error = QStringLiteral("%1 is a %2, not a %3").arg(json.toString(), QString::fromLatin1(obj->metaObject()->className()),
                                                               QString::fromLatin1(type.metaObject()->className()));
            return false;
        }
        out = QVariant(type, &obj);
        return true;
    }
    if (type.flags() & QMetaType::IsEnumeration) {
        int v = 0;
        if (json.isString()) {
            bool ok = false;
            v = json.toString().toInt(&ok);
            if (!ok) {
                const QMetaObject *mo = type.metaObject();
                QString name = QString::fromLatin1(type.name());
                name = name.mid(name.lastIndexOf(QLatin1Char(':')) + 1);
                const int idx = mo ? mo->indexOfEnumerator(name.toLatin1().constData()) : -1;
                v = idx >= 0 ? mo->enumerator(idx).keyToValue(json.toString().toLatin1().constData(), &ok) : -1;
                if (!ok) {
                    error = QStringLiteral("'%1' is not a value of %2").arg(json.toString(), QString::fromLatin1(type.name()));
                    return false;
                }
            }
        } else {
            v = json.toInt();
        }
        QVariant iv(v);
        if (!iv.convert(type)) {
            if (type.sizeOf() != static_cast<qsizetype>(sizeof(int))) {
                error = QStringLiteral("cannot convert to %1").arg(QString::fromLatin1(type.name()));
                return false;
            }
            iv = QVariant(type, &v);
        }
        out = iv;
        return true;
    }
    QVariant v = json.toVariant();
    if (!v.convert(type)) {
        error = QStringLiteral("cannot convert %1 to %2").arg(shortText(json), QString::fromLatin1(type.name()));
        return false;
    }
    out = v;
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------------------------

QMap<QString, QObject *> TarMcpServer::_roots() const
{
    QMap<QString, QObject *> r;
    auto add = [&r](const QString &name, QObject *obj) {
        if (obj) {
            r.insert(name, obj);
        }
    };
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    add(QStringLiteral("vehicle"), mvm ? mvm->activeVehicle() : nullptr);
    add(QStringLiteral("multiVehicleManager"), mvm);
    add(QStringLiteral("linkManager"), LinkManager::instance());
    add(QStringLiteral("settingsManager"), SettingsManager::instance());
    add(QStringLiteral("videoManager"), VideoManager::instance());
    add(QStringLiteral("joystickManager"), JoystickManager::instance());
    add(QStringLiteral("adsbVehicleManager"), ADSBVehicleManager::instance());
    add(QStringLiteral("positionManager"), QGCPositionManager::instance());
    add(QStringLiteral("corePlugin"), QGCCorePlugin::instance());
    if (_engine) {
        add(QStringLiteral("QGroundControl"), _engine->singletonInstance<QObject *>("QGroundControl", "QGroundControl"));
        add(QStringLiteral("window"), _engine->rootObjects().value(0));
    }
    return r;
}

QObject *TarMcpServer::_resolvePath(const QString &path, QString &error)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        error = QStringLiteral("empty path");
        return nullptr;
    }
    static const QRegularExpression segRe(QStringLiteral("^([^\\[\\]]+)(?:\\[(\\d+)\\])?$"));
    const QStringList segments = trimmed.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    QObject *obj = nullptr;
    for (int s = 0; s < segments.size(); s++) {
        const QRegularExpressionMatch m = segRe.match(segments.at(s));
        if (!m.hasMatch()) {
            error = QStringLiteral("bad path segment '%1'").arg(segments.at(s));
            return nullptr;
        }
        const QString name = m.captured(1);
        const bool indexed = !m.captured(2).isEmpty();
        const int index = m.captured(2).toInt();
        QObject *next = nullptr;
        QVariant value;

        if (s == 0) {
            if (name.startsWith(QLatin1Char('$'))) {
                next = _handles.value(name.mid(1).toInt()).data();
                if (!next) {
                    error = QStringLiteral("handle %1 no longer exists").arg(name);
                    return nullptr;
                }
            } else if (name.startsWith(QLatin1String("vehicle")) && name.size() > 7 && name.mid(7).toInt() > 0) {
                MultiVehicleManager *mvm = MultiVehicleManager::instance();
                next = mvm ? mvm->getVehicleById(name.mid(7).toInt()) : nullptr;
            } else {
                next = _roots().value(name);
            }
            if (!next && name == QLatin1String("vehicle")) {
                error = QStringLiteral("No vehicle is connected.");
                return nullptr;
            }
            if (!next) {
                error = QStringLiteral("unknown root '%1' (roots: %2, vehicle<sysid>, $<handle>)").arg(name, _roots().keys().join(QStringLiteral(", ")));
                return nullptr;
            }
        } else {
            const int propIdx = obj->metaObject()->indexOfProperty(name.toLatin1().constData());
            if (propIdx >= 0) {
                value = obj->metaObject()->property(propIdx).read(obj);
                next = (value.metaType().flags() & QMetaType::PointerToQObject) ? value.value<QObject *>() : nullptr;
            } else if (auto *v = qobject_cast<Vehicle *>(obj)) {
                if (name == QLatin1String("missionManager")) {
                    next = v->missionManager();
                } else if (name == QLatin1String("geoFenceManager")) {
                    next = v->geoFenceManager();
                } else if (name == QLatin1String("rallyPointManager")) {
                    next = v->rallyPointManager();
                }
            }
            if (!next && propIdx < 0) {
                if (auto *fg = qobject_cast<FactGroup *>(obj)) {
                    next = fg->factExists(name) ? static_cast<QObject *>(fg->getFact(name)) : fg->getFactGroup(name);
                }
            }
            if (!next && propIdx < 0) {
                next = obj->findChild<QObject *>(name, obj == _roots().value(QStringLiteral("window")) ? Qt::FindChildrenRecursively : Qt::FindDirectChildrenOnly);
            }
            if (!next && !(indexed && value.isValid())) {
                error = QStringLiteral("'%1' has no object '%2'").arg(segments.mid(0, s).join(QLatin1Char('.')), name);
                return nullptr;
            }
        }

        if (indexed) {
            QObject *item = nullptr;
            if (auto *list = qobject_cast<QmlObjectListModel *>(next)) {
                if (index >= list->count()) {
                    error = QStringLiteral("%1 has only %2 items").arg(name).arg(list->count());
                    return nullptr;
                }
                item = list->get(index);
            } else if (value.canConvert<QVariantList>()) {
                const QVariantList items = value.toList();
                if (index >= items.size()) {
                    error = QStringLiteral("%1 has only %2 items").arg(name).arg(items.size());
                    return nullptr;
                }
                item = items.at(index).value<QObject *>();
            }
            if (!item) {
                error = QStringLiteral("'%1' is not a list of objects").arg(name);
                return nullptr;
            }
            next = item;
        }
        obj = next;
    }
    return obj;
}

// ---------------------------------------------------------------------------------------------------------
// qgc_inspect / qgc_set_property / qgc_call
// ---------------------------------------------------------------------------------------------------------

QString TarMcpServer::_toolInspect(const QJsonObject &args, bool &isError)
{
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    if (path.isEmpty()) {
        QJsonObject roots;
        const auto r = _roots();
        for (auto it = r.cbegin(); it != r.cend(); ++it) {
            roots.insert(it.key(), QString::fromLatin1(it.value()->metaObject()->className()));
        }
        return jsonText(QJsonObject{{QStringLiteral("roots"), roots},
                                    {QStringLiteral("also"), QStringLiteral("vehicle<sysid> (e.g. vehicle1), $<handle> returned by qgc_call")}});
    }
    QString error;
    QObject *obj = _resolvePath(path, error);
    if (!obj) {
        isError = true;
        return error;
    }
    const QString filter = args.value(QStringLiteral("filter")).toString();
    auto matches = [&filter](const QString &name) { return filter.isEmpty() || name.contains(filter, Qt::CaseInsensitive); };

    const QMetaObject *mo = obj->metaObject();
    QJsonObject out{{QStringLiteral("path"), path}, {QStringLiteral("class"), QString::fromLatin1(mo->className())}};
    if (!obj->objectName().isEmpty()) {
        out.insert(QStringLiteral("objectName"), obj->objectName());
    }

    QJsonObject props;
    QJsonArray writable;
    for (int i = 1; i < mo->propertyCount(); i++) { // 0 is objectName
        const QMetaProperty p = mo->property(i);
        const QString name = QString::fromLatin1(p.name());
        if (!matches(name)) {
            continue;
        }
        props.insert(name, _toJson(p.read(obj)));
        if (p.isWritable() && !p.isConstant()) {
            writable.append(QStringLiteral("%1 (%2)").arg(name, QString::fromLatin1(p.typeName())));
        }
    }
    out.insert(QStringLiteral("properties"), props);
    if (!writable.isEmpty()) {
        out.insert(QStringLiteral("writable"), writable);
    }

    QJsonArray methods;
    QSet<QString> seen;
    for (int i = QObject::staticMetaObject.methodCount(); i < mo->methodCount(); i++) {
        const QMetaMethod m = mo->method(i);
        if (m.access() != QMetaMethod::Public || (m.methodType() != QMetaMethod::Method && m.methodType() != QMetaMethod::Slot)) {
            continue;
        }
        const QString name = QString::fromLatin1(m.name());
        if (name.startsWith(QLatin1Char('_')) || !matches(name)) {
            continue;
        }
        QStringList params;
        for (int a = 0; a < m.parameterCount(); a++) {
            params << QStringLiteral("%1 %2").arg(QString::fromLatin1(m.parameterTypeName(a)), QString::fromLatin1(m.parameterNames().value(a)));
        }
        const QString sig = QStringLiteral("%1 %2(%3)").arg(QString::fromLatin1(m.typeName()), name, params.join(QStringLiteral(", ")));
        if (!seen.contains(sig)) {
            seen.insert(sig);
            methods.append(sig);
        }
    }
    if (!methods.isEmpty()) {
        out.insert(QStringLiteral("methods"), methods);
    }

    if (auto *list = qobject_cast<QmlObjectListModel *>(obj)) {
        QJsonArray items;
        for (int i = 0; i < list->count() && i < kMaxListItems; i++) {
            QObject *item = list->get(i);
            QString label = item ? QString::fromLatin1(item->metaObject()->className()) : QStringLiteral("null");
            if (item && item->property("name").isValid()) {
                label += QStringLiteral(" \"%1\"").arg(item->property("name").toString());
            }
            items.append(label);
        }
        out.insert(QStringLiteral("items (use path[i])"), items);
    }
    if (auto *fg = qobject_cast<FactGroup *>(obj)) {
        QJsonObject facts;
        for (const QString &n : fg->factNames()) {
            if (matches(n)) {
                facts.insert(n, _toJson(QVariant::fromValue(static_cast<QObject *>(fg->getFact(n)))));
            }
        }
        out.insert(QStringLiteral("facts (path.<name>; set rawValue)"), facts);
        out.insert(QStringLiteral("factGroups"), QJsonArray::fromStringList(fg->factGroupNames()));
    }
    if (auto *v = qobject_cast<Vehicle *>(obj)) {
        Q_UNUSED(v)
        out.insert(QStringLiteral("also"), QStringLiteral("missionManager, geoFenceManager, rallyPointManager; vehicle parameters: use the parameter tools"));
    }
    QJsonArray named;
    if (obj != _roots().value(QStringLiteral("window"))) {
        for (QObject *child : obj->children()) {
            if (!child->objectName().isEmpty() && matches(child->objectName())) {
                named.append(QStringLiteral("%1 (%2)").arg(child->objectName(), QString::fromLatin1(child->metaObject()->className())));
            }
        }
    }
    if (!named.isEmpty()) {
        out.insert(QStringLiteral("namedChildren"), named);
    }
    QString text = jsonText(out);
    if (text.size() > 60000) {
        text = text.left(60000) + QStringLiteral("\n... (truncated; use filter)");
    }
    return text;
}

QStringList TarMcpServer::_vehicleWarnings(QObject *target) const
{
    QStringList warnings;
    for (QObject *o = target; o; o = o->parent()) {
        if (auto *v = qobject_cast<Vehicle *>(o)) {
            warnings << QStringLiteral("This acts on vehicle %1.").arg(v->id());
            if (v->armed()) {
                warnings << QStringLiteral("Vehicle is ARMED.");
            }
            if (v->flying()) {
                warnings << QStringLiteral("Vehicle is FLYING.");
            }
            break;
        }
    }
    return warnings;
}

void TarMcpServer::_toolSetProperty(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const QString propName = args.value(QStringLiteral("property")).toString().trimmed();
    QString error;
    QObject *obj = _resolvePath(path, error);
    if (!obj) {
        _sendToolResult(socket, id, error, true);
        return;
    }
    const int idx = obj->metaObject()->indexOfProperty(propName.toLatin1().constData());
    if (idx < 0) {
        _sendToolResult(socket, id, QStringLiteral("%1 has no property '%2'. Use qgc_inspect.").arg(path, propName), true);
        return;
    }
    const QMetaProperty prop = obj->metaObject()->property(idx);
    if (!prop.isWritable()) {
        _sendToolResult(socket, id, QStringLiteral("%1.%2 is read-only. Look for a method instead.").arg(path, propName), true);
        return;
    }
    QVariant value;
    if (!_fromJson(args.value(QStringLiteral("value")), prop.metaType(), value, error)) {
        _sendToolResult(socket, id, QStringLiteral("%1.%2: %3").arg(path, propName, error), true);
        return;
    }
    if (prop.metaType().id() == QMetaType::QVariant) {
        value = value.value<QVariant>(); // property writes take the value itself, not a wrapped QVariant
    }
    // Facts: validate against their metadata and write the typed raw value
    if (auto *fact = qobject_cast<Fact *>(obj); fact && propName == QLatin1String("rawValue")) {
        QVariant typed;
        QString problem;
        if (!_validateChange(fact, path, args.value(QStringLiteral("value")).toVariant(), typed, problem)) {
            _sendToolResult(socket, id, problem, true);
            return;
        }
        value = typed;
    }
    const QString oldText = shortText(_toJson(prop.read(obj)));
    const QString newText = shortText(_toJson(value));
    if (oldText == newText) {
        _sendToolResult(socket, id, QStringLiteral("Nothing to change: %1.%2 is already %3.").arg(path, propName, oldText));
        return;
    }

    PendingApproval pending;
    pending.socket = socket;
    pending.rpcId = id;
    pending.lines << QStringLiteral("%1.%2:  %3 → %4").arg(path, propName, oldText.isEmpty() ? QStringLiteral("(empty)") : oldText, newText);
    QPointer<QObject> guard(obj);
    pending.apply = [this, guard, path, propName, value](bool &ok) -> QString {
        if (!guard) {
            ok = false;
            return QStringLiteral("%1 no longer exists; nothing was changed.").arg(path);
        }
        const QMetaProperty p = guard->metaObject()->property(guard->metaObject()->indexOfProperty(propName.toLatin1().constData()));
        ok = p.write(guard, value);
        return ok ? QStringLiteral("Done. %1.%2 is now %3").arg(path, propName, shortText(_toJson(p.read(guard))))
                  : QStringLiteral("QGC refused to set %1.%2.").arg(path, propName);
    };
    _requestApproval(pending, QStringLiteral("Change a QGC property"), args.value(QStringLiteral("reason")).toString().trimmed(), _vehicleWarnings(obj));
}

QString TarMcpServer::_invoke(QObject *obj, const QString &methodName, const QJsonArray &jsonArgs, bool &ok)
{
    ok = false;
    const QMetaObject *mo = obj->metaObject();
    QString lastError = QStringLiteral("%1 has no method %2 taking %3 argument(s). Use qgc_inspect.")
                            .arg(QString::fromLatin1(mo->className()), methodName).arg(jsonArgs.size());
    for (int i = mo->methodCount() - 1; i >= 0; i--) {
        const QMetaMethod m = mo->method(i);
        if (QString::fromLatin1(m.name()) != methodName || m.parameterCount() != jsonArgs.size() || m.access() != QMetaMethod::Public
            || (m.methodType() != QMetaMethod::Method && m.methodType() != QMetaMethod::Slot)) {
            continue;
        }
        QList<QVariant> values;
        bool converted = true;
        for (int a = 0; a < m.parameterCount(); a++) {
            QVariant v;
            QString err;
            if (!_fromJson(jsonArgs.at(a), m.parameterMetaType(a), v, err)) {
                lastError = QStringLiteral("argument %1 (%2): %3").arg(a + 1).arg(QString::fromLatin1(m.parameterTypeName(a)), err);
                converted = false;
                break;
            }
            values << v;
        }
        if (!converted) {
            continue;
        }
        const QMetaType rt = m.returnMetaType();
        const bool hasReturn = rt.isValid() && rt.id() != QMetaType::Void;
        QVariant ret = hasReturn ? QVariant(rt) : QVariant();
        void *argv[11] = {hasReturn ? ret.data() : nullptr};
        for (int a = 0; a < values.size(); a++) {
            argv[a + 1] = values[a].data();
        }
        QMetaObject::metacall(obj, QMetaObject::InvokeMetaMethod, m.methodIndex(), argv);
        ok = true;
        if (!hasReturn) {
            return QStringLiteral("Done.");
        }
        if ((rt.flags() & QMetaType::PointerToQObject) && ret.value<QObject *>()) {
            const int handle = _nextHandle++;
            _handles.insert(handle, ret.value<QObject *>());
            return QStringLiteral("Returned %1, available as path $%2").arg(QString::fromLatin1(ret.value<QObject *>()->metaObject()->className())).arg(handle);
        }
        return QStringLiteral("Returned: ") + jsonText(_toJson(ret));
    }
    return lastError;
}

void TarMcpServer::_toolCall(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args)
{
    const QString path = args.value(QStringLiteral("path")).toString().trimmed();
    const QString method = args.value(QStringLiteral("method")).toString().trimmed();
    const QJsonArray callArgs = args.value(QStringLiteral("args")).toArray();
    if (callArgs.size() > 10) {
        _sendToolResult(socket, id, QStringLiteral("At most 10 arguments."), true);
        return;
    }
    QString error;
    QObject *obj = _resolvePath(path, error);
    if (!obj) {
        _sendToolResult(socket, id, error, true);
        return;
    }
    bool exists = false;
    for (int i = 0; i < obj->metaObject()->methodCount() && !exists; i++) {
        const QMetaMethod m = obj->metaObject()->method(i);
        exists = QString::fromLatin1(m.name()) == method && m.parameterCount() == callArgs.size();
    }
    if (!exists) {
        _sendToolResult(socket, id, QStringLiteral("%1 has no method %2 taking %3 argument(s). Use qgc_inspect.").arg(path, method).arg(callArgs.size()), true);
        return;
    }
    if (isReadOnlyMethodName(method)) {
        bool ok = false;
        const QString text = _invoke(obj, method, callArgs, ok);
        _sendToolResult(socket, id, text, !ok);
        return;
    }
    QStringList shown;
    for (const QJsonValue &v : callArgs) {
        shown << shortText(v);
    }
    PendingApproval pending;
    pending.socket = socket;
    pending.rpcId = id;
    pending.lines << QStringLiteral("%1.%2(%3)").arg(path, method, shown.join(QStringLiteral(", ")));
    QPointer<QObject> guard(obj);
    pending.apply = [this, guard, path, method, callArgs](bool &ok) -> QString {
        if (!guard) {
            ok = false;
            return QStringLiteral("%1 no longer exists; nothing was done.").arg(path);
        }
        return _invoke(guard, method, callArgs, ok);
    };
    _requestApproval(pending, QStringLiteral("Run a QGC action"), args.value(QStringLiteral("reason")).toString().trimmed(), _vehicleWarnings(obj));
}

// ---------------------------------------------------------------------------------------------------------
// Comm Links
// ---------------------------------------------------------------------------------------------------------

namespace {

QList<LinkConfiguration *> linkConfigs()
{
    QList<LinkConfiguration *> out;
    LinkManager *lm = LinkManager::instance();
    auto *list = lm ? lm->property("linkConfigurations").value<QmlObjectListModel *>() : nullptr;
    for (int i = 0; list && i < list->count(); i++) {
        if (auto *c = qobject_cast<LinkConfiguration *>(list->get(i))) {
            out << c;
        }
    }
    return out;
}

LinkConfiguration *findLink(const QString &name)
{
    for (LinkConfiguration *c : linkConfigs()) {
        if (c->name().compare(name, Qt::CaseInsensitive) == 0) {
            return c;
        }
    }
    return nullptr;
}

const QStringList kSkipLinkProps = {QStringLiteral("objectName"), QStringLiteral("name"), QStringLiteral("link"), QStringLiteral("linkType"),
                                    QStringLiteral("settingsURL"), QStringLiteral("settingsTitle"), QStringLiteral("linkActive"), QStringLiteral("dynamic")};

QString typeName(const LinkConfiguration *c)
{
    const QMetaEnum e = QMetaEnum::fromType<LinkConfiguration::LinkType>();
    QString n = QString::fromLatin1(e.valueToKey(c->type()));
    return n.startsWith(QLatin1String("Type")) ? n.mid(4).toLower() : n.toLower();
}

} // namespace

QString TarMcpServer::_toolListCommLinks()
{
    QJsonArray links;
    for (LinkConfiguration *c : linkConfigs()) {
        QJsonObject o{
            {QStringLiteral("name"), c->name()},
            {QStringLiteral("type"), typeName(c)},
            {QStringLiteral("connected"), c->linkActive()},
        };
        if (c->isDynamic()) {
            o.insert(QStringLiteral("dynamic"), true);
        }
        QJsonObject settings;
        const QMetaObject *mo = c->metaObject();
        for (int i = 0; i < mo->propertyCount(); i++) {
            const QMetaProperty p = mo->property(i);
            if (!kSkipLinkProps.contains(QString::fromLatin1(p.name()))) {
                settings.insert(QString::fromLatin1(p.name()), _toJson(p.read(c)));
            }
        }
        o.insert(QStringLiteral("settings"), settings);
        links.append(o);
    }
    QJsonObject out{{QStringLiteral("links"), links}};
    QJsonArray types;
    const QMetaEnum e = QMetaEnum::fromType<LinkConfiguration::LinkType>();
    for (int i = 0; i < e.keyCount(); i++) {
        const QString k = QString::fromLatin1(e.key(i));
        if (k != QLatin1String("TypeLast") && k != QLatin1String("TypeLogReplay") && k != QLatin1String("TypeMock")) {
            types.append(k.mid(4).toLower());
        }
    }
    out.insert(QStringLiteral("types"), types);
#ifndef QGC_NO_SERIAL_LINK
    QJsonArray ports;
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        ports.append(QStringLiteral("%1 (%2)").arg(info.portName(), info.description()));
    }
    out.insert(QStringLiteral("serial_ports"), ports);
#endif
    out.insert(QStringLiteral("settings_help"),
               QStringLiteral("udp: localPort, hostList (targets as \"ip:port\"); tcp: host, port; serial: portName, baud, dataBits, "
                              "parity, stopBits, flowControl; all: autoConnect, highLatency"));
    return jsonText(out);
}

bool TarMcpServer::_applyLinkSettings(LinkConfiguration *config, const QJsonObject &settings, QString &error)
{
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        const QString key = it.key();
        if (key == QLatin1String("hostList") || key == QLatin1String("hosts")) {
            const QStringList existing = config->property("hostList").toStringList();
            for (const QString &h : existing) {
                (void) QMetaObject::invokeMethod(config, "removeHost", Q_ARG(QString, h));
            }
            for (const QJsonValue &h : it.value().toArray()) {
                if (!h.toString().contains(QLatin1Char(':'))) {
                    error = QStringLiteral("hostList entries must be \"ip:port\" (got %1)").arg(h.toString());
                    return false;
                }
                (void) QMetaObject::invokeMethod(config, "addHost", Q_ARG(QString, h.toString()));
            }
            continue;
        }
        const int idx = config->metaObject()->indexOfProperty(key.toLatin1().constData());
        if (idx < 0 || kSkipLinkProps.contains(key)) {
            error = QStringLiteral("%1 links have no setting '%2'").arg(typeName(config), key);
            return false;
        }
        const QMetaProperty p = config->metaObject()->property(idx);
        QVariant v;
        if (!_fromJson(it.value(), p.metaType(), v, error) || !p.write(config, v)) {
            error = QStringLiteral("%1: %2").arg(key, error.isEmpty() ? QStringLiteral("invalid value") : error);
            return false;
        }
    }
    return true;
}

void TarMcpServer::_toolSetCommLink(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args)
{
    LinkManager *lm = LinkManager::instance();
    const QString name = args.value(QStringLiteral("name")).toString().trimmed();
    const QJsonObject settings = args.value(QStringLiteral("settings")).toObject();
    const bool connectAfter = args.value(QStringLiteral("connect")).toBool(false);
    if (!lm || name.isEmpty()) {
        _sendToolResult(socket, id, QStringLiteral("A link name is required."), true);
        return;
    }
    LinkConfiguration *existing = findLink(name);
    int type = -1;
    if (!existing) {
        const QString t = args.value(QStringLiteral("type")).toString().trimmed().toLower();
        const QMetaEnum e = QMetaEnum::fromType<LinkConfiguration::LinkType>();
        for (int i = 0; i < e.keyCount(); i++) {
            if (QString::fromLatin1(e.key(i)).mid(4).toLower() == t) {
                type = e.value(i);
            }
        }
        if (type < 0 || t == QLatin1String("last") || t == QLatin1String("logreplay")) {
            _sendToolResult(socket, id, QStringLiteral("No link named '%1' exists. To create it, give type: udp, tcp, serial or bluetooth.").arg(name), true);
            return;
        }
    } else if (existing->isDynamic()) {
        _sendToolResult(socket, id, QStringLiteral("'%1' is a temporary auto-connect link and cannot be edited.").arg(name), true);
        return;
    }

    // Validate on a scratch copy so mistakes are reported before the approval card.
    LinkConfiguration *scratch = existing ? LinkConfiguration::duplicateSettings(existing) : LinkConfiguration::createSettings(type, name);
    QString error;
    const bool valid = scratch && _applyLinkSettings(scratch, settings, error);
    QStringList lines;
    if (valid) {
        const QMetaObject *mo = scratch->metaObject();
        for (int i = 0; i < mo->propertyCount(); i++) {
            const QMetaProperty p = mo->property(i);
            if (kSkipLinkProps.contains(QString::fromLatin1(p.name()))) {
                continue;
            }
            const QString after = shortText(_toJson(p.read(scratch)));
            if (existing) {
                const QString before = shortText(_toJson(p.read(existing)));
                if (before != after) {
                    lines << QStringLiteral("%1:  %2 → %3").arg(QString::fromLatin1(p.name()), before.isEmpty() ? QStringLiteral("(empty)") : before, after);
                }
            } else {
                lines << QStringLiteral("%1:  %2").arg(QString::fromLatin1(p.name()), after);
            }
        }
    }
    delete scratch;
    if (!valid) {
        _sendToolResult(socket, id, QStringLiteral("Nothing was changed: ") + (error.isEmpty() ? QStringLiteral("could not create the link") : error), true);
        return;
    }
    if (existing && lines.isEmpty() && !(connectAfter && !existing->linkActive())) {
        _sendToolResult(socket, id, QStringLiteral("Nothing to change: '%1' already has those settings.").arg(name));
        return;
    }
    if (connectAfter) {
        lines << QStringLiteral("then connect");
    }
    QStringList warnings;
    if (existing && existing->linkActive() && !lines.isEmpty()) {
        warnings << QStringLiteral("The link is connected; it will be disconnected and reconnected.");
    }

    PendingApproval pending;
    pending.socket = socket;
    pending.rpcId = id;
    pending.lines = lines;
    pending.apply = [this, name, type, settings, connectAfter](bool &ok) -> QString {
        ok = false;
        LinkManager *lm = LinkManager::instance();
        if (!lm) {
            return QStringLiteral("LinkManager is gone.");
        }
        QString err;
        LinkConfiguration *config = findLink(name);
        bool reconnect = connectAfter;
        if (config) {
            reconnect |= config->linkActive();
            if (config->linkActive()) {
                lm->disconnectLinkConfiguration(config);
            }
            LinkConfiguration *edited = lm->startConfigurationEditing(config);
            if (!_applyLinkSettings(edited, settings, err)) {
                lm->cancelConfigurationEditing(edited);
                return QStringLiteral("Nothing was changed: ") + err;
            }
            lm->endConfigurationEditing(config, edited);
        } else {
            config = lm->createConfiguration(type, name);
            if (!config || !_applyLinkSettings(config, settings, err)) {
                delete config;
                return QStringLiteral("Nothing was created: ") + err;
            }
            lm->endCreateConfiguration(config);
            config = findLink(name);
        }
        if (reconnect && config) {
            lm->createConnectedLink(config);
        }
        ok = true;
        return QStringLiteral("Done. Link '%1' saved%2.\n").arg(name, reconnect ? QStringLiteral(" and connecting") : QString()) + _toolListCommLinks();
    };
    _requestApproval(pending, existing ? QStringLiteral("Edit Comm Link \"%1\"").arg(name) : QStringLiteral("Create %1 Comm Link \"%2\"").arg(args.value(QStringLiteral("type")).toString().toUpper(), name),
                     args.value(QStringLiteral("reason")).toString().trimmed(), warnings);
}

void TarMcpServer::_toolCommLinkAction(QTcpSocket *socket, const QJsonValue &id, const QJsonObject &args)
{
    const QString name = args.value(QStringLiteral("name")).toString().trimmed();
    const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
    LinkConfiguration *config = findLink(name);
    if (!config) {
        _sendToolResult(socket, id, QStringLiteral("No link named '%1'. Use list_comm_links.").arg(name), true);
        return;
    }
    if (action != QLatin1String("connect") && action != QLatin1String("disconnect") && action != QLatin1String("delete")) {
        _sendToolResult(socket, id, QStringLiteral("action must be connect, disconnect or delete."), true);
        return;
    }
    if (action == QLatin1String("connect") && config->linkActive()) {
        _sendToolResult(socket, id, QStringLiteral("'%1' is already connected.").arg(name));
        return;
    }
    if (action == QLatin1String("disconnect") && !config->linkActive()) {
        _sendToolResult(socket, id, QStringLiteral("'%1' is not connected.").arg(name));
        return;
    }
    QStringList warnings;
    if (action != QLatin1String("connect") && config->linkActive()) {
        MultiVehicleManager *mvm = MultiVehicleManager::instance();
        const QmlObjectListModel *vehicles = mvm ? mvm->vehicles() : nullptr;
        for (int i = 0; vehicles && i < vehicles->count(); i++) {
            Vehicle *v = vehicles->value<Vehicle *>(i);
            if (v && v->armed()) {
                warnings << QStringLiteral("Vehicle %1 is ARMED; this may cut its link.").arg(v->id());
            }
        }
    }
    PendingApproval pending;
    pending.socket = socket;
    pending.rpcId = id;
    pending.lines << QStringLiteral("%1 \"%2\" (%3)").arg(action.at(0).toUpper() + action.mid(1), name, typeName(config));
    pending.apply = [name, action](bool &ok) -> QString {
        ok = false;
        LinkManager *lm = LinkManager::instance();
        LinkConfiguration *c = findLink(name);
        if (!lm || !c) {
            return QStringLiteral("Link '%1' no longer exists.").arg(name);
        }
        if (action == QLatin1String("connect")) {
            lm->createConnectedLink(c);
        } else if (action == QLatin1String("disconnect")) {
            lm->disconnectLinkConfiguration(c);
        } else {
            lm->removeConfiguration(c);
        }
        ok = true;
        return QStringLiteral("Done: %1 '%2'.").arg(action, name);
    };
    _requestApproval(pending, QStringLiteral("Comm Link"), args.value(QStringLiteral("reason")).toString().trimmed(), warnings);
}
