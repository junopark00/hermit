#pragma once

#include "backend/nvaddress.h"
#include "clipboardarchive.h"
#include "clipboardchangeorder.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSslCertificate>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

class NvHTTP;
class NvComputer;
class QIODevice;
class QThread;
class QTimer;
struct SDL_Window;

namespace ClipboardVirtualFiles {
class VirtualFileClipboard;
}

// Shared by the main thread (cancel shortcut) and the worker (file and image transfers).
struct ClipboardTransferControl
{
    std::atomic<bool> active {false};
    std::atomic<bool> cancel {false};
    // Why clipboard content did not move (ClipboardNotice), shown again when the user returns to
    // the stream window: host content may be fetched and local content sent while they work in
    // another app, so the first notice is easy to miss.
    std::atomic<int> pendingNotice {0};
    // Host files being downloaded while File Explorer pastes them (virtual files), and a counter
    // the cancel shortcut increments to stop those downloads.
    std::atomic<int> streams {0};
    std::atomic<int> streamCancel {0};
    // Hermit: the local changes the main thread has observed so far (ClipboardLocalChanges::count),
    // read by the worker when a host poll starts: a host change the poll finds is newer than
    // these, older than every local change observed after (ClipboardChangeOrder).
    std::atomic<uint64_t> localChanges {0};
};

// Clipboard content fetched from the host, handed to the SDL main thread in an SDL user event.
struct ClipboardHostContent
{
    // RemoteFilesSuperseded: a superseded host file list went on the clipboard over host content
    // written just before it and was emptied again, taking that content with it. Hermit:
    // LocalRetry: the local clipboard could not be read a moment ago (another program held it):
    // read again now, for the content at localSeq, on trigger localTrigger (ClipboardPushRetries)
    enum Kind { Text, Image, Files, RemoteFiles, RemoteFilesReady, RemoteFilesFailed, RemoteFilesSuperseded, LocalNotSent, LocalRetry };

    int generation = 0;
    Kind kind = Text;
    // Which host content this is (ClipboardChangeOrder key: the host clipboard sequence number it
    // was fetched at; Hermit: for text-only hosts a hash of the text), so a failed write forgets
    // only this content; RemoteFilesFailed/Superseded: that of the list
    quint64 hostKey = 0;
    // Hermit: the order of the host change this content came from (ClipboardChangeOrder): not put
    // on the local clipboard once a local change came after it
    quint64 changeOrder = 0;
    QByteArray text;    // UTF-8
    QByteArray png;     // Image: PNG bytes
    QByteArray dib;     // Image: CF_DIBV5 block (Windows)
    QStringList files;  // Files: top-level local paths in the staging folder
    ClipboardArchive::RemoteFileList remoteFiles;  // RemoteFiles: host files to offer as virtual files
    // RemoteFilesReady: local clipboard sequence number once they were offered; LocalNotSent: that
    // of the local content the host could not take yet (its clipboard was busy, or Hermit: a
    // network error, or sync could not be set up yet); LocalRetry: that of the content to read again
    quint32 localSeq = 0;
    int localTrigger = 0;  // LocalRetry: the ClipboardSync::Trigger of the push to repeat
    int itemCount = 0;     // RemoteFilesReady: items the user copied on the host
    quint64 listGeneration = 0;  // RemoteFilesReady/Failed: which publish of the virtual files it answers
};

// Runs every host request on its own thread, so the SDL streaming loop is never blocked by the
// network, image conversion or file I/O. Tracks what the host clipboard holds so content only
// moves when it actually changed.
class ClipboardSyncWorker : public QObject
{
    Q_OBJECT

public:
    ClipboardSyncWorker(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert, bool useTrueUid,
                        int sdlEventCode, int generation, bool localChangeTracking, int rateMbps,
                        std::shared_ptr<std::atomic<bool>> stopped,
                        std::shared_ptr<ClipboardTransferControl> control);

    // Runs a job now, or after the current one if it is still running. Requests wait in nested
    // event loops that also deliver queued jobs, which would otherwise start re-entrantly (for
    // example a text push in the middle of a file upload) and reorder clipboard changes.
    void run(const std::function<void()>& job);

