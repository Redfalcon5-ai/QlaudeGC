#include "TarBagRecorder.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QThread>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

#include <memory>

#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(TarBagRecorderLog, "Custom.TarBagRecorder")

namespace {

constexpr const char *kGroup = "TarBagRecorder";
constexpr int kPollMs = 5000;
constexpr int kMaxManifestFiles = 100000;

const QStringList kStates = {
    QStringLiteral("idle"), QStringLiteral("starting"), QStringLiteral("recording"),
    QStringLiteral("stopping"), QStringLiteral("finalized"), QStringLiteral("failed"),
};

bool matches(const QString &pattern, const QString &text)
{
    return QRegularExpression(QStringLiteral("^(?:%1)$").arg(pattern)).match(text).hasMatch();
}

QString formatBytes(double b)
{
    if (b >= 1073741824.0) return QStringLiteral("%1 GB").arg(b / 1073741824.0, 0, 'f', 2);
    if (b >= 1048576.0) return QStringLiteral("%1 MB").arg(b / 1048576.0, 0, 'f', 1);
    return QStringLiteral("%1 kB").arg(b / 1024.0, 0, 'f', 0);
}

QString shortText(const QByteArray &data, int max = 500)
{
    QString s = QString::fromUtf8(data).trimmed();
    if (s.size() > max) s = s.left(max) + QStringLiteral("...");
    return s;
}

} // namespace

// ------------------------------------------------------------------------------------------------ validation
bool TarBagRecorder::isValidSessionId(const QString &id)
{
    return matches(QStringLiteral("[A-Za-z0-9_-]{1,96}"), id);
}

bool TarBagRecorder::isValidRemotePath(const QString &path, const QString &sessionId)
{
    // Absolute, safe characters only, no "..", and the directory must be the session's own "qgc-<id>" folder.
    if (!matches(QStringLiteral("/[A-Za-z0-9_./-]+"), path) || path.contains(QStringLiteral(".."))) return false;
    return isValidSessionId(sessionId) && path.endsWith(QStringLiteral("/qgc-") + sessionId);
}

bool TarBagRecorder::isValidManifestPath(const QString &rel)
{
    if (rel.isEmpty() || rel.size() > 512) return false;
    if (rel.startsWith(QLatin1Char('/')) || rel.contains(QLatin1Char('\\')) || rel.contains(QLatin1Char(':'))) return false;
    for (const QString &part : rel.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) return false;
    }
    return true;
}

QString TarBagRecorder::classifySshFailure(int exitCode, const QString &err, QProcess::ProcessError procError, bool timedOut)
{
    const QString e = err.toLower();
    if (procError == QProcess::FailedToStart) {
        return tr("ssh.exe / scp.exe was not found. Install the Windows 'OpenSSH Client' optional feature.");
    }
    if (timedOut) {
        return tr("The Jetson did not answer in time. The recording on the Jetson is not affected. Check that the Jetson/Tailscale is reachable and try Refresh.");
    }
    if (e.contains(QStringLiteral("host key verification failed")) || e.contains(QStringLiteral("remote host identification has changed"))) {
        return tr("SSH host key verification failed. Connect once manually (ssh <user>@<host>) and verify/accept the host key; if the Jetson was re-flashed, remove the old key from known_hosts.");
    }
    if (e.contains(QStringLiteral("permission denied")) || e.contains(QStringLiteral("no supported authentication"))) {
        return tr("SSH authentication failed. Check the user name and that your key is in the Jetson's ~/.ssh/authorized_keys (password logins are disabled for this feature).");
    }
    if (e.contains(QStringLiteral("could not resolve")) || e.contains(QStringLiteral("timed out")) || e.contains(QStringLiteral("no route"))
        || e.contains(QStringLiteral("connection refused")) || e.contains(QStringLiteral("unreachable")) || e.contains(QStringLiteral("connection reset"))
        || e.contains(QStringLiteral("connection closed")) || e.contains(QStringLiteral("broken pipe"))) {
        return tr("Cannot reach the Jetson over SSH (%1). Check Tailscale/Wi-Fi and the host name. Recording on the Jetson is unaffected.").arg(err.trimmed().left(160));
    }
    if (exitCode == 127 || e.contains(QStringLiteral("command not found")) || e.contains(QStringLiteral("no such file"))) {
        return tr("qgc-bagctl is not installed on the Jetson (or the 'Remote command' path is wrong). See jetson/SETUP.md.");
    }
    return tr("SSH failed (exit %1): %2").arg(exitCode).arg(err.trimmed().left(300));
}

