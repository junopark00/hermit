#pragma once

#include <QTranslator>

// Layers Hermit's translation files: hermit_<lang>.ts (strings Hermit added or reworded) is
// consulted first, then the language's qml_<lang>.ts.
class BrandingTranslator : public QTranslator
{
public:
    // Takes ownership of both translators; either may be null (English, or no Hermit file).
    explicit BrandingTranslator(QTranslator* language, QTranslator* hermit = nullptr);
    ~BrandingTranslator() override;

    bool isEmpty() const override;
    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation = nullptr, int n = -1) const override;

private:
    QTranslator* m_Language;
    QTranslator* m_Hermit;
};
