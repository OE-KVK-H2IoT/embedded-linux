/*
 * main.cpp - Qt6 + EGLFS dashboard backend
 *
 * Provides a SensorBackend QObject that reads MCP9808 temperature,
 * BMI160 IMU angles, and CPU usage, exposing them as Q_PROPERTYs
 * for QML binding.  When sensors are absent, arrow keys adjust
 * simulated values (same fallback as the SDL2 dashboard).
 *
 * Build:
 *   cmake -B build && cmake --build build
 *
 * Run:
 *   QT_QPA_PLATFORM=eglfs ./build/qt_dashboard
 *
 * Copyright (C) 2025 Obuda University - Embedded Systems Lab
 * SPDX-License-Identifier: MIT
 */

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QObject>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <cmath>

class SensorBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(float temperature READ temperature NOTIFY temperatureChanged)
    Q_PROPERTY(float roll READ roll NOTIFY rollChanged)
    Q_PROPERTY(float pitch READ pitch NOTIFY pitchChanged)
    Q_PROPERTY(float cpuPercent READ cpuPercent NOTIFY cpuPercentChanged)
    Q_PROPERTY(bool hasTempSensor READ hasTempSensor CONSTANT)
    Q_PROPERTY(bool hasImuSensor READ hasImuSensor CONSTANT)

public:
    explicit SensorBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
        /* Resolve MCP9808 sysfs path */
        QDir hwmon("/sys/bus/i2c/devices/1-0018/hwmon");
        if (hwmon.exists()) {
            QStringList dirs = hwmon.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            if (!dirs.isEmpty())
                m_tempPath = hwmon.absoluteFilePath(dirs.first()) + "/temp1_input";
        }
        m_hasTempSensor = QFile::exists(m_tempPath);

        /* Check BMI160 */
        m_hasImuSensor = QFile::exists("/dev/bmi160");

        /* Prime CPU usage */
        readCpuUsage();

        /* 10 Hz polling timer */
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &SensorBackend::poll);
        timer->start(100);
    }

    float temperature() const { return m_temperature; }
    float roll() const { return m_roll; }
    float pitch() const { return m_pitch; }
    float cpuPercent() const { return m_cpuPercent; }
    bool hasTempSensor() const { return m_hasTempSensor; }
    bool hasImuSensor() const { return m_hasImuSensor; }

    /* Keyboard fallback — called from QML */
    Q_INVOKABLE void adjustTemperature(float delta) {
        if (!m_hasTempSensor) {
            m_simTemp += delta;
            m_temperature = m_simTemp;
            emit temperatureChanged();
        }
    }
    Q_INVOKABLE void adjustRoll(float delta) {
        if (!m_hasImuSensor) {
            m_simRoll += delta;
            m_roll = m_simRoll;
            emit rollChanged();
        }
    }

signals:
    void temperatureChanged();
    void rollChanged();
    void pitchChanged();
    void cpuPercentChanged();

private slots:
    void poll() {
        /* Temperature */
        if (m_hasTempSensor) {
            QFile f(m_tempPath);
            if (f.open(QIODevice::ReadOnly)) {
                bool ok;
                int millideg = QTextStream(&f).readAll().trimmed().toInt(&ok);
                if (ok) {
                    m_temperature = millideg / 1000.0f;
                    emit temperatureChanged();
                }
            }
        }

        /* IMU */
        if (m_hasImuSensor) {
            QFile f("/dev/bmi160");
            if (f.open(QIODevice::ReadOnly)) {
                QString line = QTextStream(&f).readLine();
                QStringList parts = line.split(' ');
                if (parts.size() >= 6) {
                    float ax = parts[0].toFloat() / 16384.0f;
                    float ay = parts[1].toFloat() / 16384.0f;
                    float az = parts[2].toFloat() / 16384.0f;
                    m_roll = std::atan2(ay, az) * 180.0f / M_PI;
                    m_pitch = std::atan2(-ax, std::sqrt(ay*ay + az*az))
                              * 180.0f / M_PI;
                    emit rollChanged();
                    emit pitchChanged();
                }
            }
        }

        /* CPU */
        float cpu = readCpuUsage();
        if (cpu >= 0.0f) {
            m_cpuPercent = cpu;
            emit cpuPercentChanged();
        }
    }

private:
    float readCpuUsage() {
        QFile f("/proc/stat");
        if (!f.open(QIODevice::ReadOnly))
            return -1.0f;
        QString line = QTextStream(&f).readLine();
        QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 5)
            return -1.0f;

        unsigned long user = parts[1].toULong();
        unsigned long nice = parts[2].toULong();
        unsigned long sys  = parts[3].toULong();
        unsigned long idle = parts[4].toULong();
        unsigned long iow  = parts.size() > 5 ? parts[5].toULong() : 0;
        unsigned long irq  = parts.size() > 6 ? parts[6].toULong() : 0;
        unsigned long sirq = parts.size() > 7 ? parts[7].toULong() : 0;
        unsigned long steal= parts.size() > 8 ? parts[8].toULong() : 0;

        unsigned long total = user + nice + sys + idle + iow + irq + sirq + steal;
        unsigned long diffTotal = total - m_prevTotal;
        unsigned long diffIdle  = idle - m_prevIdle;
        m_prevTotal = total;
        m_prevIdle  = idle;

        if (diffTotal == 0) return 0.0f;
        return 100.0f * (1.0f - (float)diffIdle / (float)diffTotal);
    }

    QString m_tempPath;
    bool m_hasTempSensor = false;
    bool m_hasImuSensor = false;

    float m_temperature = 22.0f;
    float m_roll = 0.0f;
    float m_pitch = 0.0f;
    float m_cpuPercent = 0.0f;

    float m_simTemp = 22.0f;
    float m_simRoll = 0.0f;

    unsigned long m_prevTotal = 0;
    unsigned long m_prevIdle = 0;
};

int main(int argc, char *argv[])
{
    /* Default to EGLFS if no platform is set */
    if (qgetenv("QT_QPA_PLATFORM").isEmpty())
        qputenv("QT_QPA_PLATFORM", "eglfs");

    QGuiApplication app(argc, argv);

    SensorBackend backend;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.loadFromModule("Dashboard", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}

#include "main.moc"