// ------------------------------------------------------------------------------------------------ lifecycle
TarBagRecorder::TarBagRecorder(QObject *parent)
    : QObject(parent)
{
    _loadSettings();
    _jobTimer.setSingleShot(true);
    connect(&_jobTimer, &QTimer::timeout, this, [this]() {
        if (_sshProcess) {
            _jobTimedOut = true;
            _sshProcess->kill();   // kills only the local ssh client; the Jetson's systemd job is independent
        }
    });
    _pollTimer.setInterval(kPollMs);
    connect(&_pollTimer, &QTimer::timeout, this, [this]() {
        if (configured() && !_jobActive && _queue.isEmpty() && _transferState != QStringLiteral("downloading")) {
            refreshStatus();
        }
    });
    _progressTimer.setInterval(1000);
    connect(&_progressTimer, &QTimer::timeout, this, &TarBagRecorder::_scanProgress);
}

void TarBagRecorder::init()
{
    _pollTimer.start();
    if (configured()) refreshStatus();   // rediscover a recording that is already running
}

void TarBagRecorder::shutdown()
{
    _pollTimer.stop();
    _progressTimer.stop();
    _queue.clear();
    for (QProcess *p : {_sshProcess, _scpProcess}) {
        if (p) {
            p->disconnect(this);
            p->kill();
            p->waitForFinished(1000);
        }
    }
}

void TarBagRecorder::_loadSettings()
{
    QSettings s;
    s.beginGroup(QLatin1String(kGroup));
    _host = s.value(QStringLiteral("host")).toString();
    _sshUser = s.value(QStringLiteral("sshUser")).toString();
    _sshIdentityFile = s.value(QStringLiteral("sshIdentityFile")).toString();
    _remoteCommand = s.value(QStringLiteral("remoteCommand"), _remoteCommand).toString();
    _excludeRegex = s.value(QStringLiteral("excludeRegex")).toString();
    _vehicleLabel = s.value(QStringLiteral("vehicleLabel")).toString();
    _verifyChecksums = s.value(QStringLiteral("verifyChecksums"), false).toBool();
    _localDownloadFolder = s.value(QStringLiteral("localDownloadFolder")).toString();
    if (_localDownloadFolder.isEmpty()) {
        _localDownloadFolder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/QlaudeGC-bags");
    }
}

void TarBagRecorder::_save(const QString &key, const QVariant &value)
{
    QSettings().setValue(QStringLiteral("%1/%2").arg(QLatin1String(kGroup), key), value);
    emit settingsChanged();
}

#define TAR_SETTER(Name, member, key) \
    void TarBagRecorder::set##Name(const QString &v) { \
        const QString t = v.trimmed(); \
        if (t != member) { member = t; _save(QStringLiteral(key), t); } \
    }

TAR_SETTER(Host, _host, "host")
TAR_SETTER(SshUser, _sshUser, "sshUser")
TAR_SETTER(SshIdentityFile, _sshIdentityFile, "sshIdentityFile")
TAR_SETTER(RemoteCommand, _remoteCommand, "remoteCommand")
TAR_SETTER(VehicleLabel, _vehicleLabel, "vehicleLabel")
TAR_SETTER(LocalDownloadFolder, _localDownloadFolder, "localDownloadFolder")

