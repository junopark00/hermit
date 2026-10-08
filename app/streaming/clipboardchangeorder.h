#pragma once

#include <QLatin1Char>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdint>

// Calls are written (std::max)(...) so the min/max macros of <windows.h> cannot break them.

// Hermit: when both clipboards changed, the most recent change wins. Content that could not be
// delivered (the host was busy, a network error, a failed local write) is retried until something
// newer replaces it, and only then.
//
// Changes are put in order by the count of local changes the main thread has observed:
// - a local change gets the next number (1, 2, ...) when the main thread first sees a new local
//   clipboard sequence that is a real copy: not content we wrote from the host, not our own list of
//   host files, not an empty clipboard (we empty it ourselves, and a failed write leaves it empty);
// - a host change (a new host clipboard sequence number; for text-only hosts, new text) gets the
//   count at the time the poll that found it started (ClipboardHostPolling): newer than every
//   local change seen before that poll, older than every local change seen after it;
// - nothing on the host's clipboard under a new sequence number (emptied, or the program copying
//   there has not written its formats yet) is recorded as seen but is no change (hostNone): it
//   delivers nothing, so local content waiting to be sent still goes, and what is written there
//   next is the change;
// - content this side put on the host takes the order of its local change; dropped files the count
//   when they were dropped (newer on the host than every local change so far);
// - host content found when sync is set up is not a change while no setup failed before (at stream
//   start); a setup that succeeds after one failed takes it for a host change seen by the last
//   failed attempt (hostSetUp);
// - files whose every byte reached the host without its confirmation count as sent, and host files
//   that turn out to be them later (the host may place them after newer content) are recorded as
//   ours, in the order they already have, replacing nothing (localSentUnconfirmed), until the host
//   can no longer place them (k_UnconfirmedWaitMs);
//
// So:
// - local content is sent (again, after a busy host or a network error) only while no host change
//   was seen after it (localMayReplaceHost); otherwise it is dropped and the host's newer content
//   wins;
// - host content is fetched (again) only while no local change was seen after it (hostSeen,
//   hostPulled), and content already on its way is put on the local clipboard only then
//   (hostMayReplaceLocal, on the main thread); otherwise it is dropped and the local copy wins, and
//   is sent.
//
// The clipboard worker keeps one of these for the host side; it runs its jobs one at a time, so the
// state needs no lock. The main thread keeps the count of local changes, hands it to every push
// and publishes it for every poll (ClipboardTransferControl::localChanges). Order 0 is a change
// this side cannot place (no local change tracking): never held back.
class ClipboardChangeOrder
{
public:
    enum class HostContent {
        Unchanged,   // already delivered, given up on, or replaced by our own content
        Superseded,  // waiting to be fetched again, but a local change came after it: dropped now
        Fetch,       // new, or waiting and still the latest change: fetch and deliver it
        // Like Fetch, for host files that may be the files sent without a confirmation, placed on
        // the host's clipboard late: their list is compared with those files first
        // (hostListMatched), and they are only delivered when they are not them
        FetchAndMatch,
        // The files sent without a confirmation (judged without their list, on a host that does
        // not list its files): recorded as ours, not fetched
        OwnUpload,
    };

    // The host holds content it held already when sync started (or that we cannot have caused):
    // known, but not a change, so it is not fetched.
    void hostRecorded(uint64_t key)
    {
        m_HostKnown = true;
        m_HostKey = key;
        m_HostPending = false;
        m_Unconfirmed = false;
        m_Matching = false;
    }

    // A setup (the worker's init) starts in a job posted after localChanges local changes (a push:
    // the order of its local change). True when one failed before: what this one finds is then
    // no longer known to be the host's content from before the stream (hostSetUp).
    bool setupStarting(uint64_t localChanges)
    {
        m_SetupOrder = localChanges;
        return m_SetupFailed;
    }

