#include "computermodel.h"

#include <QDesktopServices>
#include <QHostAddress>
#include <QHostInfo>
#include <QThreadPool>
#include <QUrl>

ComputerModel::ComputerModel(QObject* object)
    : QAbstractListModel(object) {}

void ComputerModel::initialize(ComputerManager* computerManager)
{
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &ComputerModel::handleComputerStateChanged);
    connect(m_ComputerManager, &ComputerManager::pairingCompleted,
            this, &ComputerModel::handlePairingCompleted);

    m_Computers = m_ComputerManager->getComputers();
}

QVariant ComputerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    Q_ASSERT(index.row() < m_Computers.count());

    NvComputer* computer = m_Computers[index.row()];
    QReadLocker lock(&computer->lock);

    switch (role) {
    case NameRole:
        return computer->name;
    case OnlineRole:
        return computer->state == NvComputer::CS_ONLINE;
    case PairedRole:
        return computer->pairState == NvComputer::PS_PAIRED;
    case BusyRole:
        return computer->currentGameId != 0;
    case WakeableRole:
        return !computer->macAddress.isEmpty();
    case StatusUnknownRole:
        return computer->state == NvComputer::CS_UNKNOWN;
    case DetailsRole: {
        QString state, pairState;

        switch (computer->state) {
        case NvComputer::CS_ONLINE:
            state = tr("Online");
            break;
        case NvComputer::CS_OFFLINE:
            state = tr("Offline");
            break;
        default:
            state = tr("Unknown");
            break;
        }

        switch (computer->pairState) {
        case NvComputer::PS_PAIRED:
            pairState = tr("Paired");
            break;
        case NvComputer::PS_NOT_PAIRED:
            pairState = tr("Unpaired");
            break;
        default:
            pairState = tr("Unknown");
            break;
        }

        return tr("Name: %1").arg(computer->name) + '\n' +
               tr("Status: %1").arg(state) + '\n' +
               tr("Active Address: %1").arg(computer->activeAddress.toString()) + '\n' +
               tr("UUID: %1").arg(computer->uuid) + '\n' +
               tr("Local Address: %1").arg(computer->localAddress.toString()) + '\n' +
               tr("Remote Address: %1").arg(computer->remoteAddress.toString()) + '\n' +
               tr("IPv6 Address: %1").arg(computer->ipv6Address.toString()) + '\n' +
               tr("Manual Address: %1").arg(computer->manualAddress.toString()) + '\n' +
               tr("MAC Address: %1").arg(computer->macAddress.isEmpty() ? tr("Unknown") : QString(computer->macAddress.toHex(':'))) + '\n' +
               tr("Pair State: %1").arg(pairState) + '\n' +
               tr("Running Game ID: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->currentGameId) : tr("Unknown")) + '\n' +
               tr("HTTPS Port: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->activeHttpsPort) : tr("Unknown"));
    }
    default:
        return QVariant();
    }
}

int ComputerModel::rowCount(const QModelIndex& parent) const
{
    // We should not return a count for valid index values,
    // only the parent (which will not have a "valid" index).
    if (parent.isValid()) {
        return 0;
    }

    return m_Computers.count();
}

QHash<int, QByteArray> ComputerModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[OnlineRole] = "online";
    names[PairedRole] = "paired";
    names[BusyRole] = "busy";
    names[WakeableRole] = "wakeable";
    names[StatusUnknownRole] = "statusUnknown";
    names[DetailsRole] = "details";

    return names;
}

Session* ComputerModel::createSessionForCurrentGame(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    NvComputer* computer = m_Computers[computerIndex];

    // We must currently be streaming a game to use this function
    Q_ASSERT(computer->currentGameId != 0);

    for (NvApp& app : computer->appList) {
        if (app.id == computer->currentGameId) {
            return new Session(computer, app);
        }
    }

    // We have a current running app but it's not in our app list
    Q_ASSERT(false);
    return nullptr;
}

