#include "hermittheme.h"

#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QtDebug>

#include <initializer_list>

namespace {

QString s_UiFamily;
QString s_LabelFamily;

QString loadFamily(std::initializer_list<const char*> files)
{
    QString family;
    for (const char* file : files) {
        const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(file));
        if (id < 0) {
            qWarning() << "Failed to load font" << file;
            continue;
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (family.isEmpty() && !families.isEmpty()) {
            family = families.first();
        }
    }
    return family;
}

}

HermitTheme::HermitTheme(QObject* parent)
    : QObject(parent),
      m_FontFamily(s_UiFamily),
      m_LabelFamily(s_LabelFamily)
{
}

void HermitTheme::apply(QGuiApplication& app)
{
    // Material reads these when the style initializes; users can still override them with
    // their own environment variables, as with upstream's defaults.
    auto setDefault = [](const char* name, const char* value) {
        if (!qEnvironmentVariableIsSet(name)) {
            qputenv(name, value);
        }
    };
    setDefault("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "#08BDBA");
    setDefault("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", "#262626");
    setDefault("QT_QUICK_CONTROLS_MATERIAL_BACKGROUND", "#161616");
    setDefault("QT_QUICK_CONTROLS_MATERIAL_FOREGROUND", "#F4F4F4");

    s_UiFamily = loadFamily({ ":/res/fonts/IBMPlexSansKR-Regular.ttf", ":/res/fonts/IBMPlexSansKR-SemiBold.ttf" });
    s_LabelFamily = loadFamily({ ":/res/fonts/IBMPlexSansCondensed-Medium.ttf", ":/res/fonts/IBMPlexSansCondensed-SemiBold.ttf" });

    if (!s_UiFamily.isEmpty()) {
        QFont font = app.font();
        font.setFamilies({ s_UiFamily, QStringLiteral("Malgun Gothic") });
        app.setFont(font);
    }
}

QByteArray HermitTheme::overlayLabelFontData()
{
    QFile file(QStringLiteral(":/res/fonts/IBMPlexSansKR-Regular.ttf"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QByteArray HermitTheme::overlayFontData()
{
    QFile file(QStringLiteral(":/res/fonts/IBMPlexSansKR-SemiBold.ttf"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