    // The setup that started last could not get the host's answer (a network error, a busy host,
    // no active stream seen by the host yet): the next job tries again.
    void setupFailed()
    {
        m_SetupFailed = true;
        m_SetupFailedOrder = (std::max)(m_SetupFailedOrder, m_SetupOrder);
    }

    // Sync was set up and found host content key. While no setup failed before (at stream start),
    // it is the host's content from before the stream (hostRecorded). A setup after a failed one
    // cannot tell whether the host's clipboard changed in between (the user copied there during
    // the stream), so the content is a host change seen by the last failed attempt: newer than
    // every local change observed before that attempt's job was posted, older than every one
    // after. Local content from before it (waiting to be sent again) no longer replaces it and the
    // next pull fetches it; a copy made since wins and is sent, also the one whose job set sync up
    // now. The trade-off: when the host did not change after all, its older content wins over
    // local content from before the last failed attempt, which is then not sent (copy it again).
    void hostSetUp(uint64_t key)
    {
        if (!m_SetupFailed) {
            hostRecorded(key);
            return;
        }
        m_HostKnown = true;
        m_HostKey = key;
        m_HostOrder = (std::max)(m_HostOrder, m_SetupFailedOrder);
        m_HostPending = true;
        m_Unconfirmed = false;
        m_Matching = false;
    }

    // A pull posted after localChanges local changes found host content key (its clipboard sequence
    // number, or for text-only hosts a hash of its text). Fetch also takes the content: call
    // hostRetry when it could not be delivered in a way that may pass. A pull that fetches content
    // under another key than it asked for (a file list with its own sequence number) calls this
    // again with that key.
    HostContent hostSeen(uint64_t key, uint64_t localChanges)
    {
        if (!m_HostKnown || key != m_HostKey) {
            m_HostKnown = true;
            m_HostKey = key;
            m_HostOrder = (std::max)(m_HostOrder, localChanges);
            m_HostPending = false;
            m_Matching = false;
            return HostContent::Fetch;
        }
        if (!m_HostPending) {
            return HostContent::Unchanged;
        }
        m_HostPending = false;
        return hostMayReplaceLocal(m_HostOrder, localChanges) ? HostContent::Fetch : HostContent::Superseded;
    }

    // What a pull posted after localChanges local changes does with host content key: hostSeen,
    // except while files sent without a confirmation are awaited (localSentUnconfirmed). files: the
    // host holds files; listed: the host lists its files before they are fetched (a Shell host
    // that streams files).
    // - listed: new host files (or such files waiting to be fetched again after their list failed)
    //   are fetched with FetchAndMatch, so their list is compared with the files sent before
    //   anything else (hostListMatched). Other host content is a host change as usual, and the wait
    //   goes on, as it does after later sends and local changes: the host may still place the files
    //   after them (it unpacks them on a worker of its own).
    // - not listed: the files cannot be compared without downloading them, so the first new host
    //   content ends the wait, and is taken for those files (OwnUpload) when it is files and no
    //   local change came after them.
    // nowMs: a monotonic clock in milliseconds, the one handed to localSentUnconfirmed; the wait
    // ends once k_UnconfirmedWaitMs passed since the upload started.
    HostContent hostPulled(uint64_t key, bool files, bool listed, uint64_t localChanges, uint64_t nowMs)
    {
        expireUnconfirmed(nowMs);
        if (m_Unconfirmed) {
            const bool newKey = !m_HostKnown || key != m_HostKey;
            if (listed && files && (newKey || m_Matching)) {
                // The order before these files, which stays if they turn out to be ours
                const uint64_t before = newKey ? m_HostOrder : m_MatchOrder;
                const HostContent seen = hostSeen(key, localChanges);
                m_Matching = seen == HostContent::Fetch;
                m_MatchOrder = before;
                return m_Matching ? HostContent::FetchAndMatch : seen;
            }
            if (!listed && newKey) {
                m_Unconfirmed = false;
                if (files && localChanges <= m_UnconfirmedOrder) {
                    takeUpload(key, m_HostOrder);
                    return HostContent::OwnUpload;
                }
            }
        }
        return hostSeen(key, localChanges);
    }