void TarBagRecorder::setExcludeRegex(const QString &v)
{
    // Not trimmed: the regex is used literally.
    if (v != _excludeRegex) {
        _excludeRegex = v;
        _save(QStringLiteral("excludeRegex"), v);
    }
}

void TarBagRecorder::setVerifyChecksums(bool v)
{
    if (v != _verifyChecksums) {
        _verifyChecksums = v;
        _save(QStringLiteral("verifyChecksums"), v);
    }
}

bool TarBagRecorder::configured() const
{
    return !_host.isEmpty();
}

QString TarBagRecorder::settingsProblem() const
{
    if (_host.isEmpty()) return tr("Set the Jetson host (Tailscale name/IP or LAN IP) first.");
    if (!matches(QStringLiteral("[A-Za-z0-9_.:\\[\\]][A-Za-z0-9_.:\\[\\]-]*"), _host)) return tr("The Jetson host contains invalid characters.");
    if (!_sshUser.isEmpty() && !matches(QStringLiteral("[A-Za-z_][A-Za-z0-9_.-]*"), _sshUser)) return tr("The SSH user name is invalid.");
    if (!_sshIdentityFile.isEmpty() && !QFileInfo::exists(_sshIdentityFile)) return tr("SSH key file not found: %1").arg(_sshIdentityFile);
    if (!matches(QStringLiteral("[A-Za-z0-9_./~-]+"), _remoteCommand)) return tr("The remote command may only contain letters, digits and _ . / ~ -");
    if (!matches(QStringLiteral("[A-Za-z0-9_-]{0,32}"), _vehicleLabel)) return tr("Vehicle label may only contain letters, digits, _ and -");
    if (_excludeRegex.size() > 2000 || _excludeRegex.contains(QLatin1Char('\n')) || _excludeRegex.contains(QLatin1Char('\r'))) {
        return tr("The exclude regex must be a single line of at most 2000 characters.");
    }
    if (_findTool(QStringLiteral("ssh")).isEmpty()) return classifySshFailure(0, {}, QProcess::FailedToStart, false);
    return {};
}

QString TarBagRecorder::commandPreview() const
{
    QString ros = QStringLiteral("ros2 bag record -a");
    if (!_excludeRegex.isEmpty()) ros += QStringLiteral(" -x '%1'").arg(_excludeRegex);
    ros += QStringLiteral(" -o <bag_root>/qgc-<session id>");
    return tr("Jetson runs: %1 start --json   (options on stdin)\nwhich executes: %2").arg(_remoteCommand, ros);
}

// ------------------------------------------------------------------------------------------------ state helpers
QString TarBagRecorder::statusText() const
{
    if (_transferState == QStringLiteral("downloading") || _transferState == QStringLiteral("preparing")) return tr("Downloading bag");
    if (_transferState == QStringLiteral("verifying")) return tr("Verifying download");
    if (_state == QStringLiteral("unknown")) return configured() ? tr("Jetson offline / unknown") : tr("Bag recording not configured");
    if (_state == QStringLiteral("idle")) return tr("Not recording");
    if (_state == QStringLiteral("starting")) return tr("Starting...");
    if (_state == QStringLiteral("recording")) return tr("Recording bag");
    if (_state == QStringLiteral("stopping")) return tr("Stopping / finalizing...");
    if (_state == QStringLiteral("finalized")) return _transferState == QStringLiteral("completed") ? tr("Bag downloaded") : tr("Bag stopped and finalized");
    return tr("Recording failed");
}

void TarBagRecorder::_setState(const QString &s)
{
    if (s != _state) {
        _state = s;
    }
    emit stateChanged();
}

void TarBagRecorder::_setError(const QString &e)
{
    if (e != _lastError) {
        _lastError = e;
        emit lastErrorChanged();
    }
}

void TarBagRecorder::_setTransfer(const QString &s, const QString &detail)
{
    _transferState = s;
    _transferDetail = detail;
    emit transferChanged();
    emit stateChanged();
}

void TarBagRecorder::_setBusyNotify()
{
    emit busyChanged();
}