void ComputerModel::deleteComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    beginRemoveRows(QModelIndex(), computerIndex, computerIndex);

    // m_Computer[computerIndex] will be deleted by this call
    m_ComputerManager->deleteHost(m_Computers[computerIndex]);

    // Remove the now invalid item
    m_Computers.removeAt(computerIndex);

    endRemoveRows();
}

class DeferredWakeHostTask : public QRunnable
{
public:
    DeferredWakeHostTask(NvComputer* computer)
        : m_Computer(computer) {}

    void run()
    {
        m_Computer->wake();
    }

private:
    NvComputer* m_Computer;
};

void ComputerModel::wakeComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    DeferredWakeHostTask* wakeTask = new DeferredWakeHostTask(m_Computers[computerIndex]);
    QThreadPool::globalInstance()->start(wakeTask);
}

void ComputerModel::renameComputer(int computerIndex, QString name)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    m_ComputerManager->renameHost(m_Computers[computerIndex], name);
}

QString ComputerModel::generatePinString()
{
    return m_ComputerManager->generatePinString();
}

class DeferredPowerTask : public QObject, public QRunnable
{
    Q_OBJECT
public:
    // action: "query", "shutdown" or "restart"
    DeferredPowerTask(NvComputer* computer, QString uuid, QString action, bool force)
        : m_Computer(computer), m_Uuid(uuid), m_Action(action), m_Force(force) {}

    void run()
    {
        bool allowed = false;
        QStringList clients;
        QString error;
        try {
            NvHTTP http(m_Computer);
            QByteArray body = http.powerRequest(m_Action, m_Force, 8000);
            bool supported = false;
            for (const QByteArray& line : body.split('\n')) {
                if (line.startsWith("supported=")) {
                    supported = line.mid(10).trimmed() == "1";
                }
                else if (line.startsWith("allowed=")) {
                    allowed = line.mid(8).trimmed() == "1";
                }
                else if (line.startsWith("client=")) {
                    clients.append(QString::fromUtf8(line.mid(7)).trimmed());
                }
            }
            // A host that answers unknown requests with 200 does nothing
            if (isAction() ? !body.startsWith("ok") : !supported) {
                error = "unsupported";
            }
        }
        catch (const QtNetworkReplyException& e) {
            switch (e.getError()) {
            case QNetworkReply::ContentAccessDenied:
            case QNetworkReply::AuthenticationRequiredError:
                error = "permission";
                break;
            case QNetworkReply::ContentNotFoundError:
            case QNetworkReply::OperationNotImplementedError:
                error = "unsupported";
                break;
            default:
                error = QString("network:") + e.getErrorText();
                break;
            }
        }
        catch (const HostHttpResponseException& e) {
            error = QString("network:") + e.getStatusMessage();
        }

        if (isAction()) {
            emit powerActionCompleted(m_Uuid, error);
        }
        else {
            emit powerQueryCompleted(m_Uuid, allowed, clients, error);
        }
    }

signals:
    void powerQueryCompleted(QString uuid, bool allowed, QStringList clients, QString error);
    void powerActionCompleted(QString uuid, QString error);

private:
    bool isAction() const { return m_Action != "query"; }

    NvComputer* m_Computer;
    QString m_Uuid;
    QString m_Action;
    bool m_Force;
};

QString ComputerModel::computerUuid(int computerIndex) const
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return QString();
    }
    NvComputer* computer = m_Computers[computerIndex];
    QReadLocker lock(&computer->lock);
    return computer->uuid;
}

NvComputer* ComputerModel::findComputer(const QString& uuid) const
{
    for (NvComputer* computer : m_Computers) {
        QReadLocker lock(&computer->lock);
        if (!uuid.isEmpty() && computer->uuid == uuid) {
            return computer;
        }
    }
    return nullptr;
}

void ComputerModel::queryPower(QString uuid)
{
    NvComputer* computer = findComputer(uuid);
    if (computer == nullptr) {
        emit powerQueryCompleted(uuid, false, QStringList(), "gone");
        return;
    }

    auto* task = new DeferredPowerTask(computer, uuid, "query", false);
    QObject::connect(task, &DeferredPowerTask::powerQueryCompleted,
                     this, &ComputerModel::powerQueryCompleted);
    QThreadPool::globalInstance()->start(task);
}