    // The list of the host files fetched with FetchAndMatch arrived under key (its own sequence
    // number: newer if the host's clipboard changed in between); same: it holds what the files
    // sent without a confirmation held (ClipboardFilesSummary). True when they are those files:
    // recorded as ours and not delivered, keeping the order from before them, so they replace
    // nothing that came after the upload (neither the local clipboard nor local content still to
    // be sent). Otherwise they are files copied on the host: the wait ends, and they are a host
    // change (hostSeen with key). nowMs: as for hostPulled (the list may take a while).
    bool hostListMatched(uint64_t key, bool same, uint64_t nowMs)
    {
        expireUnconfirmed(nowMs);
        if (!m_Matching) {
            return false;
        }
        m_Matching = false;
        m_Unconfirmed = false;
        if (!same) {
            return false;
        }
        takeUpload(key, m_MatchOrder);
        return true;
    }

    // A pull found nothing on the host's clipboard under key. Recorded as seen, so it is not looked
    // at on every poll (the quick polls that follow look again: hostRetry), but it is no change:
    // nothing is delivered, so local content waiting to be sent still goes (localMayReplaceHost),
    // and what the host holds next is the change, in the order of the pull that finds it. Host
    // content waiting to be fetched again is gone from the host. True when the key is new, or
    // waiting to be looked at again.
    bool hostNone(uint64_t key)
    {
        if (m_HostKnown && key == m_HostKey && !m_HostPending) {
            return false;
        }
        m_HostKnown = true;
        m_HostKey = key;
        m_HostPending = false;
        m_Matching = false;
        return true;
    }

    // Host content key was not delivered (the fetch failed in a way that may pass, or it could not
    // be put on the local clipboard): fetched again on a later pull, unless newer host content was
    // seen (or sent) meanwhile or a local change comes first.
    void hostRetry(uint64_t key)
    {
        if (m_HostKnown && key == m_HostKey) {
            m_HostPending = true;
        }
    }

    // Local change localOrder is now on the host. keyKnown: the host said under which key (its
    // sequence number), so a later pull does not fetch it back. Host content waiting to be fetched
    // again is gone from the host. Files sent without a confirmation are still awaited: the host
    // may place them after this.
    void localSent(bool keyKnown, uint64_t key, uint64_t localOrder)
    {
        if (keyKnown) {
            m_HostKnown = true;
            m_HostKey = key;
        }
        m_HostPending = false;
        m_HostOrder = (std::max)(m_HostOrder, localOrder);
        m_Matching = false;
    }

    // Every byte of the files of local change localOrder reached the host, but it did not confirm
    // taking them in time (it was still unpacking them). It most likely did, under a sequence
    // number we do not know yet: recorded as sent, and host files that turn out to be them
    // (hostPulled) are recorded as ours instead of being fetched back over the local copy, also
    // when they arrive after newer content. startedMs: when the upload's request started (a
    // monotonic clock in milliseconds); the files are awaited for k_UnconfirmedWaitMs from then.
    void localSentUnconfirmed(uint64_t localOrder, uint64_t startedMs)
    {
        localSent(false, 0, localOrder);
        m_Unconfirmed = true;
        m_UnconfirmedOrder = localOrder;
        m_UnconfirmedStartedMs = startedMs;
    }

    // Shell answers a request within its content timeout (timeout_content, 1800 s): files that are
    // not on its clipboard this long after their upload's request started will not come any more,
    // and host files after that are not taken for them.
    static constexpr uint64_t k_UnconfirmedWaitMs = 30 * 60 * 1000;

    // Whether local change localOrder may still replace the host's content: no host change (nor a
    // newer local change) reached the host after it.
    bool localMayReplaceHost(uint64_t localOrder) const
    {
        return localOrder == 0 || localOrder > m_HostOrder;
    }

