/*
 * main.cpp - Qt6 App Launcher backend
 *
 * Provides two QObjects exposed to QML:
 *   LaunchManager — launches child apps via QProcess, hides the
 *                   launcher window while the child owns the display.
 *   SystemInfo    — reads CPU, memory, temperature, uptime, IP from
 *                   sysfs / procfs and exposes them as Q_PROPERTYs.
 *
 * Build:
 *   cmake -B build && cmake --build build
 *
 * Run:
 *   QT_QPA_PLATFORM=eglfs ./build/qt_launcher
 *
 * Copyright (C) 2025 Obuda University - Embedded Systems Lab
 * SPDX-License-Identifier: MIT
 */

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QObject>
#include <QTimer>
#include <QProcess>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QWindow>
#include <QNetworkInterface>

#include <xf86drm.h>    /* drmDropMaster / drmSetMaster */

/* ── LaunchManager ──────────────────────────────────────────── */

class LaunchManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool childRunning READ childRunning NOTIFY childRunningChanged)

public:
    explicit LaunchManager(QObject *parent = nullptr)
        : QObject(parent) {}

    bool childRunning() const { return m_childRunning; }

    Q_INVOKABLE void launch(const QString &command) {
        if (m_childRunning) return;

        m_childRunning = true;
        emit childRunningChanged();

        /* Release the display so the child can claim DRM master */
        hideWindow();
        dropDrmMaster();

        m_process = new QProcess(this);
        m_process->setProcessChannelMode(QProcess::ForwardedChannels);

        connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus) {
            /* Remove PID file so home-button daemon knows child is gone */
            QFile::remove("/tmp/launcher_child.pid");

            m_process->deleteLater();
            m_process = nullptr;
            m_childRunning = false;
            emit childRunningChanged();

            /* Reclaim display */
            acquireDrmMaster();
            showWindow();
            emit childFinished(code);
        });

        m_process->start("/bin/sh", {"-c", command});

        /* Write child PID so the home-button daemon can send SIGTERM */
        QFile pidFile("/tmp/launcher_child.pid");
        if (pidFile.open(QIODevice::WriteOnly)) {
            pidFile.write(QByteArray::number(m_process->processId()));
            pidFile.close();
        }
    }

signals:
    void childRunningChanged();
    void childFinished(int exitCode);

private:
    void hideWindow() {
        for (QWindow *w : QGuiApplication::allWindows())
            w->hide();
    }

    void showWindow() {
        for (QWindow *w : QGuiApplication::allWindows())
            w->show();
    }

    /*
     * Drop DRM master so the child process can claim the display.
     *
     * DRM master is tracked per open file-descriptor, not per process.
     * EGLFS opens /dev/dri/cardN internally and we cannot access that
     * fd through public Qt API.  Instead we scan /proc/self/fd/ to
     * find every fd that points to a DRM card device and call
     * drmDropMaster() on the EGLFS-held fd directly.
     *
     * We do NOT close the fd — it belongs to EGLFS.  We just
     * temporarily relinquish master status.
     */
    void dropDrmMaster() {
        QDir fdDir("/proc/self/fd");
        for (const QString &entry : fdDir.entryList(QDir::NoDotAndDotDot)) {
            QString target = QFile::symLinkTarget(fdDir.filePath(entry));
            if (target.startsWith("/dev/dri/card")) {
                int fd = entry.toInt();
                if (drmDropMaster(fd) == 0)
                    m_drmFds.append(fd);
            }
        }
    }

    /*
     * Re-acquire DRM master on the fds we previously dropped.
     * EGLFS will resume rendering after the window is shown.
     */
    void acquireDrmMaster() {
        for (int fd : m_drmFds)
            drmSetMaster(fd);
        m_drmFds.clear();
    }

    QProcess *m_process = nullptr;
    QList<int> m_drmFds;
    bool m_childRunning = false;
};

/* ── SystemInfo ─────────────────────────────────────────────── */

class SystemInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(float cpuPercent READ cpuPercent NOTIFY updated)
    Q_PROPERTY(int memTotalMB READ memTotalMB NOTIFY updated)
    Q_PROPERTY(int memUsedMB READ memUsedMB NOTIFY updated)
    Q_PROPERTY(float temperature READ temperature NOTIFY updated)
    Q_PROPERTY(QString uptime READ uptime NOTIFY updated)
    Q_PROPERTY(QString ipAddress READ ipAddress NOTIFY updated)
    Q_PROPERTY(QString hostname READ hostname CONSTANT)