// ------------------------------------------------------------------------------------------------ ssh plumbing
QString TarBagRecorder::_findTool(const QString &name) const
{
    QString p = QStandardPaths::findExecutable(name);
#ifdef Q_OS_WIN
    if (p.isEmpty()) {
        const QString sys = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
        const QString cand = sys + QStringLiteral("\\System32\\OpenSSH\\") + name + QStringLiteral(".exe");
        if (QFileInfo::exists(cand)) p = cand;
    }
#endif
    return p;
}

QString TarBagRecorder::_target() const
{
    return _sshUser.isEmpty() ? _host : (_sshUser + QLatin1Char('@') + _host);
}

QStringList TarBagRecorder::_sshCommonOptions() const
{
    QStringList o = {
        QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),                 // never prompt for passwords
        QStringLiteral("-o"), QStringLiteral("ConnectTimeout=5"),
        QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=yes"),     // never auto-accept host keys
        QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=10"),
        QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=3"),
    };
    if (!_sshIdentityFile.isEmpty()) {
        o << QStringLiteral("-i") << _sshIdentityFile << QStringLiteral("-o") << QStringLiteral("IdentitiesOnly=yes");
    }
    return o;
}

void TarBagRecorder::_enqueue(const Job &job)
{
    const QString problem = settingsProblem();
    if (!problem.isEmpty()) {
        _setError(problem);
        return;
    }
    // Serial and idempotent: collapse duplicate queued polls.
    if (job.kind == QStringLiteral("status")) {
        for (const Job &j : std::as_const(_queue)) if (j.kind == job.kind) return;
    }
    _queue.append(job);
    _setBusyNotify();
    _runNext();
}

void TarBagRecorder::_runNext()
{
    if (_jobActive || _queue.isEmpty()) return;
    _job = _queue.takeFirst();
    _jobActive = true;
    _jobTimedOut = false;

    QStringList args = {QStringLiteral("-T")};
    args += _sshCommonOptions();
    args << _target() << _remoteCommand;
    args += _job.remoteArgs;

    _sshProcess = new QProcess(this);
    QProcess *proc = _sshProcess;
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus) {
        if (proc != _sshProcess) return;
        const QByteArray out = proc->readAllStandardOutput();
        const QByteArray err = proc->readAllStandardError();
        _jobTimer.stop();
        _sshProcess = nullptr;
        proc->deleteLater();
        _jobFinished(code, out, err, _jobTimedOut ? QProcess::Timedout : QProcess::UnknownError);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || proc != _sshProcess) return;
        _jobTimer.stop();
        _sshProcess = nullptr;
        proc->deleteLater();
        _jobFinished(-1, {}, {}, QProcess::FailedToStart);
    });

    qCDebug(TarBagRecorderLog) << "ssh job" << _job.kind;
    proc->start(_findTool(QStringLiteral("ssh")), args);
    if (!_job.input.isEmpty() || _job.kind == QStringLiteral("start")) {
        proc->write(_job.input);
    }
    proc->closeWriteChannel();
    _jobTimer.start(_job.timeoutMs);
}

