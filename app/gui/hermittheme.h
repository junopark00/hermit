#pragma once

#include <QColor>
#include <QObject>
#include <QString>

class QGuiApplication;

// Hermit's look: a neutral, enterprise-style dark theme (IBM Carbon grey scale with a single
// teal accent) and the IBM Plex type family.
//
// apply() must run after the QGuiApplication exists and before the QML engine loads, so the
// Material style picks up the colours (through its environment variables) and the font.
// The same tokens are exposed to QML as the HermitTheme singleton for custom components.
class HermitTheme : public QObject
{
    Q_OBJECT

    // Layers, from the window background upwards
    Q_PROPERTY(QColor background MEMBER m_Background CONSTANT)
    Q_PROPERTY(QColor layer1 MEMBER m_Layer1 CONSTANT)
    Q_PROPERTY(QColor layer2 MEMBER m_Layer2 CONSTANT)
    Q_PROPERTY(QColor layerHover MEMBER m_LayerHover CONSTANT)
    Q_PROPERTY(QColor borderSubtle MEMBER m_BorderSubtle CONSTANT)
    Q_PROPERTY(QColor borderStrong MEMBER m_BorderStrong CONSTANT)

    // Text
    Q_PROPERTY(QColor textPrimary MEMBER m_TextPrimary CONSTANT)
    Q_PROPERTY(QColor textSecondary MEMBER m_TextSecondary CONSTANT)
    Q_PROPERTY(QColor textHelper MEMBER m_TextHelper CONSTANT)

    // The one accent colour (interactive elements), and status colours (status only)
    Q_PROPERTY(QColor accent MEMBER m_Accent CONSTANT)
    Q_PROPERTY(QColor accentFill MEMBER m_AccentFill CONSTANT)
    Q_PROPERTY(QColor success MEMBER m_Success CONSTANT)
    Q_PROPERTY(QColor warning MEMBER m_Warning CONSTANT)
    Q_PROPERTY(QColor danger MEMBER m_Danger CONSTANT)
    Q_PROPERTY(QColor info MEMBER m_Info CONSTANT)

    Q_PROPERTY(int radius MEMBER m_Radius CONSTANT)

    // Type: UI text (numbers use its tabular figures rather than a monospace font) and
    // small labels or table headers
    Q_PROPERTY(QString fontFamily MEMBER m_FontFamily CONSTANT)
    Q_PROPERTY(QString labelFamily MEMBER m_LabelFamily CONSTANT)

public:
    explicit HermitTheme(QObject* parent = nullptr);

    static void apply(QGuiApplication& app);

    // Font files for SDL overlays drawn over the stream (Hangul-capable), or empty: the
    // semi-bold weight for messages and values, the regular weight for overlay labels.
    static QByteArray overlayFontData();
    static QByteArray overlayLabelFontData();

private:
    QColor m_Background { 0x16, 0x16, 0x16 };
    QColor m_Layer1 { 0x26, 0x26, 0x26 };
    QColor m_Layer2 { 0x39, 0x39, 0x39 };
    QColor m_LayerHover { 0x33, 0x33, 0x33 };
    QColor m_BorderSubtle { 0x39, 0x39, 0x39 };
    QColor m_BorderStrong { 0x6F, 0x6F, 0x6F };

    QColor m_TextPrimary { 0xF4, 0xF4, 0xF4 };
    QColor m_TextSecondary { 0xC6, 0xC6, 0xC6 };
    QColor m_TextHelper { 0xA8, 0xA8, 0xA8 };

    QColor m_Accent { 0x08, 0xBD, 0xBA };     // teal 40: text/indicators on dark
    QColor m_AccentFill { 0x00, 0x7D, 0x79 }; // teal 60: filled buttons with white text
    QColor m_Success { 0x42, 0xBE, 0x65 };
    QColor m_Warning { 0xF1, 0xC2, 0x1B };
    QColor m_Danger { 0xFA, 0x4D, 0x56 };
    QColor m_Info { 0x78, 0xA9, 0xFF };

    int m_Radius = 2;

    QString m_FontFamily;
    QString m_LabelFamily;
};
