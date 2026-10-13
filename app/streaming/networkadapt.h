#pragma once

#include <algorithm>
#include <cstdint>

// Calls are written (std::min)(...) so the min/max macros of <windows.h> cannot break them.

// Hermit: network adaptation for streams to a Shell host (DEV-PLAN 1.4 step 2). Replaces the
// one-second automatic bitrate: fed every 250 ms with the frames that arrived, the frame numbers
// the network skipped and the control stream's round trip, it tells two kinds of trouble apart
// and answers each with the tool that works for it:
// - congestion (the round trip rises well above its usual value: by more than 40 ms and 1.5x,
//   with or without loss) means the link cannot carry the bitrate: the bitrate is cut at once,
//   by 30% (frame priority) or 20% (quality priority), again every 500 ms while it lasts, down
//   to the policy's floor;
// - link loss (5% or more of the frames lost while the round trip stays flat, within 20 ms of
//   usual) is not caused by the bitrate (the first recording showed the same loss at 35 and at
//   7 Mbps), so forward error correction is raised first, from the host's default to 35 and 50%
//   (at least). 50% parity puts 1.5x the bytes on the wire, so when loss goes on for 8 more
//   windows (2 s) at 50% the bitrate is cut by 15%, at most twice (about 0.72x, which brings the
//   wire rate back near the original); further cuts come only from congestion. Quality priority
//   keeps a high floor and stays at 50% FEC there.
// A decision needs 2 consecutive windows of the same kind (0.5 s); one action per 500 ms at
// most; a window of the other kind does not switch the regime until it repeats (no flapping);
// windows with fewer than 3 frames (a still picture) decide nothing and neither add to nor clear
// the clean windows.
// The usual round trip is tracked as a minimum that rises only in clean windows (0.5 ms per
// second), so a congestion that lasts is not mistaken for the new normal.
// Recovery: after 12 clean windows (3 s) the bitrate rises by 15% of the chosen value towards
// it, every 3 s; the first rise after a cut waits 6 s after that cut. FEC goes down one step
// after 24 clean windows (6 s), every 6 s. A grey window (1-5% loss, or a round trip 16-40 ms
// above usual) counts half a clean window towards recovery, and 24 grey or clean windows in a
// row end a trouble regime, so a link with a steady 2% loss still recovers, slowly.
// Floors: frame priority max(2 Mbps, chosen/8), quality priority max(4 Mbps, chosen/3), rounded
// down to 500 kbps. A switch to quality priority below its floor raises the bitrate to the floor
// at the next clean window.
// No Qt, no SDL: tested in hermit/tests/hermit_core_test.cpp (testNetworkAdapt), the plumbing
// (timer, requests, panel, overlay, diagnostics log) lives in session.cpp.
class NetworkAdapt
{
public:
    enum class Policy { FramePriority, QualityPriority };

    // The controller's view of the link (shown in the overlay). Idle windows leave it as it is.
    enum class Regime { Clean, Loss, Congested };

    struct Window {
        int framesReceived = 0;
        int framesDropped = 0;   // frame numbers skipped on arrival (lost in the network)
        int rttMs = 0;           // 0 when unknown
    };

    struct Action {
        int bitrateKbps = 0;     // 0: keep the bitrate
        int fecPercent = 0;      // 0: keep FEC; fecBase(): back to the host's default
        const char* reason = nullptr;  // "congested", "loss", "recover", "floor" or "ceiling"; null without an action
    };

    static constexpr int kWindowMs = 250;
    static constexpr int kFecDefault = 20;  // the usual host default, until the host tells its own
    static constexpr int kFecMid = 35;
    static constexpr int kFecMax = 50;
    static constexpr int kMinFramesPerWindow = 3;
    static constexpr int kDecisionWindows = 2;
    static constexpr uint64_t kActionGapMs = 500;
    static constexpr int kLossWindowsBeforeCut = 8;
    static constexpr int kMaxLossCuts = 2;
    static constexpr int kRaiseAfterCleanWindows = 12;
    static constexpr int kFecDownAfterCleanWindows = 24;
    static constexpr int kGreyWindowsEndRegime = 24;
    static constexpr uint64_t kRaiseGapMs = 3000;
    static constexpr uint64_t kRaiseHoldAfterCutMs = 6000;
    static constexpr uint64_t kFecDownGapMs = 6000;

    // chosenKbps: the user's bitrate, the ceiling; startKbps: where the host streams now (0: at
    // the ceiling); fecAvailable: the host takes live FEC changes and its default is known
    // (otherwise loss goes straight to the bitrate rule). The FEC ladder starts at the base.
    void reset(int chosenKbps, int startKbps, Policy policy, bool fecAvailable)
    {
        m_Chosen = chosenKbps;
        m_Policy = policy;
        m_FecAvailable = fecAvailable;
        m_Bitrate = startKbps > 0 ? (std::clamp)(startKbps, (std::min)(floorKbps(), chosenKbps), chosenKbps) : chosenKbps;
        m_Fec = m_FecBase;
        m_BaseRtt = -1;
        m_Regime = Regime::Clean;
        m_RaiseToFloor = false;
        m_LastActionMs = m_LastCutMs = m_LastRaiseMs = m_LastFecDownMs = 0;
        clearStreaks();
        m_LossCuts = 0;
    }