void TarBagRecorder::_jobFinished(int exitCode, const QByteArray &out, const QByteArray &err, QProcess::ProcessError procError)
{
    const Job job = _job;
    _jobActive = false;
    const bool timedOut = procError == QProcess::Timedout;

    bool transportFailed = timedOut || procError == QProcess::FailedToStart || exitCode == 255 || exitCode == 127 || exitCode < 0;
    QJsonObject obj;
    if (!transportFailed) {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(out, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
            _setError(tr("Unexpected reply from the Jetson: %1").arg(shortText(out.isEmpty() ? err : out, 300)));
            transportFailed = true;
            if (job.kind == QStringLiteral("status")) _setState(QStringLiteral("unknown"));
            if (job.kind == QStringLiteral("manifest")) _setTransfer(QStringLiteral("failed"), _lastError);
            _setBusyNotify();
            _runNext();
            return;
        }
        obj = doc.object();
    }

    if (transportFailed) {
        const QString msg = classifySshFailure(exitCode, QString::fromUtf8(err), procError, timedOut);
        qCWarning(TarBagRecorderLog) << job.kind << "failed:" << msg;
        _setError(msg);
        // Network problems mean "unknown", never "stopped".
        if (job.kind == QStringLiteral("status") || job.kind == QStringLiteral("start") || job.kind == QStringLiteral("stop")) {
            _lastStatusOk = false;
            if (_state != QStringLiteral("unknown")) { _state = QStringLiteral("unknown"); }
            emit stateChanged();
        }
        if (job.kind == QStringLiteral("manifest")) {
            _pendingDownload = false;
            _setTransfer(QStringLiteral("failed"), msg);
        }
    } else {
        _handleReply(job.kind, obj, obj.value(QStringLiteral("ok")).toBool());
    }
    _setBusyNotify();
    _runNext();
}

// ------------------------------------------------------------------------------------------------ replies
void TarBagRecorder::_applyStatus(const QJsonObject &o)
{
    const QString state = o.value(QStringLiteral("state")).toString();
    if (!kStates.contains(state)) {
        _setError(tr("Jetson returned an unknown recorder state: %1").arg(state.left(40)));
        _setState(QStringLiteral("unknown"));
        return;
    }
    const QString sid = o.value(QStringLiteral("session_id")).toString();
    const QString path = o.value(QStringLiteral("bag_path")).toString();
    if (!sid.isEmpty() && (!isValidSessionId(sid) || !isValidRemotePath(path, sid))) {
        _setError(tr("Jetson returned an invalid session id or bag path; ignoring it."));
        _setState(QStringLiteral("unknown"));
        return;
    }
    _lastStatusOk = true;
    _sessionId = sid;
    _remoteBagPath = path;
    _startedAt = o.value(QStringLiteral("started_at")).toString();
    _freeBytes = o.value(QStringLiteral("free_bytes")).toDouble();
    const QString err = o.value(QStringLiteral("last_error")).toString();
    _setError(state == QStringLiteral("failed") ? (err.isEmpty() ? tr("Recording failed on the Jetson.") : err) : err);
    _setState(state);

    if (_pendingDownload && sid == _downloadId) {
        if (state == QStringLiteral("finalized")) {
            _pendingDownload = false;
            emit transferChanged();
            downloadBag(sid);
        } else if (state == QStringLiteral("failed") || state == QStringLiteral("idle")) {
            _pendingDownload = false;
            emit transferChanged();
        }
    }
    // While stopping, poll quickly instead of waiting for the slow timer.
    if (state == QStringLiteral("stopping") || state == QStringLiteral("starting")) {
        QTimer::singleShot(1500, this, &TarBagRecorder::refreshStatus);
    }
}

