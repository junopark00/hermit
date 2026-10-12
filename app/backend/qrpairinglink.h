#pragma once

// Hermit: the QR pairing link a Shell host's web UI shows (notes/DESIGN-I-qr-pairing.md, section
// 3 and 6.1). Header-only and Qt Core only, so hermit/tests/hermit_core_test.cpp can build it.
//
//   hermit://pair?v=1&host=<host>&port=<HTTP port>&fp=<64 hex>&token=<26 base32>&name=<name>
//
// The token is the 128-bit one-time secret that takes the PIN's place in the pairing key
// derivation; it never goes on the wire. The host identifies the pending QR record by
// id = SHA-256("hermit-qr-id" || token ASCII), first 16 hex characters, which both sides compute.

#include <QByteArray>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

struct QrPairingLink
{
    enum Error
    {
        NoError,
        NotPairingLink,      // not "hermit://pair?..."
        UnsupportedVersion,  // v is not 1
        Invalid              // a required key is missing or malformed
    };

    QString host;         // IPv4, IPv6 without brackets, or a host name; never a zone id
    uint16_t port = 0;    // the host's HTTP (nvhttp) port
    QString fingerprint;  // SHA-256 of the host certificate's DER, 64 lowercase hex characters
    QString token;        // 26 base32 characters, uppercase
    QString id;           // tokenId(token)
    QString name;         // the host's name, for display only (may be empty)

    // True when text (trimmed) starts with hermit://pair? (the scheme in any case). Decides whether
    // Add PC input takes the QR path before the normal address parsing prefixes "hermit://" itself.
    static bool looksLikeLink(const QString& text)
    {
        return hasPairPrefix(text.trimmed());
    }

    static QString tokenId(const QString& token)
    {
        return QCryptographicHash::hash(QByteArray("hermit-qr-id") + token.toLatin1(),
                                        QCryptographicHash::Sha256).toHex().left(16);
    }

    // Parses and validates text; on failure returns a default link and sets *error.
    static QrPairingLink parse(const QString& text, Error* error = nullptr)
    {
        QrPairingLink link;
        const Error result = parseInto(text.trimmed(), link);
        if (error != nullptr) {
            *error = result;
        }
        return result == NoError ? link : QrPairingLink();
    }

private:
    // "hermit://" in any case, then exactly "pair?"
    static bool hasPairPrefix(const QString& text)
    {
        return text.left(9).compare(QLatin1String("hermit://"), Qt::CaseInsensitive) == 0 &&
               text.mid(9).startsWith(QLatin1String("pair?"));
    }

    static Error parseInto(const QString& text, QrPairingLink& link)
    {
        // Scheme (any case) and path, then the query. Not QUrl::fromUserInput(): the query is RFC
        // 3986 percent-encoded, so "+" stays "+" (base32 has none, names may).
        if (!hasPairPrefix(text)) {
            return NotPairingLink;
        }
        const int query = text.indexOf('?');

        QString v, port, fp, token;
        bool haveV = false, havePort = false, haveFp = false, haveToken = false;
        bool haveHost = false, haveName = false;
        const QStringList pairs = text.mid(query + 1).split('&', Qt::SkipEmptyParts);
        for (const QString& pair : pairs) {
            const int eq = pair.indexOf('=');
            const QString key = eq < 0 ? pair : pair.left(eq);
            const QString value = eq < 0 ? QString() : QUrl::fromPercentEncoding(pair.mid(eq + 1).toUtf8());
            // The first occurrence of a key counts; unknown keys are for later versions
            if (key == "v" && !haveV) {
                v = value; haveV = true;
            } else if (key == "host" && !haveHost) {
                link.host = value; haveHost = true;
            } else if (key == "port" && !havePort) {
                port = value; havePort = true;
            } else if (key == "fp" && !haveFp) {
                fp = value; haveFp = true;
            } else if (key == "token" && !haveToken) {
                token = value; haveToken = true;
            } else if (key == "name" && !haveName) {
                link.name = value.trimmed(); haveName = true;
            }
        }

        if (!haveV || !haveHost || !havePort || !haveFp || !haveToken) {
            return Invalid;
        }
        if (v != "1") {
            return UnsupportedVersion;
        }

        // host: not empty, no zone id ("fe80::1%eth0"), brackets around an IPv6 literal removed
        if (link.host.startsWith('[') && link.host.endsWith(']')) {
            link.host = link.host.mid(1, link.host.length() - 2);
        }
        if (link.host.isEmpty() || link.host.contains('%') || link.host.contains('/') ||
                link.host.contains(QRegularExpression("\\s"))) {
            return Invalid;
        }

        static const QRegularExpression digits("^[0-9]{1,5}$");
        if (!digits.match(port).hasMatch() || port.toInt() < 1 || port.toInt() > 65535) {
            return Invalid;
        }
        link.port = static_cast<uint16_t>(port.toInt());

        static const QRegularExpression hex64("^[0-9a-f]{64}$");
        link.fingerprint = fp.toLower();
        if (!hex64.match(link.fingerprint).hasMatch()) {
            return Invalid;
        }

        static const QRegularExpression base32x26("^[A-Z2-7]{26}$");
        link.token = token.toUpper();
        if (!base32x26.match(link.token).hasMatch()) {
            return Invalid;
        }

        link.id = tokenId(link.token);
        return NoError;
    }
};
