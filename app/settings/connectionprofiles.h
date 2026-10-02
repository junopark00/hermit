#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

class StreamingPreferences;

// Named snapshots of the stream settings (resolution, frame rate, bitrate, window mode, V-Sync,
// frame pacing, codec, ...) that can be applied in one step, for example "1440p
// windowed" and "Full screen". Stored as JSON in the Hermit settings.
class ConnectionProfiles : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QStringList names READ names NOTIFY profilesChanged)
    // Name of the profile whose values equal the current settings, or empty.
    Q_PROPERTY(QString current READ current NOTIFY profilesChanged)

public:
    explicit ConnectionProfiles(StreamingPreferences* preferences, QObject* parent = nullptr);

    QStringList names() const;
    QString current() const;

    // Saves the current settings under this name, replacing a profile with the same name.
    Q_INVOKABLE bool saveCurrent(const QString& name);
    // Applies a profile to the settings and saves them. Returns false if it does not exist.
    Q_INVOKABLE bool apply(const QString& name);
    Q_INVOKABLE void remove(const QString& name);
    // Short description such as "2560x1440 · 60 FPS · 50 Mbps · window".
    Q_INVOKABLE QString describe(const QString& name) const;

    // Re-evaluates current() after settings were changed elsewhere.
    Q_INVOKABLE void refresh();

signals:
    void profilesChanged();

private:
    QVariantMap snapshot() const;
    void load();
    void store() const;
    int indexOf(const QString& name) const;

    StreamingPreferences* m_Preferences;
    QList<QPair<QString, QVariantMap>> m_Profiles;
};