void TarBagRecorder::_handleReply(const QString &kind, const QJsonObject &o, bool ok)
{
    const QString code = o.value(QStringLiteral("error_code")).toString();
    const QString message = o.value(QStringLiteral("message")).toString();

    if (kind == QStringLiteral("status")) {
        if (!ok) { _setError(message); _setState(QStringLiteral("unknown")); return; }
        _applyStatus(o);
    } else if (kind == QStringLiteral("start")) {
        if (ok) {
            _applyStatus(o);
            _setState(QStringLiteral("starting"));
        } else if (code == QStringLiteral("already_recording")) {
            _setError(tr("A recording is already active on the Jetson (not starting a second one)."));
            refreshStatus();
        } else {
            _setError(tr("Start failed (%1): %2").arg(code, message));
            refreshStatus();
        }
    } else if (kind == QStringLiteral("stop")) {
        if (ok) _applyStatus(o); else { _pendingDownload = false; _setError(tr("Stop failed (%1): %2").arg(code, message)); refreshStatus(); }
    } else if (kind == QStringLiteral("doctor")) {
        QStringList lines;
        for (const QJsonValue &v : o.value(QStringLiteral("checks")).toArray()) {
            const QJsonObject c = v.toObject();
            lines << QStringLiteral("%1 %2: %3").arg(c.value(QStringLiteral("ok")).toBool() ? QStringLiteral("[ok]  ") : QStringLiteral("[FAIL]"),
                                                     c.value(QStringLiteral("name")).toString(), c.value(QStringLiteral("detail")).toString());
        }
        if (lines.isEmpty()) lines << (message.isEmpty() ? tr("No report") : message);
        _doctorReport = lines.join(QLatin1Char('\n'));
        emit doctorChanged();
    } else if (kind == QStringLiteral("manifest")) {
        if (!ok) {
            _pendingDownload = false;
            _setTransfer(QStringLiteral("failed"), tr("Bag is not ready to download: %1").arg(message));
            return;
        }
        _manifest.clear();
        _manifestTotal = 0;
        const QJsonArray files = o.value(QStringLiteral("files")).toArray();
        if (files.isEmpty() || files.size() > kMaxManifestFiles) {
            _setTransfer(QStringLiteral("failed"), tr("The Jetson returned an empty or oversized file list."));
            return;
        }
        for (const QJsonValue &v : files) {
            const QJsonObject f = v.toObject();
            ManifestFile mf;
            mf.path = f.value(QStringLiteral("path")).toString();
            mf.size = qint64(f.value(QStringLiteral("size")).toDouble(-1));
            mf.sha256 = f.value(QStringLiteral("sha256")).toString();
            if (!isValidManifestPath(mf.path) || mf.size < 0) {
                _setTransfer(QStringLiteral("failed"), tr("The Jetson returned an unsafe file list; refusing to download."));
                return;
            }
            _manifestTotal += mf.size;
            _manifest.append(mf);
        }
        _startScp();
    }
}

// ------------------------------------------------------------------------------------------------ public actions
void TarBagRecorder::refreshStatus()
{
    _enqueue({QStringLiteral("status"), {QStringLiteral("status"), QStringLiteral("--json")}, {}, 20000});
}

void TarBagRecorder::runDoctor()
{
    _enqueue({QStringLiteral("doctor"), {QStringLiteral("doctor"), QStringLiteral("--json")}, {}, 60000});
}

void TarBagRecorder::startRecording()
{
    static const QStringList active = {QStringLiteral("starting"), QStringLiteral("recording"), QStringLiteral("stopping")};
    if (active.contains(_state) || (_jobActive && _job.kind == QStringLiteral("start"))) return;   // no double start
    for (const Job &j : std::as_const(_queue)) if (j.kind == QStringLiteral("start")) return;
    _setError({});
    QJsonObject opts;
    opts[QStringLiteral("exclude_regex")] = _excludeRegex;
    opts[QStringLiteral("vehicle_label")] = _vehicleLabel;
    _setTransfer(QStringLiteral("none"), {});
    _enqueue({QStringLiteral("start"), {QStringLiteral("start"), QStringLiteral("--json")},
              QJsonDocument(opts).toJson(QJsonDocument::Compact), 30000});
}

void TarBagRecorder::stopRecording()
{
    if (!isValidSessionId(_sessionId)) return;
    if (_state != QStringLiteral("recording") && _state != QStringLiteral("starting")) return;
    _enqueue({QStringLiteral("stop"), {QStringLiteral("stop"), QStringLiteral("--id"), _sessionId, QStringLiteral("--json")}, {}, 30000});
}

void TarBagRecorder::stopAndDownload()
{
    if (!isValidSessionId(_sessionId)) return;
    _downloadId = _sessionId;
    if (_state == QStringLiteral("finalized")) {
        downloadBag(_sessionId);
        return;
    }
    _pendingDownload = true;
    emit transferChanged();
    stopRecording();
}

