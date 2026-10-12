#include "backend/computermanager.h"
#include "streaming/session.h"

#include <QAbstractListModel>

class ComputerModel : public QAbstractListModel
{
    Q_OBJECT

    enum Roles
    {
        NameRole = Qt::UserRole,
        OnlineRole,
        PairedRole,
        BusyRole,
        WakeableRole,
        StatusUnknownRole,
        DetailsRole,
        // Hermit: what the host's last power query reported (false until one answered)
        CanSleepRole,
        CanHibernateRole
    };

    // Hermit: sleep and hibernation availability a host reported (queryPower())
    struct PowerModes
    {
        bool sleep = false;
        bool hibernate = false;
    };

public:
    explicit ComputerModel(QObject* object = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager);

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void deleteComputer(int computerIndex);

    Q_INVOKABLE QString generatePinString();

    Q_INVOKABLE void pairComputer(int computerIndex, QString pin);

    // Hermit: abandons the attempt pairComputer() started (the PIN dialog was closed): its
    // pending request is aborted and pairingCompleted is not emitted for it
    Q_INVOKABLE void cancelPairing();

    // Hermit: opens the host's web UI pairing page (Shell: https://<address>:<web UI port>/pin) in
    // the default browser with the PIN and this PC's name in the URL fragment, which the browser
    // keeps to itself. The PC is named by its uuid (computerUuid()) like the power actions.
    // False when the page could not be opened (no browser, or the PC has no address).
    Q_INVOKABLE bool openPairingPage(QString uuid, QString pin);

    Q_INVOKABLE void wakeComputer(int computerIndex);

    Q_INVOKABLE void renameComputer(int computerIndex, QString name);

    Q_INVOKABLE Session* createSessionForCurrentGame(int computerIndex);

    // Hermit: turning the host PC off, restarting it, or putting it to sleep (Shell). The PC is
    // named by its uuid (computerUuid()), since the list can be re-sorted while a dialog is open.
    // queryPower() asks whether this client may, who else is streaming, which of sleep and
    // hibernation the host supports (kept as the canSleep / canHibernate roles, so the PC menu only
    // offers what works) and whether Wake-on-LAN can wake it ("yes", "no" or "unknown")
    // (powerQueryCompleted); powerComputer() runs action "shutdown", "restart", "sleep" or
    // "hibernate" (powerActionCompleted; force: also close apps that hold a shutdown back). error
    // is empty, "permission", "unsupported", "unavailable" (the host cannot sleep or hibernate
    // right now), "gone" (no longer in the list) or "network:<details>".
    Q_INVOKABLE QString computerUuid(int computerIndex) const;
    Q_INVOKABLE void queryPower(QString uuid);
    Q_INVOKABLE void powerComputer(QString uuid, QString action, bool force);

signals:
    void pairingCompleted(QVariant error);
    void powerQueryCompleted(QString uuid, bool allowed, QStringList clients, QString wol, QString error);
    void powerActionCompleted(QString uuid, QString error);

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handlePairingCompleted(NvComputer* computer, QString error, int attempt);

    void handlePowerQueryCompleted(QString uuid, bool allowed, QStringList clients,
                                   bool sleep, bool hibernate, QString wol, QString error);

private:
    NvComputer* findComputer(const QString& uuid) const;

    QVector<NvComputer*> m_Computers;
    ComputerManager* m_ComputerManager;
    int m_PairingAttempt = 0; // Hermit: the attempt pairComputer() started, 0 if none
    QHash<QString, PowerModes> m_PowerModes; // Hermit: by uuid, from the hosts' power queries
};