    // Whether host content of hostOrder may still replace the local clipboard, localChanges local
    // changes having been observed by now: none came after it.
    static bool hostMayReplaceLocal(uint64_t hostOrder, uint64_t localChanges)
    {
        return localChanges <= hostOrder;
    }

    bool hostKnown() const { return m_HostKnown; }
    uint64_t hostKey() const { return m_HostKey; }
    // The order of the change the host's content came from (0 before any)
    uint64_t hostOrder() const { return m_HostOrder; }
    // Host content waiting to be fetched again
    bool hostPending() const { return m_HostPending; }
    // Files sent without a confirmation, not seen on the host yet
    bool unconfirmedUpload() const { return m_Unconfirmed; }
    // Host files fetched with FetchAndMatch, their list not compared yet (hostListMatched)
    bool matchingUpload() const { return m_Matching; }

private:
    // The wait for files sent without a confirmation ends once the host can no longer place them.
    void expireUnconfirmed(uint64_t nowMs)
    {
        if (m_Unconfirmed && nowMs >= m_UnconfirmedStartedMs + k_UnconfirmedWaitMs) {
            m_Unconfirmed = false;
            m_Matching = false;
        }
    }

    // Host files under key are the files sent without a confirmation: the host's content, in the
    // order of their upload unless content after it (orderBefore) reached the host first.
    void takeUpload(uint64_t key, uint64_t orderBefore)
    {
        m_HostKnown = true;
        m_HostKey = key;
        m_HostOrder = (std::max)(orderBefore, m_UnconfirmedOrder);
        m_HostPending = false;
        m_Unconfirmed = false;
        m_Matching = false;
    }

    bool m_HostKnown = false;
    uint64_t m_HostKey = 0;
    uint64_t m_HostOrder = 0;
    bool m_HostPending = false;
    uint64_t m_SetupOrder = 0;        // of the job whose setup started last
    bool m_SetupFailed = false;       // a setup failed (tried again by a later job)
    uint64_t m_SetupFailedOrder = 0;  // of the job whose setup failed last
    bool m_Unconfirmed = false;
    uint64_t m_UnconfirmedOrder = 0;
    uint64_t m_UnconfirmedStartedMs = 0;  // when their upload's request started
    // The host files under m_HostKey are being compared with the unconfirmed upload; m_MatchOrder:
    // the host's order before them
    bool m_Matching = false;
    uint64_t m_MatchOrder = 0;
};

// Hermit: what a set of files holds, to recognize files we sent among the host's files: the names
// of its top-level items (what was copied; sorted) and the total size of its files. Entries: the
// items of an upload (ClipboardArchive::Entry) or of a host file list (ClipboardArchive::RemoteFile),
// each with a relative path ('/' separators), whether it is a directory and its size.
struct ClipboardFilesSummary
{
    QStringList names;
    uint64_t bytes = 0;

    template <typename Entries>
    static ClipboardFilesSummary of(const Entries& entries)
    {
        ClipboardFilesSummary summary;
        for (const auto& entry : entries) {
            if (!entry.path.contains(QLatin1Char('/'))) {
                summary.names.append(entry.path);
            }
            if (!entry.directory) {
                summary.bytes += entry.size;
            }
        }
        summary.names.sort();
        return summary;
    }

    bool operator==(const ClipboardFilesSummary& other) const
    {
        return bytes == other.bytes && names == other.names;
    }
};

// Hermit: host content that could not be delivered in a way that may be passing: its fetch hit a
// network error without a reply (a timeout, connection refused or reset, a transfer cut off), or
// it could not be written to the local clipboard (another program held it). One notice per content
// and kind, and whether it is fetched again: a few times at most (maxRetries), then not until the
// host's clipboard changes (a new key), so a failure that lasts does not fetch the same image (up
// to 32 MB) or archive (up to 256 MB) on every poll, every second. Each kind keeps its own record,
// so a failed file list does not use up the retries and notice of the archive of the same files
// (and the other way round), nor does a failed fetch those of a failed local write.
class ClipboardHostRetries
{
public:
    enum class Kind {
        Image,       // large: fetched once more at most
        FileList,    // small (names and sizes), but the host's walk may take a while: twice more
        Archive,     // large, like an image
        LocalWrite,  // any content not written locally: fetched once more at most
    };

