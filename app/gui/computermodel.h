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
        DetailsRole
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

    // Hermit: opens the host's web UI pairing page (Shell: https://<address>:<web UI port>/pin) in
    // the default browser with the PIN and this PC's name in the URL fragment, which the browser
    // keeps to itself. The PC is named by its uuid (computerUuid()) like the power actions.
    // False when the page could not be opened (no browser, or the PC has no address).
    Q_INVOKABLE bool openPairingPage(QString uuid, QString pin);

    Q_INVOKABLE void wakeComputer(int computerIndex);

    Q_INVOKABLE void renameComputer(int computerIndex, QString name);

    Q_INVOKABLE Session* createSessionForCurrentGame(int computerIndex);

    // Hermit: turning the host PC off or restarting it (Shell). The PC is named by its uuid
    // (computerUuid()), since the list can be re-sorted while a dialog is open. queryPower() asks
    // whether this client may and who else is streaming (powerQueryCompleted); powerComputer()
    // turns it off or restarts it (powerActionCompleted). error is empty, "permission",
    // "unsupported", "gone" (no longer in the list) or "network:<details>".
    Q_INVOKABLE QString computerUuid(int computerIndex) const;
    Q_INVOKABLE void queryPower(QString uuid);
    Q_INVOKABLE void powerComputer(QString uuid, bool restart, bool force);

signals:
    void pairingCompleted(QVariant error);
    void powerQueryCompleted(QString uuid, bool allowed, QStringList clients, QString error);
    void powerActionCompleted(QString uuid, QString error);

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handlePairingCompleted(NvComputer* computer, QString error);

private:
    NvComputer* findComputer(const QString& uuid) const;

    QVector<NvComputer*> m_Computers;
    ComputerManager* m_ComputerManager;
};