    // Detects the host protocol (Shell extensions or the text-only clipboard endpoint) and records
    // the host's current clipboard state without transferring it. The host grants reading its
    // clipboard (GET) and setting it (POST) separately, so either direction may be refused alone.
    // Hermit: localChanges: the local changes observed when the job was posted (a push: the order
    // of its local change). While no attempt failed (at stream start) the host's content is
    // recorded as no change; after one failed, as a host change seen by the last failed attempt
    // (ClipboardChangeOrder::hostSetUp).
    void init(quint64 localChanges = 0);

    // localSeq: the local clipboard sequence number of the content (Windows), handed back in a
    // LocalNotSent event when the host's clipboard was busy (Hermit: or a network error stopped
    // it, or the host could not be reached to set up sync), so the next trigger sends it again.
    // Hermit: localOrder: its local change (ClipboardChangeOrder), not sent once a host change was
    // seen after it; 0 if not tracked.
    void pushText(const QByteArray& utf8, quint32 localSeq = 0, quint64 localOrder = 0);
    void pushImage(const QByteArray& data, bool isDib, quint32 localSeq = 0, quint64 localOrder = 0);
    // dropped: files dropped on the stream window, so every outcome is announced; always sent,
    // localOrder then being the local changes observed when they were dropped
    void pushFiles(const QStringList& paths, bool dropped = false, quint32 localSeq = 0, quint64 localOrder = 0);

    // Hermit: starts polling the host clipboard (ClipboardHostPolling) once sync is set up: a
    // Shell host's info request every second, a text-only host's text every two seconds, while
    // the stream runs. Each poll brings the host clipboard to the client if it changed since we
    // last saw or set it (pullOnce), unless a local change came after it (ClipboardChangeOrder;
    // the local changes observed when the poll starts: ClipboardTransferControl::localChanges).
    // Timed on this thread, after the job in progress like a posted one.
    void startPolling();
    // Hermit: polls now rather than at the cadence (the user left the stream window), and starts
    // a new run of quick polls should the host's clipboard be busy or still empty.
    void pollNow();

    // Host content key (ClipboardHostContent::hostKey) is no longer on the local clipboard (removed
    // with a superseded host file list): a later pull fetches it again, unless newer host content
    // was seen or sent meanwhile, or a local change came after it.
    void forgetHostContent(quint64 key);
    // Hermit: host content key could not be put on the local clipboard (another program held it):
    // fetched again like forgetHostContent, once more at most for this content; after that, not
    // until the host's clipboard changes (ClipboardHostRetries), so a write that keeps failing
    // does not fetch a large image or archive on every poll.
    void hostContentNotWritten(quint64 key);

    // Hermit: the main thread could not read the local clipboard content at localSeq (another
    // program held it): hands it a LocalRetry event after ClipboardPushRetries::k_RetryDelayMs
    // so it reads again, on trigger (a ClipboardSync::Trigger). Timed here because the main
    // thread runs no Qt timers while it streams; called with a queued invocation, not as a job,
    // so a transfer in progress does not hold it up.
    void retryLocalPush(int trigger, quint32 localSeq);

private:
    enum class Mode { Unknown, Extended, Legacy, Disabled };
    enum class Direction { Pull, Push };
    enum class TransferResult { Done, Failed, Cancelled, Stopped };