    struct Outcome {
        bool notice = false;  // the first failure for this content and kind: say so
        bool retry = false;   // fetch it again on a later pull (ClipboardChangeOrder::hostRetry)
    };

    // How often content of kind is fetched again for the same key
    static int maxRetries(Kind kind)
    {
        return kind == Kind::FileList ? 2 : 1;
    }

    // Host content key failed in the way kind stands for
    Outcome failed(Kind kind, uint64_t key)
    {
        Seen& seen = m_Seen[static_cast<int>(kind)];
        if (!seen.valid || seen.key != key) {
            seen.valid = true;
            seen.key = key;
            seen.failures = 0;
        }
        seen.failures++;
        Outcome outcome;
        outcome.notice = seen.failures == 1;
        outcome.retry = seen.failures <= maxRetries(kind);
        return outcome;
    }

    // Failures of kind recorded for the key seen last
    int failures(Kind kind) const { return m_Seen[static_cast<int>(kind)].failures; }

private:
    struct Seen {
        bool valid = false;
        uint64_t key = 0;
        int failures = 0;
    };
    Seen m_Seen[4];
};

// Hermit: a clipboard job that came a moment too early (another program still holds a clipboard,
// or has not written its formats yet) is tried again shortly (k_RetryDelayMs), a few times
// (k_MaxRetries) for the same occasion; after that, as before, at the usual cadence or on the next
// trigger. The occasion is a key (a run of host polls, a local clipboard sequence number): a new
// key starts over, and a retry scheduled for a key that moved on meanwhile is dropped (the new
// occasion has a job of its own). Only the job is repeated, so what it decides (which change wins,
// once-per-content notices) stays as it is. ClipboardHostPolling (the host side) and
// ClipboardPushRetries (the local side) name the results that count as such a passing condition.
class ClipboardShortRetries
{
public:
    static constexpr int k_RetryDelayMs = 300;
    static constexpr int k_MaxRetries = 4;  // per key: about 1.2 s in all

    // The job for key ended with a passing condition (passing) or not. True: run it again after
    // k_RetryDelayMs (unless the key moved on by then).
    bool again(bool passing, uint64_t key)
    {
        if (!m_KeyValid || key != m_Key) {
            m_KeyValid = true;
            m_Key = key;
            m_Retries = 0;
        }
        if (!passing || m_Retries >= k_MaxRetries) {
            return false;
        }
        m_Retries++;
        return true;
    }

    // Retries made for the key seen last
    int retries() const { return m_Retries; }

private:
    bool m_KeyValid = false;
    uint64_t m_Key = 0;
    int m_Retries = 0;
};

