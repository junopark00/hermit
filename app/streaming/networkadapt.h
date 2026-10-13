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
//   7 Mbps), so forward error correction is raised first: 20 -> 35 -> 50%. Only when loss goes
//   on for 4 more windows at 50% is the bitrate cut by 15% (smaller frames, fewer packets per
//   frame), down to the floor. Quality priority keeps a high floor and stays at 50% FEC there.
// A decision needs 2 consecutive windows of the same kind (0.5 s); one action per 500 ms at
// most; a window of the other kind does not switch the regime until it repeats (no flapping);
// windows with fewer than 3 frames (a still picture) decide nothing and neither add to nor clear
// the clean windows.
// Recovery: after 12 clean windows (3 s) the bitrate rises by 15% of the chosen value towards
// it, every 3 s; the first rise after a cut waits 6 s after that cut. FEC goes down one step
// after 24 clean windows (6 s), every 6 s.
// Floors: frame priority max(2 Mbps, chosen/8), quality priority max(4 Mbps, chosen/3).
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
        int fecPercent = 0;      // 0: keep FEC; kFecBase: back to the host's default
        const char* reason = nullptr;  // "congested", "loss", "recover" or "ceiling"; null without an action
    };

    static constexpr int kWindowMs = 250;
    static constexpr int kFecBase = 20;    // the host's usual default, the ladder's first rung
    static constexpr int kFecMid = 35;
    static constexpr int kFecMax = 50;
    static constexpr int kMinFramesPerWindow = 3;
    static constexpr int kDecisionWindows = 2;
    static constexpr uint64_t kActionGapMs = 500;
    static constexpr int kLossWindowsBeforeCut = 4;
    static constexpr int kRaiseAfterCleanWindows = 12;
    static constexpr int kFecDownAfterCleanWindows = 24;
    static constexpr uint64_t kRaiseGapMs = 3000;
    static constexpr uint64_t kRaiseHoldAfterCutMs = 6000;
    static constexpr uint64_t kFecDownGapMs = 6000;

    // chosenKbps: the user's bitrate, the ceiling; startKbps: where the host streams now (0: at
    // the ceiling); fecAvailable: the host takes live FEC changes (otherwise loss goes straight
    // to the bitrate rule). The FEC ladder starts at kFecBase.
    void reset(int chosenKbps, int startKbps, Policy policy, bool fecAvailable)
    {
        m_Chosen = chosenKbps;
        m_Policy = policy;
        m_FecAvailable = fecAvailable;
        m_Bitrate = startKbps > 0 ? (std::clamp)(startKbps, (std::min)(floorKbps(), chosenKbps), chosenKbps) : chosenKbps;
        m_Fec = kFecBase;
        m_BaseRtt = -1;
        m_Regime = Regime::Clean;
        m_LastActionMs = m_LastCutMs = m_LastRaiseMs = m_LastFecDownMs = 0;
        clearStreaks();
    }

    int bitrate() const { return m_Bitrate; }
    int fec() const { return m_Fec; }
    int chosen() const { return m_Chosen; }
    Policy policy() const { return m_Policy; }
    Regime regime() const { return m_Regime; }
    float baseRtt() const { return m_BaseRtt; }

    // The policy follows the mouse mode; its floor applies from the next cut on
    void setPolicy(Policy policy) { m_Policy = policy; }
    void setFecAvailable(bool available) { m_FecAvailable = available; }

    int floorKbps() const
    {
        return m_Policy == Policy::FramePriority ? (std::max)(2000, m_Chosen / 8)
                                                 : (std::max)(4000, m_Chosen / 3);
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
            clearStreaks();
            action.bitrateKbps = m_Bitrate;
            action.reason = "ceiling";
            return action;
        }

        // Usual round trip: follows drops at once and rises only slowly (0.5 ms per second)
        if (w.rttMs > 0) {
            m_BaseRtt = m_BaseRtt < 0 ? w.rttMs : (std::min<float>)(w.rttMs, m_BaseRtt + 0.125f);
        }

        if (w.framesReceived < kMinFramesPerWindow) {
            return action;  // too few frames to judge: neither clean nor trouble
        }

        const Class c = classify(w);
        if (c == Class::Clean) {
            m_CleanStreak++;
            m_BadStreak = m_LossStreak = m_CongestedStreak = 0;
            m_LossWindowsAtMaxFec = 0;
            if (m_CleanStreak >= kDecisionWindows) {
                m_Regime = Regime::Clean;
            }
            return recover(nowMs, action);
        }

        m_CleanStreak = 0;
        m_BadStreak++;
        if (c == Class::Loss) {
            m_LossStreak++;
            m_CongestedStreak = 0;
        }
        else if (c == Class::Congested) {
            m_CongestedStreak++;
            m_LossStreak = 0;
        }
        // Grey windows (mild loss, or loss with a somewhat higher round trip) keep the streaks
        if (m_CongestedStreak >= kDecisionWindows) {
            m_Regime = Regime::Congested;
        }
        else if (m_LossStreak >= kDecisionWindows) {
            m_Regime = Regime::Loss;
        }
        if (m_Regime == Regime::Loss && (m_Fec >= kFecMax || !m_FecAvailable)) {
            m_LossWindowsAtMaxFec++;
        }

        if (m_Regime == Regime::Clean || m_BadStreak < kDecisionWindows || nowMs - m_LastActionMs < kActionGapMs) {
            return action;
        }
        if (m_Regime == Regime::Congested) {
            return cut(m_Policy == Policy::FramePriority ? 0.7f : 0.8f, nowMs, "congested", action);
        }
        if (m_Fec < kFecMax && m_FecAvailable) {
            m_Fec = m_Fec < kFecMid ? kFecMid : kFecMax;
            m_LastActionMs = nowMs;
            m_LossWindowsAtMaxFec = 0;
            action.fecPercent = m_Fec;
            action.reason = "loss";
            return action;
        }
        if (m_LossWindowsAtMaxFec >= kLossWindowsBeforeCut) {
            m_LossWindowsAtMaxFec = 0;
            return cut(0.85f, nowMs, "loss", action);
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
        if (nowMs - m_LastActionMs < kActionGapMs) {
            return action;
        }
        if (m_Bitrate < m_Chosen && m_CleanStreak >= kRaiseAfterCleanWindows &&
                nowMs - m_LastRaiseMs >= kRaiseGapMs && nowMs - m_LastCutMs >= kRaiseHoldAfterCutMs) {
            const int step = (std::max)(500, m_Chosen * 15 / 100 / 500 * 500);
            m_Bitrate = (std::min)(m_Chosen, m_Bitrate + step);
            m_LastActionMs = m_LastRaiseMs = nowMs;
            action.bitrateKbps = m_Bitrate;
            action.reason = "recover";
            return action;
        }
        if (m_Fec > kFecBase && m_FecAvailable && m_CleanStreak >= kFecDownAfterCleanWindows &&
                nowMs - m_LastFecDownMs >= kFecDownGapMs) {
            m_Fec = m_Fec > kFecMid ? kFecMid : kFecBase;
            m_LastActionMs = m_LastFecDownMs = nowMs;
            action.fecPercent = m_Fec;
            action.reason = "recover";
        }
        return action;
    }

    void clearStreaks()
    {
        m_CleanStreak = m_BadStreak = m_LossStreak = m_CongestedStreak = m_LossWindowsAtMaxFec = 0;
    }

    int m_Chosen = 0;
    int m_Bitrate = 0;
    int m_Fec = kFecBase;
    Policy m_Policy = Policy::FramePriority;
    bool m_FecAvailable = false;
    float m_BaseRtt = -1;
    Regime m_Regime = Regime::Clean;
    int m_CleanStreak = 0;
    int m_BadStreak = 0;
    int m_LossStreak = 0;
    int m_CongestedStreak = 0;
    int m_LossWindowsAtMaxFec = 0;
    uint64_t m_LastActionMs = 0;
    uint64_t m_LastCutMs = 0;
    uint64_t m_LastRaiseMs = 0;
    uint64_t m_LastFecDownMs = 0;
};