    NvHTTP* http();
    bool request(const QString& type, const QByteArray* postBody, int timeoutMs, QByteArray& body, int& qtError);
    // Moves an image or file archive with the rate limit, progress notices, cancel support and
    // an inactivity timeout (no data sent, received or, for an upload, read from the local files
    // for inactivityTimeoutMs). An upload sends the open device and stores the host's reply in
    // response; a download (upload == nullptr) writes the body to sink, up to sinkLimit bytes.
    TransferResult transfer(const QString& type, ClipboardArchive::ThrottledUploadDevice* upload,
                            QIODevice* sink, qint64 sinkLimit, QByteArray* response, int& qtError,
                            qint64 inactivityTimeoutMs);
    void showTransferProgress(bool upload, qint64 done, qint64 total);
    // A cancelled transfer, or one that failed without a more specific notice.
    void showTransferEnd(TransferResult result, int qtError);
    // Hermit: localSeq: local content being sent, left to the next trigger (LocalNotSent) when sync
    // could not be set up yet (a network error, a busy host, or no active stream seen by the host);
    // localChanges: the local changes observed when the job was posted (init)
    bool ensureReady(quint32 localSeq = 0, quint64 localChanges = 0);
    // A Shell host, asked in a way that needs no clipboard permission (when reading its clipboard
    // is refused, the clipboard itself cannot tell).
    bool hostIsShell();
    // One pull: what it found, for ClipboardHostPolling. A busy host leaves the content to be
    // fetched again as before; nothing under a new sequence number is recorded as seen, and poll
    // then has it looked at again by the quick polls that follow. Content the host cannot send
    // (type "other") is a host change nothing is fetched for.
    ClipboardHostPolling::Pull pullOnce(quint64 localChanges);
    // Hermit: one poll (a job): pullOnce for the local changes observed now, then the next poll
    // is timed from what it found (ClipboardHostPolling::next), or polling ends.
    void poll();
    // Hermit: times the next poll in delayMs, or stops polling (ClipboardHostPolling::k_Stop)
    void schedulePoll(int delayMs);
    // Hermit: the host's clipboard can no longer be read: the stream ended, reading its clipboard
    // is refused (401) or the host has no clipboard endpoint (404)
    bool pollingEnded() const;
    ClipboardHostPolling::Host pollingHost() const;
    // Host files (Windows): a file list for virtual files when the host can stream files, else
    // the whole archive.
    void pullFileList(quint64 localChanges);
    void pullArchive();
    // Host files not fetched for another reason than those with their own notice: says so, and
    // leaves the content to be fetched again on the next pull when that may help. Hermit: archive:
    // the files themselves (large), fetched again once at most after a network error; else their
    // list (small), fetched again twice at most, with one notice.
    void failedHostFiles(int qtError, bool archive);
    // Hermit: a network error without a reply (timeout, connection refused or reset, a transfer cut
    // off) for host text, images or files, by kind. True the first time for this host content and
    // kind: the caller shows its notice. The content is fetched again on a later pull, an image or
    // archive once, text or a file list twice, then not until the host's clipboard changes
    // (ClipboardHostRetries).
    bool retryAfterNetworkError(ClipboardHostRetries::Kind kind);
    // Logs a failed request; a missing endpoint turns sync off, a missing permission turns off
    // that direction only, with a notice.
    void handleFailure(const char* operation, int qtError, Direction direction);
    // Hermit: handleFailure for the requests a poll repeats every second or two (the setup, the
    // info or text request, a text fetch tried again): logged when the poll fails in a new way
    // (another error or status, or the first failure after the host answered), silent while it
    // keeps failing the same way, and pollRecovered says once when the host answers again.
    void handlePollFailure(const char* operation, int qtError, Direction direction);
    void pollRecovered();
    void denyDirection(Direction direction);
    // Shows a ClipboardNotice now; repeat: also once more when the user returns to the stream window.
    void notify(int notice, bool repeat);
    // Hermit: local change localOrder is on the host; the host's reply names its sequence number.
    void recordLocalSent(const QByteArray& responseBody, quint64 localOrder);
    void deliver(ClipboardHostContent* content);
    // 503 on the last request: another program held the host's clipboard
    bool hostClipboardBusy() const { return m_LastHttpStatus == 503; }
    // Local content the host's busy clipboard could not take (Hermit: or that a network error
    // stopped): the main thread sends it again on the next trigger.
    void localNotSent(quint32 localSeq);
    // Hermit: local image or files not sent after a network error without a reply. True the first
    // time for this local sequence, so the caller shows its notice once per content (and sends
    // files again only once).
    bool firstLocalNetworkError(quint32 localSeq);
    // A 422 for host files: a notice for the reason the host gave.
    void notifyHostFilesRefused();
    bool stopped() const { return m_Stopped->load(); }