    int bitrate() const { return m_Bitrate; }
    int fec() const { return m_Fec; }
    int fecBase() const { return m_FecBase; }
    int chosen() const { return m_Chosen; }
    Policy policy() const { return m_Policy; }
    Regime regime() const { return m_Regime; }
    float baseRtt() const { return m_BaseRtt; }

    // The policy follows the mouse mode; its floor applies from the next cut on. Quality priority
    // taken up below its floor raises the bitrate to the floor at the next clean window.
    void setPolicy(Policy policy)
    {
        if (policy == m_Policy) {
            return;
        }
        m_Policy = policy;
        m_RaiseToFloor = m_Bitrate < floorKbps();
    }

    void setFecAvailable(bool available) { m_FecAvailable = available; }

    // The host's default FEC (its first reply): the ladder's base and the value a recovery ends
    // at. The current step follows when it was at the old base or below the new one.
    void setFecBase(int percent)
    {
        const int oldBase = m_FecBase;
        m_FecBase = percent;
        if (m_Fec <= oldBase || m_Fec < percent) {
            m_Fec = percent;
        }
    }

    int fecMid() const { return (std::max)(m_FecBase, kFecMid); }
    int fecMax() const { return (std::max)(m_FecBase, kFecMax); }

    int floorKbps() const
    {
        return m_Policy == Policy::FramePriority ? (std::max)(2000, m_Chosen / 8 / 500 * 500)
                                                 : (std::max)(4000, m_Chosen / 3 / 500 * 500);
    }

    // One 250 ms window ending at nowMs. chosenKbps: the user's current choice; a new one
    // restarts the bitrate there (the ceiling moved) and keeps the FEC step.
    Action update(const Window& w, uint64_t nowMs, int chosenKbps)
    {
        Action action;
        if (chosenKbps != m_Chosen) {
            m_Chosen = chosenKbps;
            m_Bitrate = chosenKbps;
            m_LastActionMs = nowMs;
            m_LastCutMs = 0;
            m_RaiseToFloor = false;
            clearStreaks();
            m_LossCuts = 0;
            action.bitrateKbps = m_Bitrate;
            action.reason = "ceiling";
            return action;
        }

        // Usual round trip: follows drops at once; rises (0.5 ms per second) only in clean
        // windows, so trouble that lasts does not become the new normal
        if (w.rttMs > 0 && (m_BaseRtt < 0 || w.rttMs < m_BaseRtt)) {
            m_BaseRtt = w.rttMs;
        }

        if (w.framesReceived < kMinFramesPerWindow) {
            return action;  // too few frames to judge: neither clean nor trouble
        }

        const Class c = classify(w);
        if (c == Class::Clean) {
            if (w.rttMs > 0) {
                m_BaseRtt = (std::min<float>)(w.rttMs, m_BaseRtt + 0.125f);
            }
            m_CleanStreak++;
            m_NonBadStreak++;
            m_RaiseHalves += 2;
            m_FecHalves += 2;
            m_BadStreak = m_LossStreak = m_CongestedStreak = 0;
            m_LossWindowsAtMaxFec = 0;
            if (m_CleanStreak >= kDecisionWindows || m_NonBadStreak >= kGreyWindowsEndRegime) {
                endRegime();
            }
            if (m_RaiseToFloor && nowMs - m_LastActionMs >= kActionGapMs) {
                // Quality priority taken up below its floor: one step up to the floor
                m_RaiseToFloor = false;
                m_Bitrate = (std::min)(m_Chosen, floorKbps());
                m_LastActionMs = m_LastRaiseMs = nowMs;
                action.bitrateKbps = m_Bitrate;
                action.reason = "floor";
                return action;
            }
            return recover(nowMs, action);
        }

        m_CleanStreak = 0;
        m_BadStreak++;
        if (c == Class::Grey) {
            // Mild loss, or a somewhat higher round trip: no decision, half a clean window
            m_NonBadStreak++;
            m_RaiseHalves += 1;
            m_FecHalves += 1;
            if (m_NonBadStreak >= kGreyWindowsEndRegime) {
                endRegime();
            }
        }
        else {
            m_NonBadStreak = 0;
            m_RaiseHalves = m_FecHalves = 0;
            if (c == Class::Loss) {
                m_LossStreak++;
                m_CongestedStreak = 0;
            }
            else {
                m_CongestedStreak++;
                m_LossStreak = 0;
            }
        }
        if (m_CongestedStreak >= kDecisionWindows) {
            m_Regime = Regime::Congested;
        }
        else if (m_LossStreak >= kDecisionWindows) {
            m_Regime = Regime::Loss;
        }
        if (c == Class::Loss && m_Regime == Regime::Loss && (m_Fec >= fecMax() || !m_FecAvailable)) {
            m_LossWindowsAtMaxFec++;  // loss going on at the top of the ladder (grey windows do not count)
        }

        if (m_Regime == Regime::Clean) {
            return recover(nowMs, action);  // grey windows after a clean run
        }
        if (c == Class::Grey || m_BadStreak < kDecisionWindows || nowMs - m_LastActionMs < kActionGapMs) {
            return action;  // a grey window decides nothing (trouble that is easing is not cut further)
        }
        if (m_Regime == Regime::Congested) {
            return cut(m_Policy == Policy::FramePriority ? 0.7f : 0.8f, nowMs, "congested", action);
        }
        if (m_Fec < fecMax() && m_FecAvailable) {
            m_Fec = m_Fec < fecMid() ? fecMid() : fecMax();
            m_LastActionMs = nowMs;
            m_LossWindowsAtMaxFec = 0;
            action.fecPercent = m_Fec;
            action.reason = "loss";
            return action;
        }
        if (m_LossWindowsAtMaxFec >= kLossWindowsBeforeCut && m_LossCuts < kMaxLossCuts) {
            m_LossWindowsAtMaxFec = 0;
            cut(0.85f, nowMs, "loss", action);
            if (action.reason != nullptr) {
                m_LossCuts++;
            }
        }
        return action;
    }

private:
    enum class Class { Clean, Grey, Loss, Congested };

