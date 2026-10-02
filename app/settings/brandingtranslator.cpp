#include "brandingtranslator.h"

BrandingTranslator::BrandingTranslator(QTranslator* language, QTranslator* hermit)
    : m_Language(language),
      m_Hermit(hermit)
{
}

BrandingTranslator::~BrandingTranslator()
{
    delete m_Hermit;
    delete m_Language;
}

bool BrandingTranslator::isEmpty() const
{
    return false;
}

QString BrandingTranslator::translate(const char* context, const char* sourceText,
                                      const char* disambiguation, int n) const
{
    QString text;
    if (m_Hermit != nullptr) {
        text = m_Hermit->translate(context, sourceText, disambiguation, n);
    }
    if (text.isEmpty() && m_Language != nullptr) {
        text = m_Language->translate(context, sourceText, disambiguation, n);
    }
    // An empty result lets Qt fall back to the source text.
    return text;
}