    NvAddress m_Address;
    uint16_t m_HttpsPort;
    QSslCertificate m_ServerCert;
    bool m_UseTrueUid;
    int m_SdlEventCode;
    int m_Generation;
    bool m_LocalChangeTracking;
    qint64 m_RateBytesPerSecond;  // 0: unlimited
    std::shared_ptr<std::atomic<bool>> m_Stopped;
    std::shared_ptr<ClipboardTransferControl> m_Control;
    NvHTTP* m_Http;

    bool m_Busy;
    QList<std::function<void()>> m_Pending;
    QByteArray m_ReadBuffer;

    Mode m_Mode;
    // Hermit: what the host holds (its sequence number; Legacy: a hash of its text), whether it is
    // waiting to be fetched again, and which change is the latest (clipboardchangeorder.h)
    ClipboardChangeOrder m_Order;
    bool m_HostStreamsFiles;  // "files=stream" in the host's info reply
    bool m_HostFilesErrorKeyValid;  // Hermit: host files of m_HostFilesErrorKey failed once with a host error
    quint64 m_HostFilesErrorKey;
    ClipboardHostRetries m_HostRetries;  // Hermit: host content that had a network error or failed to be written locally, by kind
    ClipboardHostPolling m_Polling;  // Hermit: when the host clipboard is polled next (cadence, quick polls after a busy host)
    QTimer* m_PollTimer;             // Hermit: single-shot, created on this thread for the first poll
    // Hermit: the poll's request failed and was logged, and how (handlePollFailure); whether a
    // request of the poll in progress failed (pollRecovered otherwise); the setup retry said once
    bool m_PollFailing;
    int m_PollFailError;
    int m_PollFailStatus;
    bool m_PollRequestFailed;
    bool m_SetupRetryLogged;
    bool m_LocalNetworkNoticeSeqValid;  // Hermit: local content of m_LocalNetworkNoticeSeq had a network error
    quint32 m_LocalNetworkNoticeSeq;
    bool m_UploadSentAll;  // Hermit: the last upload sent every byte (transfer)
    // Hermit: what the files sent last without the host's confirmation held, to recognize them in
    // a host file list (ClipboardChangeOrder::hostListMatched)
    ClipboardFilesSummary m_UnconfirmedFiles;
    QElapsedTimer m_Clock;  // Hermit: monotonic, for how long such files are awaited
    // Hermit: the host text last seen or sent, so identical text is not echoed back without local
    // change tracking
    bool m_HostTextHashValid;
    QByteArray m_HostTextHash;
    bool m_WarnedTextOnly;
    bool m_PullDenied;    // 401 on GET: this device may not read the host clipboard
    bool m_PushDenied;    // 401 on POST: this device may not set the host clipboard
    bool m_PushAccepted;  // a POST succeeded, so a later 401 on files means no file upload permission
    QString m_LastError;
    QByteArray m_LastErrorBody;  // start of the last failed request's body (the host's reason)
    int m_LastHttpStatus;  // of the last failed request, 0 if none
};

// Clipboard sync with hosts that have the /actions/clipboard extension.
//
// Text works with any such host. With a Shell host, Windows clients also sync
// images (PNG / DIB) and copied files and folders.
//
// Local -> host: text and images when the stream starts, when the stream window regains focus
// and whenever the local clipboard changes; files when the stream starts and when the stream
// window regains focus (not on every copy, since they can be large). Hermit: a copy that cannot
// be read yet because the program that copied still holds the clipboard is read again shortly,
// a few times (ClipboardPushRetries).
// Host -> local: Hermit: the host clipboard is polled while the stream runs (a Shell host's cheap
// info request every second, a text-only host's text every two seconds; ClipboardHostPolling),
// whether or not the stream window has the focus, and a change is fetched at once: text, images
// and file lists alike, so "copy on the host, switch to a local app, paste" works like RDP
// without leaving the window first. Leaving the window polls right away as well. Files from a
// host that can stream them are offered as virtual files and only downloaded while they are
// pasted (clipboardvirtualfiles.h).
//
// Images and files move at most at the configured rate (clipboardRateMbps), so a large
// transfer does not starve the video stream or the input packets, and can be cancelled with
// Ctrl+Alt+Shift+T.
//
// The host allows reading its clipboard and setting it separately (and file transfers on top), so
// a refused direction stops alone, with a notice. Content that does not move (too large,
// unsupported, refused by the host) gets a short notice over the stream as well.
//
// Hermit: when both sides changed, the most recent change wins; content that could not be
// delivered (a busy host, a network error, a failed local write) is retried until something newer
// replaces it (clipboardchangeorder.h). This side counts the local changes it observes, hands the
// count to every push and publishes it for every poll; content from the host carries the order of
// its change.
//
// All public methods must be called on the SDL main thread.
class ClipboardSync
{
public:
    enum class Trigger { Start, FocusGained, ClipboardChanged };

