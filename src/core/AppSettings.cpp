#include "AppSettings.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QSettings>
#include <QUuid>
#include <QDebug>
#include <QCoreApplication>

namespace AetherSDR {

AppSettings& AppSettings::instance()
{
    static AppSettings s;
    return s;
}

AppSettings::AppSettings()
{
    const QString configDir =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + "/AetherSDR";
    QDir().mkpath(configDir);
    m_filePath = configDir + "/AetherSDR.settings";
}

// ─── Load ─────────────────────────────────────────────────────────────────────

void AppSettings::load()
{
    QFile file(m_filePath);
    if (!file.exists()) {
        // First launch or migration needed
        migrateFromQSettings();
        return;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "AppSettings: cannot open" << m_filePath;
        return;
    }

    m_settings.clear();
    m_stationSettings.clear();

    QXmlStreamReader xml(&file);
    QString currentStation;  // non-empty when inside a station element

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            const QString tag = xml.name().toString();

            if (tag == "Settings") continue;  // root element

            // Check if this is the station element
            if (tag == m_stationName && currentStation.isEmpty()) {
                currentStation = tag;
                continue;
            }

            // Read the text content
            const QString text = xml.readElementText();

            if (!currentStation.isEmpty()) {
                m_stationSettings.insert(tag, text);
            } else {
                m_settings.insert(tag, text);
                // Track station name
                if (tag == "StationName")
                    m_stationName = text;
            }
        } else if (xml.isEndElement()) {
            if (xml.name().toString() == m_stationName && !currentStation.isEmpty())
                currentStation.clear();
        }
    }

    if (xml.hasError())
        qWarning() << "AppSettings: XML parse error:" << xml.errorString();

    file.close();
    qDebug() << "AppSettings: loaded" << m_settings.size() << "settings +"
             << m_stationSettings.size() << "station settings from" << m_filePath;
}

// ─── Save ─────────────────────────────────────────────────────────────────────

void AppSettings::save()
{
    QFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "AppSettings: cannot write" << m_filePath;
        return;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);
    xml.writeStartDocument();
    xml.writeStartElement("Settings");

    // Write top-level settings (sorted for consistency)
    QList<QString> keys = m_settings.keys();
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        // Skip station name element here — it goes inline
        xml.writeTextElement(key, m_settings.value(key));
    }

    // Write per-station section
    if (!m_stationSettings.isEmpty()) {
        xml.writeStartElement(m_stationName);
        QList<QString> stKeys = m_stationSettings.keys();
        std::sort(stKeys.begin(), stKeys.end());
        for (const auto& key : stKeys) {
            xml.writeTextElement(key, m_stationSettings.value(key));
        }
        xml.writeEndElement(); // station
    }

    xml.writeEndElement(); // Settings
    xml.writeEndDocument();
    file.close();
}

// ─── Top-level accessors ──────────────────────────────────────────────────────

QVariant AppSettings::value(const QString& key, const QVariant& defaultValue) const
{
    if (m_settings.contains(key))
        return m_settings.value(key);
    return defaultValue;
}

void AppSettings::setValue(const QString& key, const QVariant& val)
{
    m_settings.insert(key, val.toString());
}

void AppSettings::remove(const QString& key)
{
    m_settings.remove(key);
}

bool AppSettings::contains(const QString& key) const
{
    return m_settings.contains(key);
}

// ─── Per-station accessors ────────────────────────────────────────────────────

QVariant AppSettings::stationValue(const QString& key, const QVariant& defaultValue) const
{
    if (m_stationSettings.contains(key))
        return m_stationSettings.value(key);
    return defaultValue;
}

void AppSettings::setStationValue(const QString& key, const QVariant& val)
{
    m_stationSettings.insert(key, val.toString());
}

QString AppSettings::stationName() const
{
    return m_stationName;
}

void AppSettings::setStationName(const QString& name)
{
    m_stationName = name;
    m_settings.insert("StationName", name);
}

// ─── Migration from QSettings ─────────────────────────────────────────────────

void AppSettings::migrateFromQSettings()
{
    QSettings old("AetherSDR", "AetherSDR");
    const QStringList keys = old.allKeys();

    if (keys.isEmpty()) {
        // No old settings — first launch. Set defaults.
        setValue("ApplicationVersion", QCoreApplication::applicationVersion());
        setValue("AutoConnect", "True");
        setValue("StationName", "AetherSDR");
        setValue("GUIClientID", QUuid::createUuid().toString(QUuid::WithoutBraces));
        setValue("IsSingleClickTuneEnabled", "False");
        setValue("IsSpotsEnabled", "True");
        setValue("SpotsMaxLevel", "3");
        setValue("SpotFontSize", "16");
        setValue("FavoriteMode0", "USB");
        setValue("FavoriteMode1", "CW");
        setValue("FavoriteMode2", "AM");
        save();
        qDebug() << "AppSettings: created new settings file with defaults";
        return;
    }

    qDebug() << "AppSettings: migrating" << keys.size() << "keys from QSettings";

    // Map old QSettings keys to new XML keys
    // MainWindow
    if (old.contains("lastRadioSerial"))
        setValue("LastConnectedRadioSerial", old.value("lastRadioSerial").toString());
    if (old.contains("geometry"))
        setValue("MainWindowGeometry", old.value("geometry").toByteArray().toBase64());
    if (old.contains("windowState"))
        setValue("MainWindowState", old.value("windowState").toByteArray().toBase64());
    if (old.contains("splitterState"))
        setValue("SplitterState", old.value("splitterState").toByteArray().toBase64());

    // Spectrum
    if (old.contains("spectrum/splitRatio"))
        setValue("SpectrumSplitRatio", old.value("spectrum/splitRatio").toString());

    // Spots
    if (old.contains("spots/enabled"))
        setValue("IsSpotsEnabled", old.value("spots/enabled").toBool() ? "True" : "False");
    if (old.contains("spots/levels"))
        setValue("SpotsMaxLevel", old.value("spots/levels").toString());
    if (old.contains("spots/position"))
        setValue("SpotsStartingHeightPercentage", old.value("spots/position").toString());
    if (old.contains("spots/fontSize"))
        setValue("SpotFontSize", old.value("spots/fontSize").toString());
    if (old.contains("spots/overrideColors"))
        setValue("IsSpotsOverrideColorsEnabled",
                 old.value("spots/overrideColors").toBool() ? "True" : "False");
    if (old.contains("spots/overrideBg"))
        setValue("IsSpotsOverrideBackgroundColorsEnabled",
                 old.value("spots/overrideBg").toBool() ? "True" : "False");
    if (old.contains("spots/overrideBgAuto"))
        setValue("IsSpotsOverrideToAutoBackgroundColorEnabled",
                 old.value("spots/overrideBgAuto").toBool() ? "True" : "False");

    // Set defaults for new keys
    setValue("ApplicationVersion", QCoreApplication::applicationVersion());
    setValue("AutoConnect", "True");
    setValue("StationName", "AetherSDR");
    setValue("GUIClientID", QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!contains("IsSingleClickTuneEnabled"))
        setValue("IsSingleClickTuneEnabled", "False");
    if (!contains("FavoriteMode0")) setValue("FavoriteMode0", "USB");
    if (!contains("FavoriteMode1")) setValue("FavoriteMode1", "CW");
    if (!contains("FavoriteMode2")) setValue("FavoriteMode2", "AM");

    save();
    qDebug() << "AppSettings: migration complete, saved to" << m_filePath;
}

} // namespace AetherSDR
