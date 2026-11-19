#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

Q_DECLARE_LOGGING_CATEGORY(TarBagRecorderLog)

/// Controls a persistent ROS 2 bag recording on a Jetson through `qgc-bagctl` over OpenSSH (see jetson/).
///
/// - Every remote operation is one non-blocking `ssh.exe` QProcess running a FIXED remote command; user input
///   (regex, label) travels as JSON on stdin and is never spliced into a shell string.
/// - The recorder runs under the Jetson's systemd, so closing QGC / SSH / NoMachine never stops it.
/// - "recording" is only reported when the Jetson says so; SSH failures give state "unknown", never "stopped".
/// - Bags are downloaded with scp into an ".incomplete-<id>" folder, verified against a server manifest, then
///   renamed. Nothing is ever deleted on the Jetson.
class TarBagRecorder : public QObject
{
    Q_OBJECT

    // Recorder state as reported by the Jetson: unknown, idle, starting, recording, stopping, finalized, failed.
    Q_PROPERTY(QString state            READ state              NOTIFY stateChanged)
    // none, preparing, downloading, verifying, completed, failed
    Q_PROPERTY(QString transferState    READ transferState      NOTIFY transferChanged)
    Q_PROPERTY(QString transferDetail   READ transferDetail     NOTIFY transferChanged)
    Q_PROPERTY(QString statusText       READ statusText         NOTIFY stateChanged)
    Q_PROPERTY(bool    busy             READ busy               NOTIFY busyChanged)
    Q_PROPERTY(QString lastError        READ lastError          NOTIFY lastErrorChanged)
    Q_PROPERTY(QString activeSessionId  READ activeSessionId    NOTIFY stateChanged)
    Q_PROPERTY(QString remoteBagPath    READ remoteBagPath      NOTIFY stateChanged)
    Q_PROPERTY(QString startedAt        READ startedAt          NOTIFY stateChanged)
    Q_PROPERTY(double  freeBytes        READ freeBytes          NOTIFY stateChanged)
    Q_PROPERTY(QString lastLocalFolder  READ lastLocalFolder    NOTIFY transferChanged)
    Q_PROPERTY(QString doctorReport     READ doctorReport       NOTIFY doctorChanged)
    Q_PROPERTY(bool    pendingDownload  READ pendingDownload    NOTIFY transferChanged)
    Q_PROPERTY(bool    configured       READ configured         NOTIFY settingsChanged)

    Q_PROPERTY(QString host                 READ host               WRITE setHost               NOTIFY settingsChanged)
    Q_PROPERTY(QString sshUser              READ sshUser            WRITE setSshUser            NOTIFY settingsChanged)
    Q_PROPERTY(QString sshIdentityFile      READ sshIdentityFile    WRITE setSshIdentityFile    NOTIFY settingsChanged)
    Q_PROPERTY(QString remoteCommand        READ remoteCommand      WRITE setRemoteCommand      NOTIFY settingsChanged)
    Q_PROPERTY(QString excludeRegex         READ excludeRegex       WRITE setExcludeRegex       NOTIFY settingsChanged)
    Q_PROPERTY(QString vehicleLabel         READ vehicleLabel       WRITE setVehicleLabel       NOTIFY settingsChanged)
    Q_PROPERTY(QString localDownloadFolder  READ localDownloadFolder WRITE setLocalDownloadFolder NOTIFY settingsChanged)
    Q_PROPERTY(bool    verifyChecksums      READ verifyChecksums    WRITE setVerifyChecksums    NOTIFY settingsChanged)

public:
    explicit TarBagRecorder(QObject *parent = nullptr);

    /// Called once the event loop runs: restores state from the Jetson and starts polling.
    void init();
    /// Kills local ssh/scp helpers only. Never stops the remote recording.
    void shutdown();

    QString state() const { return _state; }
    QString transferState() const { return _transferState; }
    QString transferDetail() const { return _transferDetail; }
    QString statusText() const;
    bool busy() const { return _jobActive || !_queue.isEmpty(); }
    QString lastError() const { return _lastError; }
    QString activeSessionId() const { return _sessionId; }
    QString remoteBagPath() const { return _remoteBagPath; }
    QString startedAt() const { return _startedAt; }
    double freeBytes() const { return _freeBytes; }
    QString lastLocalFolder() const { return _lastLocalFolder; }
    QString doctorReport() const { return _doctorReport; }
    bool pendingDownload() const { return _pendingDownload; }
    bool configured() const;

