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
//   count at the time its pull was posted: newer than every local change seen before that pull,
//   older than every local change seen after it;
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
// state needs no lock. The main thread keeps the count of local changes and hands it to every pull
// and push. Order 0 is a change this side cannot place (no local change tracking): never held back.
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

// Hermit: host images and files whose fetch hit a network error without a reply (a timeout,
// connection refused or reset, a transfer cut off), which may be passing: one notice per content,
// and whether it is fetched again. Each kind of transfer keeps its own record, so a failed file
// list does not use up the one retry and notice of the archive of the same files (and the other
// way round).
class ClipboardHostNetworkErrors
{
public:
    enum class Transfer {
        Image,     // large: fetched once more at most, then not until the host's clipboard changes
        FileList,  // small (names and sizes): fetched again each time
        Archive,   // large, like an image
    };

    struct Outcome {
        bool notice = false;  // the first error for this content and transfer: say so
        bool retry = false;   // fetch it again on a later pull (ClipboardChangeOrder::hostRetry)
    };

    // A network error for transfer of host content key
    Outcome failed(Transfer transfer, uint64_t key)
    {
        Seen& seen = m_Seen[static_cast<int>(transfer)];
        Outcome outcome;
        outcome.notice = !seen.valid || seen.key != key;
        outcome.retry = outcome.notice || transfer == Transfer::FileList;
        seen.valid = true;
        seen.key = key;
        return outcome;
    }

private:
    struct Seen {
        bool valid = false;
        uint64_t key = 0;
    };
    Seen m_Seen[3];
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