void TarBagRecorder::downloadBag(const QString &sessionId)
{
    const QString id = sessionId.isEmpty() ? _sessionId : sessionId;
    if (_transferState == QStringLiteral("downloading") || _transferState == QStringLiteral("preparing")
        || _transferState == QStringLiteral("verifying")) {
        return;
    }
    if (!isValidSessionId(id) || id != _sessionId || _state != QStringLiteral("finalized")
        || !isValidRemotePath(_remoteBagPath, id)) {
        _setTransfer(QStringLiteral("failed"), tr("Only a finalized bag can be downloaded. Stop the recording and wait for it to finalize."));
        return;
    }
    _downloadId = id;
    _downloadRemotePath = _remoteBagPath;
    _setTransfer(QStringLiteral("preparing"), tr("Asking the Jetson for the file list..."));
    QStringList args = {QStringLiteral("manifest"), QStringLiteral("--id"), id, QStringLiteral("--json")};
    if (_verifyChecksums) args << QStringLiteral("--sha256");
    _enqueue({QStringLiteral("manifest"), args, {}, _verifyChecksums ? 1800000 : 60000});
}

void TarBagRecorder::cancelDownload()
{
    _pendingDownload = false;
    if (_scpProcess) {
        _scpCancelled = true;
        _scpProcess->kill();     // local scp only; the Jetson copy is untouched
    }
    emit transferChanged();
}

void TarBagRecorder::openDownloadFolder() const
{
    const QString p = _lastLocalFolder.isEmpty() ? _localDownloadFolder : _lastLocalFolder;
    QDir().mkpath(p);
    (void) QDesktopServices::openUrl(QUrl::fromLocalFile(p));
}

// ------------------------------------------------------------------------------------------------ download
void TarBagRecorder::_startScp()
{
    const QString root = QDir::fromNativeSeparators(_localDownloadFolder);
    _finalDir = root + QStringLiteral("/qgc-") + _downloadId;
    _incompleteDir = root + QStringLiteral("/.incomplete-") + _downloadId;

    if (QFileInfo::exists(_finalDir)) {
        _setTransfer(QStringLiteral("failed"), tr("%1 already exists; refusing to overwrite. Move or rename it and retry.").arg(QDir::toNativeSeparators(_finalDir)));
        return;
    }
    QDir(_incompleteDir).removeRecursively();      // our own earlier partial copy only
    if (!QDir().mkpath(_incompleteDir)) {
        _setTransfer(QStringLiteral("failed"), tr("Cannot create %1").arg(QDir::toNativeSeparators(_incompleteDir)));
        return;
    }
    const QString scp = _findTool(QStringLiteral("scp"));
    if (scp.isEmpty()) {
        _setTransfer(QStringLiteral("failed"), classifySshFailure(0, {}, QProcess::FailedToStart, false));
        return;
    }

    QStringList args = {QStringLiteral("-r")};
    args += _sshCommonOptions();
    args << (_target() + QLatin1Char(':') + _downloadRemotePath) << QStringLiteral(".");   // "." avoids "C:" being parsed as a host

    _scpStderr.clear();
    _scpCancelled = false;
    _scpProcess = new QProcess(this);
    _scpProcess->setWorkingDirectory(_incompleteDir);
    connect(_scpProcess, &QProcess::readyReadStandardError, this, [this]() {
        _scpStderr += QString::fromUtf8(_scpProcess->readAllStandardError());
        if (_scpStderr.size() > 4000) _scpStderr = _scpStderr.right(4000);
    });
    connect(_scpProcess, &QProcess::finished, this, &TarBagRecorder::_scpFinished);
    connect(_scpProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) _scpFinished(-1, QProcess::CrashExit);
    });
    _setTransfer(QStringLiteral("downloading"), tr("Starting..."));
    _scpProcess->start(scp, args);
    _progressTimer.start();
}

void TarBagRecorder::_scanProgress()
{
    int files = 0;
    qint64 bytes = 0;
    QDirIterator it(_incompleteDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++files;
        bytes += it.fileInfo().size();
    }
    // No percentage: scp gives no reliable byte progress. Show counts instead.
    _transferDetail = tr("%1 of %2 files, %3 of %4 received (in progress)")
                          .arg(files).arg(_manifest.size()).arg(formatBytes(double(bytes)), formatBytes(double(_manifestTotal)));
    emit transferChanged();
}