    QString host() const { return _host; }
    QString sshUser() const { return _sshUser; }
    QString sshIdentityFile() const { return _sshIdentityFile; }
    QString remoteCommand() const { return _remoteCommand; }
    QString excludeRegex() const { return _excludeRegex; }
    QString vehicleLabel() const { return _vehicleLabel; }
    QString localDownloadFolder() const { return _localDownloadFolder; }
    bool verifyChecksums() const { return _verifyChecksums; }

    void setHost(const QString &v);
    void setSshUser(const QString &v);
    void setSshIdentityFile(const QString &v);
    void setRemoteCommand(const QString &v);
    void setExcludeRegex(const QString &v);
    void setVehicleLabel(const QString &v);
    void setLocalDownloadFolder(const QString &v);
    void setVerifyChecksums(bool v);

    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void stopAndDownload();
    Q_INVOKABLE void refreshStatus();
    Q_INVOKABLE void downloadBag(const QString &sessionId = QString());
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void runDoctor();
    Q_INVOKABLE void openDownloadFolder() const;
    /// The exact command line that will run on the Jetson for start (for display).
    Q_INVOKABLE QString commandPreview() const;
    /// Empty if the settings are usable, otherwise a message for the user.
    Q_INVOKABLE QString settingsProblem() const;

    // Validation helpers (public + static so they can be unit tested).
    static bool isValidSessionId(const QString &id);
    static bool isValidRemotePath(const QString &path, const QString &sessionId);
    static bool isValidManifestPath(const QString &rel);
    static QString classifySshFailure(int exitCode, const QString &stderrText, QProcess::ProcessError procError, bool timedOut);

signals:
    void stateChanged();
    void transferChanged();
    void busyChanged();
    void lastErrorChanged();
    void doctorChanged();
    void settingsChanged();

private:
    struct Job {
        QString kind;               // start, stop, status, doctor, manifest
        QStringList remoteArgs;     // fixed tokens only
        QByteArray input;           // JSON for stdin
        int timeoutMs = 20000;
    };
    struct ManifestFile {
        QString path;
        qint64 size = 0;
        QString sha256;
    };

    void _enqueue(const Job &job);
    void _runNext();
    void _jobFinished(int exitCode, const QByteArray &out, const QByteArray &err, QProcess::ProcessError procError);
    void _handleReply(const QString &kind, const QJsonObject &obj, bool ok);
    void _applyStatus(const QJsonObject &obj);
    void _setState(const QString &s);
    void _setError(const QString &e);
    void _setTransfer(const QString &s, const QString &detail);
    void _setBusyNotify();
    QStringList _sshCommonOptions() const;
    QString _target() const;
    QString _findTool(const QString &name) const;
    void _startScp();
    void _scanProgress();
    void _scpFinished(int exitCode, QProcess::ExitStatus status);
    void _verifyAndFinish();
    void _loadSettings();
    void _save(const QString &key, const QVariant &value);

    // settings
    QString _host, _sshUser, _sshIdentityFile, _remoteCommand = QStringLiteral("~/.local/bin/qgc-bagctl");
    QString _excludeRegex, _vehicleLabel, _localDownloadFolder;
    bool _verifyChecksums = false;

    // remote state
    QString _state = QStringLiteral("unknown");
    QString _sessionId, _remoteBagPath, _startedAt;
    double _freeBytes = 0;
    QString _lastError;
    QString _doctorReport;
    bool _lastStatusOk = false;

    // job queue
    QList<Job> _queue;
    bool _jobActive = false;
    Job _job;
    QProcess *_sshProcess = nullptr;
    QTimer _jobTimer;
    bool _jobTimedOut = false;
    QTimer _pollTimer;

    // download
    QString _transferState = QStringLiteral("none");
    QString _transferDetail;
    QString _lastLocalFolder;
    bool _pendingDownload = false;     // "stop & download" waiting for finalization
    QString _downloadId;
    QString _downloadRemotePath;
    QList<ManifestFile> _manifest;
    qint64 _manifestTotal = 0;
    QString _incompleteDir, _finalDir;
    QProcess *_scpProcess = nullptr;
    bool _scpCancelled = false;
    QTimer _progressTimer;
    QString _scpStderr;
};
