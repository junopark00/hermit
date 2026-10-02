#include "connectionprofiles.h"

#include "streamingpreferences.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {

const char* const kSettingsKey = "hermitProfiles";

// StreamingPreferences properties that make up a profile. Everything else (input, UI,
// language, gamepad) stays global.
const char* const kProfileProperties[] = {
    "width", "height", "fps", "bitrateKbps", "unlockBitrate", "autoAdjustBitrate",
    "enableVsync", "framePacing", "windowMode", "videoCodecConfig", "enableHdr", "enableYUV444",
    "audioConfig", "showPerformanceOverlay", "largeRemotePackets",
};

}

ConnectionProfiles::ConnectionProfiles(StreamingPreferences* preferences, QObject* parent)
    : QObject(parent),
      m_Preferences(preferences)
{
    load();
}

QStringList ConnectionProfiles::names() const
{
    QStringList result;
    for (const auto& profile : m_Profiles) {
        result.append(profile.first);
    }
    return result;
}

QString ConnectionProfiles::current() const
{
    const QVariantMap now = snapshot();
    for (const auto& profile : m_Profiles) {
        bool same = true;
        for (auto it = profile.second.constBegin(); it != profile.second.constEnd(); ++it) {
            if (now.value(it.key()).toString() != it.value().toString()) {
                same = false;
                break;
            }
        }
        if (same) {
            return profile.first;
        }
    }
    return {};
}

bool ConnectionProfiles::saveCurrent(const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const int index = indexOf(trimmed);
    if (index >= 0) {
        m_Profiles[index].second = snapshot();
    }
    else {
        m_Profiles.append({ trimmed, snapshot() });
    }
    store();
    emit profilesChanged();
    return true;
}

bool ConnectionProfiles::apply(const QString& name)
{
    const int index = indexOf(name);
    if (index < 0) {
        return false;
    }
    const QVariantMap& values = m_Profiles.at(index).second;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        // Enum properties accept their integer value.
        m_Preferences->setProperty(it.key().toUtf8().constData(), it.value());
    }
    m_Preferences->save();
    emit profilesChanged();
    return true;
}

void ConnectionProfiles::remove(const QString& name)
{
    const int index = indexOf(name);
    if (index < 0) {
        return;
    }
    m_Profiles.removeAt(index);
    store();
    emit profilesChanged();
}

QString ConnectionProfiles::describe(const QString& name) const
{
    const int index = indexOf(name);
    if (index < 0) {
        return {};
    }
    const QVariantMap& v = m_Profiles.at(index).second;
    QStringList parts;
    parts.append(QStringLiteral("%1x%2").arg(v.value("width").toInt()).arg(v.value("height").toInt()));
    parts.append(QStringLiteral("%1 FPS").arg(v.value("fps").toInt()));
    parts.append(QStringLiteral("%1 Mbps").arg(v.value("bitrateKbps").toInt() / 1000.0, 0, 'g', 3));
    switch (v.value("windowMode").toInt()) {
    case StreamingPreferences::WM_FULLSCREEN:
        parts.append(tr("full screen"));
        break;
    case StreamingPreferences::WM_FULLSCREEN_DESKTOP:
        parts.append(tr("borderless"));
        break;
    default:
        parts.append(tr("window"));
        break;
    }
    if (v.value("enableVsync").toBool()) {
        parts.append(tr("V-Sync"));
    }
    if (v.value("framePacing").toBool()) {
        parts.append(tr("frame pacing"));
    }
    return parts.join(QStringLiteral(" · "));
}

void ConnectionProfiles::refresh()
{
    emit profilesChanged();
}

QVariantMap ConnectionProfiles::snapshot() const
{
    QVariantMap values;
    for (const char* property : kProfileProperties) {
        const QVariant value = m_Preferences->property(property);
        if (!value.isValid()) {
            continue;
        }
        // Store enums as integers so the JSON stays readable and stable.
        bool isInt = false;
        const int asInt = value.toInt(&isInt);
        if (value.typeId() == QMetaType::Bool) {
            values.insert(property, value.toBool());
        }
        else if (isInt) {
            values.insert(property, asInt);
        }
        else {
            values.insert(property, value);
        }
    }
    return values;
}

void ConnectionProfiles::load()
{
    QSettings settings;
    const QJsonArray array = QJsonDocument::fromJson(settings.value(kSettingsKey).toByteArray()).array();
    m_Profiles.clear();
    for (const QJsonValue& entry : array) {
        const QJsonObject object = entry.toObject();
        const QString name = object.value("name").toString().trimmed();
        if (name.isEmpty() || indexOf(name) >= 0) {
            continue;
        }
        m_Profiles.append({ name, object.value("values").toObject().toVariantMap() });
    }
}

void ConnectionProfiles::store() const
{
    QJsonArray array;
    for (const auto& profile : m_Profiles) {
        QJsonObject object;
        object.insert("name", profile.first);
        object.insert("values", QJsonObject::fromVariantMap(profile.second));
        array.append(object);
    }
    QSettings settings;
    settings.setValue(kSettingsKey, QJsonDocument(array).toJson(QJsonDocument::Compact));
}

int ConnectionProfiles::indexOf(const QString& name) const
{
    for (int i = 0; i < m_Profiles.size(); i++) {
        if (m_Profiles.at(i).first.compare(name, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}
