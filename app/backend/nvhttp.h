#pragma once

#include "identitymanager.h"
#include "nvapp.h"
#include "nvaddress.h"

#include <Limelight.h>

#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class NvComputer;

class NvDisplayMode
{
public:
    bool operator==(const NvDisplayMode& other) const
    {
        return width == other.width &&
                height == other.height &&
                refreshRate == other.refreshRate;
    }

    int width;
    int height;
    int refreshRate;
};
Q_DECLARE_TYPEINFO(NvDisplayMode, Q_PRIMITIVE_TYPE);

class HostHttpResponseException : public std::exception
{
public:
    HostHttpResponseException(int statusCode, QString message) :
        m_StatusCode(statusCode),
        m_StatusMessage(message.toUtf8())
    {

    }

    const char* what() const throw()
    {
        return m_StatusMessage.constData();
    }

    const char* getStatusMessage() const
    {
        return m_StatusMessage.constData();
    }

    int getStatusCode() const
    {
        return m_StatusCode;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_StatusMessage) + " (Error " + QString::number(m_StatusCode) + ")";
    }

private:
    int m_StatusCode;
    QByteArray m_StatusMessage;
};

class QtNetworkReplyException : public std::exception
{
public:
    // Hermit: httpStatus is the HTTP status code of the reply, 0 if there was none (Qt maps
    // several codes, 413 and 422 for example, to the same NetworkError); body is the start of the
    // reply's body, where the host may say why it refused
    QtNetworkReplyException(QNetworkReply::NetworkError error, QString errorText, int httpStatus = 0,
                            QByteArray body = QByteArray()) :
        m_Error(error),
        m_ErrorText(errorText.toUtf8()),
        m_HttpStatus(httpStatus),
        m_Body(body)
    {

    }

    const char* what() const throw()
    {
        return m_ErrorText.constData();
    }

    const char* getErrorText() const
    {
        return m_ErrorText.constData();
    }

    QNetworkReply::NetworkError getError() const
    {
        return m_Error;
    }

    int getHttpStatus() const
    {
        return m_HttpStatus;
    }

    const QByteArray& getBody() const
    {
        return m_Body;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_ErrorText) + " (Error " + QString::number(m_Error) + ")";
    }

private:
    QNetworkReply::NetworkError m_Error;
    QByteArray m_ErrorText;
    int m_HttpStatus;
    QByteArray m_Body;
};

class NvHTTP : public QObject
{
    Q_OBJECT

public:
    enum NvLogLevel {
        NVLL_NONE,
        NVLL_ERROR,
        NVLL_VERBOSE
    };

    explicit NvHTTP(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert, bool useTrueUid, QNetworkAccessManager* nam = nullptr);

    explicit NvHTTP(NvComputer* computer, QNetworkAccessManager* nam = nullptr);

    static
    int
    getCurrentGame(QString serverInfo);

    QString
    getServerInfo(NvLogLevel logLevel, bool fastFail = false);

    static
    void
    verifyResponseStatus(QString xml);

    static
    QString
    getXmlString(QString xml,
                 QString tagName);

    static
    QByteArray
    getXmlStringFromHex(QString xml,
                        QString tagName);

    QString
    openConnectionToString(QUrl baseUrl,
                           QString command,
                           QString arguments,
                           int timeoutMs,
                           NvLogLevel logLevel = NvLogLevel::NVLL_VERBOSE);

    void setServerCert(QSslCertificate serverCert);
    void setAddress(NvAddress address);
    void setHttpsPort(uint16_t port);
    void setTrueUid(bool useTrueUid);

    NvAddress address();

    QSslCertificate serverCert();

    uint16_t httpPort();

    uint16_t httpsPort();

    static
    QVector<int>
    parseQuad(QString quad);

    void
    quitApp();

    void
    startApp(QString verb,
             bool isNvidiaServerSoftware,
             int appId,
             PSTREAM_CONFIGURATION streamConfig,
             bool sops,
             bool localAudio,
             int gamepadMask,
             bool persistGameControllersOnDisconnect,
             QString& rtspSessionUrl);

    QVector<NvApp>
    getAppList();

    QImage
    getBoxArt(int appId);

    // Shell host extension: GET (postBody == nullptr) or POST /actions/clipboard?type=<type>.
    // The host only accepts these while this client has an active stream. Returns the raw
    // response body. Throws on HTTP or network errors.
    QByteArray
    clipboardRequest(const QString& type, const QByteArray* postBody, int timeoutMs);

    // Streaming variant for large clipboard transfers: starts a GET (body == nullptr) or a POST
    // of bodySize bytes read from body, which may be sequential and produce its data gradually,
    // and returns the reply at once. The caller runs the event loop, reads the reply
    // incrementally, applies its own (inactivity) timeout, checks reply->error() when it
    // finishes and deletes the reply. body must stay open until then.
    QNetworkReply*
    startClipboardRequest(const QString& type, QIODevice* body, qint64 bodySize);

    // Shell host extension: GET /actions/bitrate?kbps=<kbps> changes the bitrate of this client's
    // running stream without restarting it. Returns the host's encoding bitrate in kbps, or -1 if the
    // reply has no "bitrate=" line (nothing changed). Throws on HTTP or network errors (404 or 501:
    // not supported).
    int
    setLiveBitrate(int kbps, int timeoutMs);

    // Shell host extension: GET /actions/power. action "query" answers "allowed=0|1" and the
    // other clients streaming right now ("streaming=<n>", "client=<name>" lines); "shutdown"
    // turns the host PC off and "restart" restarts it (force: also close apps that hold it back).
    // Returns the reply body.
    // Throws on HTTP or network errors (404: not supported, 401: no permission).
    QByteArray
    powerRequest(const QString& action, bool force, int timeoutMs);

    static
    QVector<NvDisplayMode>
    getDisplayModeList(QString serverInfo);

    QUrl m_BaseUrlHttp;
    QUrl m_BaseUrlHttps;
private:
    void
    handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors);

    QNetworkRequest
    buildRequest(QUrl baseUrl,
                 QString command,
                 QString arguments);

    QNetworkReply*
    openConnection(QUrl baseUrl,
                   QString command,
                   QString arguments,
                   int timeoutMs,
                   NvLogLevel logLevel,
                   const QByteArray* postBody = nullptr);

    NvAddress m_Address;
    QNetworkAccessManager* m_Nam;
    QSslCertificate m_ServerCert;
    bool m_UseTrueUid;
};
