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
        PIN_NOT_ENTERED  // Hermit: the host ended the wait for its PIN (5 minutes on Shell)
    };

    explicit NvPairingManager(NvComputer* computer);

    ~NvPairingManager();

    PairState
    pair(QString appVersion, QString pin, QSslCertificate& serverCert);

    // Hermit: once cancelled() returns true, the pending request is aborted and pair() throws
    // QtNetworkReplyException(OperationCanceledError)
    void
    setCancelCheck(std::function<bool()> cancelled);

    // Hermit: after a cancelled pair(): tells the host to drop the unfinished pairing session
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