    ClipboardSync(NvComputer* computer, int sdlEventCode, SDL_Window* window);
    ~ClipboardSync();

    void start();
    void pushLocalToHost(Trigger trigger);

    // Files and folders dropped on the stream window: put on the host clipboard, to be pasted
    // there with Ctrl+V.
    void pushDroppedFiles(const QStringList& paths);
    // Hermit: looks at the host clipboard now rather than at the next poll (the stream window
    // lost the focus: what was copied on the host is wanted in the local app right away).
    void pollHostNow();

    // Applies content from the SDL user event and takes ownership of it.
    void onHostContent(ClipboardHostContent* content);

    // Frees event content that arrives when no sync object exists.
    static void discardHostContent(void* content);

    // Cancels the image or file transfer in progress, if any (cancel shortcut). False if there
    // was none.
    static bool cancelActiveTransfer();

private:
    void markLocalHandled();
    // Hermit: the order of the local change at local clipboard sequence seq, a real copy: the next
    // one the first time this sequence is seen, the same one when it is handled again
    quint64 observeLocalChange(quint32 seq);
    // Hermit: hands the count of local changes observed to the worker's polls
    // (ClipboardTransferControl::localChanges)
    void publishLocalChanges();
    // Hermit (Windows): what the local clipboard holds now, without sending anything; seq: its
    // sequence number, read before the content
    ClipboardLocalChanges::Content localClipboard(quint32& seq) const;
    // Hermit: pushLocalToHost without what a trigger does once (a focus gain's bookkeeping): also
    // a retry's push (onHostContent, LocalRetry; retry), which opens the clipboard once without
    // waiting for it, as the retry after it is scheduled anyway and the main thread streams
    void pushLocal(Trigger trigger, bool retry = false);
    // Hermit: the local clipboard content at localSeq (what: text, image or files, for the log)
    // could not be read because another program holds the clipboard: read again shortly while
    // the clipboard still holds it (ClipboardPushRetries), through the worker's timer. False
    // once the retries for this content ran out: the caller says what happens then.
    bool localClipboardBusy(Trigger trigger, quint32 localSeq, const char* what);
    void post(const std::function<void()>& job);
    void releaseHostFileList();

    QThread* m_Thread;
    ClipboardSyncWorker* m_Worker;
    std::shared_ptr<std::atomic<bool>> m_Stopped;
    std::shared_ptr<ClipboardTransferControl> m_Control;
    int m_Generation;
    void* m_WindowHandle;  // HWND on Windows, used as the clipboard owner
    int m_SdlEventCode;

    // For the downloads of pasted host files (Windows)
    NvAddress m_Address;
    uint16_t m_HttpsPort;
    QSslCertificate m_ServerCert;
    bool m_UseTrueUid;
    int m_RateMbps;
    ClipboardVirtualFiles::VirtualFileClipboard* m_VirtualFiles;

    bool m_LocalSeqValid;
    quint32 m_LocalSeq;
    // Qt's clipboard change signal, turned into the SDL event (see the constructor)
    QMetaObject::Connection m_ClipboardChanged;

    // Hermit: local changes observed so far (ClipboardChangeOrder), and the local clipboard
    // sequence number of the last one
    ClipboardLocalChanges m_Local;
    // Hermit: pushes tried again shortly while another program holds the local clipboard
    ClipboardPushRetries m_PushRetries;

    // Hermit: the host content last written to the local clipboard (its ClipboardHostContent
    // hostKey), forgotten again if a superseded host file list removed it
    bool m_LastHostContentValid = false;
    quint64 m_LastHostKey = 0;
};