public:
    explicit SystemInfo(QObject *parent = nullptr)
        : QObject(parent)
    {
        readCpuJiffies();

        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &SystemInfo::poll);
        timer->start(2000);
        poll();
    }

    float cpuPercent() const { return m_cpuPercent; }
    int memTotalMB() const { return m_memTotalMB; }
    int memUsedMB() const { return m_memUsedMB; }
    float temperature() const { return m_temperature; }
    QString uptime() const { return m_uptime; }
    QString ipAddress() const { return m_ipAddress; }
    QString hostname() const { return QSysInfo::machineHostName(); }

signals:
    void updated();

private slots:
    void poll() {
        /* CPU */
        float cpu = readCpuJiffies();
        if (cpu >= 0) m_cpuPercent = cpu;

        /* Memory from /proc/meminfo */
        QFile mf("/proc/meminfo");
        if (mf.open(QIODevice::ReadOnly)) {
            long total = 0, avail = 0;
            QTextStream ts(&mf);
            while (!ts.atEnd()) {
                QString line = ts.readLine();
                if (line.startsWith("MemTotal:"))
                    total = line.split(' ', Qt::SkipEmptyParts).at(1).toLong();
                else if (line.startsWith("MemAvailable:"))
                    avail = line.split(' ', Qt::SkipEmptyParts).at(1).toLong();
            }
            m_memTotalMB = total / 1024;
            m_memUsedMB = (total - avail) / 1024;
        }

        /* CPU temperature */
        QFile tf("/sys/class/thermal/thermal_zone0/temp");
        if (tf.open(QIODevice::ReadOnly)) {
            bool ok;
            int millideg = QTextStream(&tf).readAll().trimmed().toInt(&ok);
            if (ok) m_temperature = millideg / 1000.0f;
        }

        /* Uptime */
        QFile uf("/proc/uptime");
        if (uf.open(QIODevice::ReadOnly)) {
            float secs = QTextStream(&uf).readAll().split(' ').first().toFloat();
            int h = (int)secs / 3600;
            int m = ((int)secs % 3600) / 60;
            m_uptime = QString("%1h %2m").arg(h).arg(m);
        }

        /* IP address — first non-loopback IPv4 */
        m_ipAddress = "no network";
        for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
            if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
            if (!iface.flags().testFlag(QNetworkInterface::IsUp)) continue;
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    m_ipAddress = entry.ip().toString();
                    break;
                }
            }
            if (m_ipAddress != "no network") break;
        }

        emit updated();
    }

private:
    float readCpuJiffies() {
        QFile f("/proc/stat");
        if (!f.open(QIODevice::ReadOnly)) return -1;
        QStringList parts = QTextStream(&f).readLine().split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 5) return -1;

        unsigned long user = parts[1].toULong();
        unsigned long nice = parts[2].toULong();
        unsigned long sys  = parts[3].toULong();
        unsigned long idle = parts[4].toULong();
        unsigned long iow  = parts.size() > 5 ? parts[5].toULong() : 0;
        unsigned long irq  = parts.size() > 6 ? parts[6].toULong() : 0;
        unsigned long sirq = parts.size() > 7 ? parts[7].toULong() : 0;
        unsigned long steal= parts.size() > 8 ? parts[8].toULong() : 0;

        unsigned long total = user + nice + sys + idle + iow + irq + sirq + steal;
        unsigned long dt = total - m_prevTotal;
        unsigned long di = idle - m_prevIdle;
        m_prevTotal = total;
        m_prevIdle = idle;

        if (dt == 0) return 0;
        return 100.0f * (1.0f - (float)di / (float)dt);
    }

    float m_cpuPercent = 0;
    int m_memTotalMB = 0;
    int m_memUsedMB = 0;
    float m_temperature = 0;
    QString m_uptime = "0h 0m";
    QString m_ipAddress = "...";
    unsigned long m_prevTotal = 0;
    unsigned long m_prevIdle = 0;
};

/* ── main ───────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty())
        qputenv("QT_QPA_PLATFORM", "eglfs");

    QGuiApplication app(argc, argv);

    LaunchManager launcher;
    SystemInfo sysinfo;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("launcher", &launcher);
    engine.rootContext()->setContextProperty("sysinfo", &sysinfo);
    engine.loadFromModule("Launcher", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}

#include "main.moc"
