#pragma once

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
// - host content found when sync is set up is not a change only at stream start; a setup that
//   succeeds later takes it for a host change seen then (hostSetUp);
// - files whose every byte reached the host without its confirmation count as sent, and the next
//   new host files as those files, unless a local change came first (localSentUnconfirmed).
//
// So:
// - local content is sent (again, after a busy host or a network error) only while no host change
//   was seen after it (localMayReplaceHost); otherwise it is dropped and the host's newer content
//   wins;
// - host content is fetched (again) only while no local change was seen after it (hostSeen), and
//   content already on its way is put on the local clipboard only then (hostMayReplaceLocal, on the
//   main thread); otherwise it is dropped and the local copy wins, and is sent.
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
    };

    // The host holds content it held already when sync started (or that we cannot have caused):
    // known, but not a change, so it is not fetched.
    void hostRecorded(uint64_t key)
    {
        m_HostKnown = true;
        m_HostKey = key;
        m_HostPending = false;
        m_Unconfirmed = false;
    }

    // Sync was set up and found host content key. Only the setup at stream start records it as
    // the host's content before the stream (hostRecorded). A setup that failed then and succeeds
    // later cannot tell whether the host's clipboard changed in between (the user copied there
    // during the stream), so the content is a host change seen now by a job posted after
    // localChanges local changes: local content older than that (the content from stream start,
    // waiting to be sent again) no longer replaces it, and the next pull fetches it while no newer
    // local change came. The trade-off: when the host did not change after all, its older content
    // wins over the local content from stream start, which is then not sent (copy it again).
    void hostSetUp(uint64_t key, bool atStart, uint64_t localChanges)
    {
        if (atStart) {
            hostRecorded(key);
            return;
        }
        m_HostKnown = true;
        m_HostKey = key;
        m_HostOrder = (std::max)(m_HostOrder, localChanges);
        m_HostPending = true;
        m_Unconfirmed = false;
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
            return HostContent::Fetch;
        }
        if (!m_HostPending) {
            return HostContent::Unchanged;
        }
        m_HostPending = false;
        return hostMayReplaceLocal(m_HostOrder, localChanges) ? HostContent::Fetch : HostContent::Superseded;
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
    // again is gone from the host.
    void localSent(bool keyKnown, uint64_t key, uint64_t localOrder)
    {
        if (keyKnown) {
            m_HostKnown = true;
            m_HostKey = key;
        }
        m_HostPending = false;
        m_HostOrder = (std::max)(m_HostOrder, localOrder);
        m_Unconfirmed = false;
    }

    // Every byte of the files of local change localOrder reached the host, but it did not confirm
    // taking them in time (it was still unpacking them). It most likely did, under a sequence
    // number we do not know yet: recorded as sent, and the next host files seen are taken for them
    // (hostIsUnconfirmedUpload) instead of being fetched back over the local copy.
    void localSentUnconfirmed(uint64_t localOrder)
    {
        localSent(false, 0, localOrder);
        m_Unconfirmed = true;
        m_UnconfirmedOrder = localOrder;
    }

    // A pull posted after localChanges local changes found host content key; files: it is a file
    // list. True when these are most likely the files of an unconfirmed upload: new host content,
    // the first host change since that upload, files, and no local change since. They are then
    // recorded as ours (not fetched). Any other host change ends the wait for them.
    bool hostIsUnconfirmedUpload(uint64_t key, bool files, uint64_t localChanges)
    {
        if (!m_Unconfirmed || (m_HostKnown && key == m_HostKey)) {
            return false;
        }
        m_Unconfirmed = false;
        if (!files || localChanges > m_UnconfirmedOrder) {
            return false;
        }
        localSent(true, key, m_UnconfirmedOrder);
        return true;
    }

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

private:
    bool m_HostKnown = false;
    uint64_t m_HostKey = 0;
    uint64_t m_HostOrder = 0;
    bool m_HostPending = false;
    bool m_Unconfirmed = false;
    uint64_t m_UnconfirmedOrder = 0;
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
        bool descriptor = false;  // file descriptors (FILEDESCRIPTORW) are on it
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
        // copy would release the list (the owner thread then empties the clipboard).
        if (view.marker || (view.publishing && (view.descriptor || view.ownerPublishes))) {
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