void TarBagRecorder::_scpFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!_scpProcess) return;
    _progressTimer.stop();
    _scpStderr += QString::fromUtf8(_scpProcess->readAllStandardError());
    _scpProcess->deleteLater();
    _scpProcess = nullptr;

    if (_scpCancelled) {
        _setTransfer(QStringLiteral("failed"), tr("Download cancelled. Partial copy kept in %1; the Jetson copy is untouched.").arg(QDir::toNativeSeparators(_incompleteDir)));
        return;
    }
    if (status != QProcess::NormalExit || exitCode != 0) {
        const QString why = classifySshFailure(exitCode == 0 ? 1 : exitCode, _scpStderr, exitCode < 0 ? QProcess::FailedToStart : QProcess::UnknownError, false);
        _setTransfer(QStringLiteral("failed"), tr("Download interrupted: %1 Partial copy kept in %2 - press Download bag to retry. The Jetson copy was not changed.")
                                                   .arg(why, QDir::toNativeSeparators(_incompleteDir)));
        return;
    }
    _verifyAndFinish();
}

void TarBagRecorder::_verifyAndFinish()
{
    _setTransfer(QStringLiteral("verifying"), tr("Checking file names and sizes%1...").arg(_verifyChecksums ? tr(" and SHA-256") : QString()));
    const QString bagDir = _incompleteDir + QStringLiteral("/qgc-") + _downloadId;
    const QList<ManifestFile> manifest = _manifest;
    const bool hashes = _verifyChecksums;
    auto result = std::make_shared<QString>();

    // Hashing multi-GB files must not block the UI thread.
    QThread *thread = QThread::create([bagDir, manifest, hashes, result]() {
        for (const ManifestFile &f : manifest) {
            QFileInfo fi(bagDir + QLatin1Char('/') + f.path);
            if (!fi.isFile()) { *result = QStringLiteral("missing file %1").arg(f.path); return; }
            if (fi.size() != f.size) {
                *result = QStringLiteral("size mismatch for %1 (%2 instead of %3 bytes)").arg(f.path).arg(fi.size()).arg(f.size);
                return;
            }
            if (hashes && !f.sha256.isEmpty()) {
                QFile file(fi.absoluteFilePath());
                QCryptographicHash h(QCryptographicHash::Sha256);
                if (!file.open(QIODevice::ReadOnly) || !h.addData(&file) || h.result().toHex() != f.sha256.toLatin1()) {
                    *result = QStringLiteral("checksum mismatch for %1").arg(f.path);
                    return;
                }
            }
        }
    });
    connect(thread, &QThread::finished, this, [this, thread, result, bagDir]() {
        thread->deleteLater();
        if (!result->isEmpty()) {
            _setTransfer(QStringLiteral("failed"), tr("Downloaded copy is incomplete (%1). Press Download bag to retry; partial copy kept in %2.")
                                                       .arg(*result, QDir::toNativeSeparators(_incompleteDir)));
            return;
        }
        if (QFileInfo::exists(_finalDir) || !QDir().rename(bagDir, _finalDir)) {
            _setTransfer(QStringLiteral("failed"), tr("Verified, but could not move the bag into %1. It is still in %2.")
                                                       .arg(QDir::toNativeSeparators(_finalDir), QDir::toNativeSeparators(bagDir)));
            return;
        }
        QDir().rmdir(_incompleteDir);    // only succeeds when empty
        _lastLocalFolder = _finalDir;
        _setTransfer(QStringLiteral("completed"), tr("Saved to %1 (%2 files, %3)")
                                                      .arg(QDir::toNativeSeparators(_finalDir)).arg(_manifest.size()).arg(formatBytes(double(_manifestTotal))));
    });
    thread->start();
}