// Hermit: the host's clipboard is polled while the stream runs, whether or not the stream window
// has the focus, so what is copied on the host is on the local clipboard within about a second:
// a Shell host answers a cheap info request (its clipboard sequence number and content type)
// every k_ExtendedIntervalMs, and a change found is fetched at once; a text-only host has no such
// request, so its text is fetched every k_LegacyIntervalMs (its hash tells whether it changed).
// This decides when the next poll comes from what the last one found:
// - a poll that found the host's clipboard busy (503: another program holds it, such as a
//   messenger or a clipboard manager reading every change), or nothing under a new sequence number
//   (the program copying there has emptied the clipboard and not written its formats yet), is
//   followed by the next poll shortly (k_RetryDelayMs), a few times (k_MaxRetries) for the same
//   occasion (ClipboardShortRetries); after that, at the usual cadence. Both usually pass within a
//   moment. A poll that delivered, found nothing new or failed in another way starts a new
//   occasion, so a later busy host gets its own quick polls;
// - leaving the stream window runs a poll at once (pollNow) rather than at the cadence, as a pull
//   on focus loss did before, and that poll is a new occasion as well;
// - while sync is not set up yet (the host could not be reached, or saw no active stream), each
//   poll tries the setup again, every k_SetupRetryMs;
// - polling ends (k_Stop) once the host's clipboard can no longer be read: the stream ended, the
//   host refused reading its clipboard (401) or has no clipboard endpoint (404).
// Every poll is an ordinary pull: a local copy made meanwhile still wins (ClipboardChangeOrder: a
// host change found by a poll is newer than the local changes observed when the poll started,
// older than every one after), and nothing under a new number is looked at again by the quick
// polls only, staying recorded as seen when it is still nothing after the last one. The clipboard
// worker keeps one of these (its jobs run one at a time) and times the polls on its own thread.
class ClipboardHostPolling
{
public:
    enum class Pull {
        Done,     // delivered, nothing new, or failed in a way handled by a later poll (or not at all)
        Busy,     // 503 on a request of the pull (info, text, image, file list or archive)
        NoneNew,  // the host holds nothing, under a sequence number not seen before (or seen with nothing)
    };

    enum class Host {
        Unknown,   // sync not set up yet: the poll tries the setup again
        Extended,  // a Shell host: info requests
        Legacy,    // a text-only host: its text
    };

    static constexpr int k_ExtendedIntervalMs = 1000;
    static constexpr int k_LegacyIntervalMs = 2000;  // the whole text each time: less often
    static constexpr int k_SetupRetryMs = 2000;
    static constexpr int k_RetryDelayMs = ClipboardShortRetries::k_RetryDelayMs;
    static constexpr int k_MaxRetries = ClipboardShortRetries::k_MaxRetries;  // per occasion
    static constexpr int k_Stop = -1;

    // The poll that just ended found result, the host being host. stop: the host's clipboard can
    // no longer be read (see above). Returns the delay until the next poll in milliseconds, or
    // k_Stop when polling ends.
    int next(Pull result, Host host, bool stop)
    {
        if (stop) {
            return k_Stop;
        }
        if (m_Retries.again(result != Pull::Done, m_Occasion)) {
            return k_RetryDelayMs;
        }
        if (result == Pull::Done) {
            m_Occasion++;
        }
        return interval(host);
    }

    // A poll runs now, outside the cadence (the user left the stream window): a new occasion, so
    // a busy host is polled again shortly from here, as it was after a focus loss before.
    void pollNow()
    {
        m_Occasion++;
    }

    // The usual cadence for host
    static int interval(Host host)
    {
        switch (host) {
        case Host::Extended:
            return k_ExtendedIntervalMs;
        case Host::Legacy:
            return k_LegacyIntervalMs;
        default:
            return k_SetupRetryMs;
        }
    }

    // Quick polls made for the occasion seen last
    int retries() const { return m_Retries.retries(); }

private:
    ClipboardShortRetries m_Retries;
    uint64_t m_Occasion = 0;
};

// Hermit: a push right after a local copy may come a moment too early as well: the program that
// copied still holds the local clipboard (the snipping tool after Win+Shift+S holds it for a
// while), so the content cannot be read: SDL's text read gives nothing while text is listed (it
// opens the clipboard once, without waiting), the image read cannot open the clipboard, the file
// read gives no files while files are listed. Without this, the content waited for the next focus
// change, and a paste in the stream meanwhile gave the host's older content. So the push is tried
// again shortly, a few times, while the local clipboard still holds the same content (its sequence
// number: a newer copy has a trigger, and retries, of its own); after the last retry, as before,
// on the next trigger. Only the read is repeated: a push that sent, found content it cannot send
// or found the clipboard handled is not repeated, so the content is sent once and its notices
// shown once. The main thread keeps one of these (every push runs there).
class ClipboardPushRetries
{
public:
    enum class Read {
        Done,  // sent, nothing to send, or not read for a reason the next trigger handles
        Busy,  // the local clipboard could not be read: another program holds it
    };