    Class classify(const Window& w) const
    {
        const int total = w.framesReceived + w.framesDropped;
        const float lossPct = total > 0 ? w.framesDropped * 100.0f / total : 0.0f;
        const float overBase = m_BaseRtt > 0 && w.rttMs > 0 ? w.rttMs - m_BaseRtt : 0.0f;
        if (overBase > 40 && w.rttMs > m_BaseRtt * 1.5f) {
            return Class::Congested;
        }
        if (lossPct >= 5.0f && overBase <= 20) {
            return Class::Loss;
        }
        if (lossPct < 1.0f && overBase <= 15) {
            return Class::Clean;
        }
        return Class::Grey;
    }

    void endRegime()
    {
        m_Regime = Regime::Clean;
        m_LossStreak = m_CongestedStreak = m_LossWindowsAtMaxFec = 0;
        m_LossCuts = 0;
    }

    Action& cut(float factor, uint64_t nowMs, const char* reason, Action& action)
    {
        const int next = (std::max)(floorKbps(), (int)(m_Bitrate * factor) / 500 * 500);
        if (next < m_Bitrate) {
            m_Bitrate = next;
            m_LastActionMs = m_LastCutMs = nowMs;
            action.bitrateKbps = next;
            action.reason = reason;
        }
        return action;
    }

    Action& recover(uint64_t nowMs, Action& action)
    {
        if (m_Regime != Regime::Clean || nowMs - m_LastActionMs < kActionGapMs) {
            return action;
        }
        if (m_Bitrate < m_Chosen && m_RaiseHalves >= 2 * kRaiseAfterCleanWindows &&
                nowMs - m_LastRaiseMs >= kRaiseGapMs && nowMs - m_LastCutMs >= kRaiseHoldAfterCutMs) {
            const int step = (std::max)(500, m_Chosen * 15 / 100 / 500 * 500);
            m_Bitrate = (std::min)(m_Chosen, m_Bitrate + step);
            m_LastActionMs = m_LastRaiseMs = nowMs;
            m_RaiseHalves = 0;
            action.bitrateKbps = m_Bitrate;
            action.reason = "recover";
            return action;
        }
        if (m_Fec > m_FecBase && m_FecAvailable && m_FecHalves >= 2 * kFecDownAfterCleanWindows &&
                nowMs - m_LastFecDownMs >= kFecDownGapMs) {
            m_Fec = m_Fec > fecMid() ? fecMid() : m_FecBase;
            m_LastActionMs = m_LastFecDownMs = nowMs;
            m_FecHalves = 0;
            action.fecPercent = m_Fec;
            action.reason = "recover";
        }
        return action;
    }

    void clearStreaks()
    {
        m_CleanStreak = m_NonBadStreak = m_BadStreak = m_LossStreak = m_CongestedStreak = 0;
        m_LossWindowsAtMaxFec = m_RaiseHalves = m_FecHalves = 0;
    }

    int m_Chosen = 0;
    int m_Bitrate = 0;
    int m_FecBase = kFecDefault;
    int m_Fec = kFecDefault;
    Policy m_Policy = Policy::FramePriority;
    bool m_FecAvailable = false;
    bool m_RaiseToFloor = false;
    float m_BaseRtt = -1;
    Regime m_Regime = Regime::Clean;
    int m_CleanStreak = 0;
    int m_NonBadStreak = 0;        // clean or grey windows in a row
    int m_BadStreak = 0;           // windows that are not clean in a row
    int m_LossStreak = 0;
    int m_CongestedStreak = 0;
    int m_LossWindowsAtMaxFec = 0;
    int m_LossCuts = 0;            // bitrate cuts in the loss regime since it began
    int m_RaiseHalves = 0;         // clean windows (2) and grey windows (1) since the last raise
    int m_FecHalves = 0;           // and since the last FEC step down
    uint64_t m_LastActionMs = 0;
    uint64_t m_LastCutMs = 0;
    uint64_t m_LastRaiseMs = 0;
    uint64_t m_LastFecDownMs = 0;
};