void ComputerModel::powerComputer(QString uuid, bool restart, bool force)
{
    NvComputer* computer = findComputer(uuid);
    if (computer == nullptr) {
        emit powerActionCompleted(uuid, "gone");
        return;
    }

    auto* task = new DeferredPowerTask(computer, uuid, restart ? "restart" : "shutdown", force);
    QObject::connect(task, &DeferredPowerTask::powerActionCompleted,
                     this, &ComputerModel::powerActionCompleted);
    QThreadPool::globalInstance()->start(task);
}

void ComputerModel::pairComputer(int computerIndex, QString pin)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    // Hermit: a newer attempt supersedes the previous one (its result is ignored)
    m_PairingAttempt = m_ComputerManager->pairHost(m_Computers[computerIndex], pin);
}

void ComputerModel::cancelPairing()
{
    if (m_PairingAttempt != 0) {
        m_ComputerManager->cancelPairing(m_PairingAttempt);
        m_PairingAttempt = 0;
    }
}

bool ComputerModel::openPairingPage(QString uuid, QString pin)
{
    NvComputer* computer = findComputer(uuid);
    if (computer == nullptr) {
        qWarning() << "Pairing page: PC" << uuid << "is no longer in the list";
        return false;
    }

    QString host;
    uint16_t webUiPort;
    {
        QReadLocker lock(&computer->lock);
        NvAddress address = computer->activeAddress;
        if (address.isNull()) {
            qWarning() << "Pairing page: no active address for" << computer->name;
            return false;
        }

        // IPv6 literals go in brackets, as NvAddress::toString() does, without a zone id
        // ("fe80::1%eth0"), which a URL cannot carry
        host = address.address();
        if (QHostAddress(host).protocol() == QAbstractSocket::IPv6Protocol) {
            host = "[" + host.section('%', 0, 0) + "]";
        }

        // Sunshine convention: HTTPS = base - 5, HTTP = base, web UI = base + 1. The active
        // address carries the HTTP (base) port; fall back to the default when it is unknown.
        webUiPort = address.port() != 0 ? address.port() + 1 : DEFAULT_HTTP_PORT + 1;
    }

    // The name Shell shows for this device. The pairing request itself carries the upstream
    // "roth" placeholder, so the host takes the name from the web UI form.
    QString deviceName = QHostInfo::localHostName();
    if (deviceName.isEmpty()) {
        deviceName = "Hermit";
    }

    // The PIN and name stay in the fragment: browsers never send it to the server
    QUrl url(QString("https://%1:%2/pin").arg(host).arg(webUiPort));
    url.setFragment("pin=" + pin + "&name=" + QString::fromLatin1(QUrl::toPercentEncoding(deviceName)),
                    QUrl::TolerantMode);

    if (!QDesktopServices::openUrl(url)) {
        qWarning() << "Pairing page: couldn't open" << url.toString(QUrl::RemoveFragment);
        return false;
    }
    return true;
}

void ComputerModel::handlePairingCompleted(NvComputer*, QString error, int attempt)
{
    // Hermit: a cancelled or superseded attempt must not close the dialog of a newer one or
    // show its error (the aborted request, or the host's "Superseded")
    if (attempt == 0 || attempt != m_PairingAttempt) {
        qInfo() << "Ignoring the result of superseded pairing attempt" << attempt << ":" << error;
        return;
    }
    m_PairingAttempt = 0;

    emit pairingCompleted(error.isEmpty() ? QVariant() : error);
}

void ComputerModel::handleComputerStateChanged(NvComputer* computer)
{
    QVector<NvComputer*> newComputerList = m_ComputerManager->getComputers();

    // Reset the model if the structural layout of the list has changed
    if (m_Computers != newComputerList) {
        beginResetModel();
        m_Computers = newComputerList;
        endResetModel();
    }
    else {
        // Let the view know that this specific computer changed
        int index = m_Computers.indexOf(computer);
        emit dataChanged(createIndex(index, 0), createIndex(index, 0));
    }
}

#include "computermodel.moc"