    static constexpr int k_RetryDelayMs = ClipboardShortRetries::k_RetryDelayMs;
    static constexpr int k_MaxRetries = ClipboardShortRetries::k_MaxRetries;  // per local sequence number

    // The push for the local clipboard content at sequence number seq ended with result. True:
    // read it again after k_RetryDelayMs, unless the clipboard changed by then (applies).
    bool again(Read result, uint32_t seq)
    {
        return m_Retries.again(result == Read::Busy, seq);
    }

    // Whether a retry scheduled for the content at seq is still about what the local clipboard
    // holds when it fires (currentSeq): a copy made since has a trigger of its own.
    static bool applies(uint32_t seq, uint32_t currentSeq)
    {
        return seq == currentSeq;
    }

    // Retries made for the sequence number seen last
    int retries() const { return m_Retries.retries(); }

private:
    ClipboardShortRetries m_Retries;
};

// Hermit: the main thread's side: which local clipboard content is a local change, and the count
// of local changes observed (the order ClipboardChangeOrder puts changes in).
class ClipboardLocalChanges
{
public:
    enum class Content {
        Handled,       // its sequence number was handled already (sent, or written by us)
        OwnHostFiles,  // our own list of host files (virtual files), also while it is being set
        Empty,         // nothing: we empty it ourselves, and a failed write of host content leaves it empty
        Copy,          // a copy made locally
    };

    // What the local clipboard holds, read without opening it.
    struct View {
        bool handled = false;     // its sequence number is the one handled last
        bool marker = false;      // our marker format is on it
        // Our owner thread is putting a host file list on the clipboard (OleSetClipboard), or was
        // while the clipboard was read: the formats appear one by one, the marker possibly not yet
        bool publishing = false;
        bool ownerPublishes = false;  // the clipboard's owner window belongs to our owner thread
        int formats = 0;          // formats on it
    };

    static Content classify(const View& view)
    {
        if (view.handled) {
            return Content::Handled;
        }
        // Never a local copy: sending it would echo the host's files back, and counting it as a
        // copy would release the list (the owner thread then empties the clipboard). While a
        // publish is in progress, only content our owner thread's window owns is ours: file
        // descriptors another program put there meanwhile (an e-mail attachment) are a copy.
        if (view.marker || (view.publishing && view.ownerPublishes)) {
            return Content::OwnHostFiles;
        }
        if (view.formats == 0) {
            return Content::Empty;
        }
        return Content::Copy;
    }

    // A copy at local clipboard sequence number seq: its order, the next one the first time this
    // sequence number is seen and the same one when it is seen again. isNew: whether it was new.
    uint64_t observe(uint32_t seq, bool* isNew = nullptr)
    {
        const bool fresh = !m_SeqValid || m_Seq != seq;
        if (fresh) {
            m_Seq = seq;
            m_SeqValid = true;
            m_Count++;
        }
        if (isNew != nullptr) {
            *isNew = fresh;
        }
        return m_Count;
    }

    // Host content of hostOrder arrives while the local clipboard holds current at seq: whether
    // it may replace it. A copy there not observed yet (its clipboard update notice still queued
    // behind this content) counts first, and then wins. isNew: whether such a copy was observed now.
    bool hostMayReplace(uint64_t hostOrder, Content current, uint32_t seq, bool* isNew = nullptr)
    {
        if (isNew != nullptr) {
            *isNew = false;
        }
        if (current == Content::Copy) {
            observe(seq, isNew);
        }
        return ClipboardChangeOrder::hostMayReplaceLocal(hostOrder, m_Count);
    }

    // Local changes observed so far
    uint64_t count() const { return m_Count; }

private:
    uint64_t m_Count = 0;
    bool m_SeqValid = false;
    uint32_t m_Seq = 0;
};
