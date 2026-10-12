#pragma once

#include "identitymanager.h"
#include "nvhttp.h"

#include <openssl/x509.h>
#include <openssl/evp.h>

class NvPairingManager
{
public:
    enum PairState
    {
        PAIRED,
        PIN_WRONG,
        FAILED,
        ALREADY_IN_PROGRESS,
        // Hermit: the wait for the PIN ended (just under 5 minutes, on every host). The host may
        // still hold the session (Shell ends it a few seconds later): call abandonPairing().
        PIN_NOT_ENTERED,
        // Hermit, QR pairing only: the host's certificate is not the one the QR link named
        // (SHA-256 of its DER differs from fp). Nothing was pinned; the host was asked to drop the
        // session.
        CERT_MISMATCH,
        // Hermit, QR pairing only: getservercert got no answer within 10 s (a host that answers
        // at once was expected). The host was asked to drop the session.
        QR_NO_ANSWER
    };

    // Hermit: what a QR pairing link adds to pair() (notes/DESIGN-I-qr-pairing.md, 4.3). The
    // token itself is the pin argument.
    struct QrPairing
    {
        QString id;           // the record id the host looks up: qr=<id> on getservercert
        QString fingerprint;  // expected SHA-256 of the host certificate's DER, lowercase hex
        QString deviceName;   // sent as devicename= instead of the "roth" placeholder
    };

    explicit NvPairingManager(NvComputer* computer);

    ~NvPairingManager();

    // Hermit: with qr, getservercert carries qr=<id> and the device name, waits 10 s instead of
    // the PIN window, and the host certificate must match qr->fingerprint before it is pinned
    PairState
    pair(QString appVersion, QString pin, QSslCertificate& serverCert, const QrPairing* qr = nullptr);

    // Hermit: once cancelled() returns true, the pending request is aborted and pair() throws
    // QtNetworkReplyException(OperationCanceledError)
    void
    setCancelCheck(std::function<bool()> cancelled);

    // Hermit: after a cancelled pair() or PIN_NOT_ENTERED: tells the host to drop the unfinished pairing session
    // (no longer cancellable, short timeout), so a PIN entered on the host later is not handed
    // to it. Failures are only logged.
    void
    abandonPairing();

private:
    // Hermit: tells the host to drop the unfinished pairing session; failures are only logged
    void
    cleanupPairing();

    void
    sendUnpair(int timeoutMs);

    QByteArray
    generateRandomBytes(int length);

    QByteArray
    saltPin(const QByteArray& salt, QString pin);

    QByteArray
    encrypt(const QByteArray& plaintext, const QByteArray& key);

    QByteArray
    decrypt(const QByteArray& ciphertext, const QByteArray& key);

    QByteArray
    getSignatureFromCert(X509* cert);

    QByteArray
    getSignatureFromPemCert(const QByteArray& certificate);

    bool
    verifySignature(const QByteArray& data, const QByteArray& signature, const QByteArray& serverCertificate);

    QByteArray
    signMessage(const QByteArray& message);

    NvHTTP m_Http;
    X509* m_Cert;
    EVP_PKEY* m_PrivateKey;
};
