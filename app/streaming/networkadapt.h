#pragma once

#include <algorithm>
#include <cstdint>

// Calls are written (std::min)(...) so the min/max macros of <windows.h> cannot break them.

// Hermit: network adaptation for streams to a Shell host (DEV-PLAN 1.4 steps 2-4). Replaces the
// one-second automatic bitrate: fed every 250 ms with the frames that arrived, the frame numbers
// the network skipped, the longest gap between arrivals and the control stream's round trip, it
// tells two kinds of trouble apart and answers each with the tool that works for it:
// - congestion (the round trip rises well above its usual value: by more than 40 ms and 1.5x,
//   with or without loss) means the link may not carry the bitrate: the bitrate is cut once as a
//   precaution, by 30% (frame priority) or 20% (quality priority). Further cuts need a delivery
//   symptom (a dropped frame, or an arrival gap of 2.5 frame intervals) in the last two windows,
//   then every 500 ms down to the floor; without a symptom one more cut comes only when the round
//   trip is still rising (10 ms above the previous cut's), 1.5 s later at the earliest. The
//   second recording showed round trips of 50-79 ms for 4-6 s with perfect delivery that did not
//   react to cuts at all;
// - link loss (5% or more of the frames lost, at least two, while the round trip stays flat,
//   within 20 ms of usual) is not caused by the bitrate (the first recording showed the same loss
//   at 35 and at 7 Mbps), so forward error correction is raised first, from the host's default to
//   35 and 50% (at least). 50% parity puts 1.5x the bytes on the wire, so when loss goes on for
//   8 more windows (2 s) at 50% the bitrate is cut by 15%, at most twice (about 0.72x, which
//   brings the wire rate back near the original); further cuts come only from congestion.
//   Quality priority keeps a high floor and stays at 50% FEC there.
// Frame rate (quality priority on a host that takes live frame rate changes, /actions/fps): after
// the precautionary cut, the next cut a delivery symptom asks for halves the frame rate instead
// (rounded down to a multiple of 5, 15 at least: 120 -> 60, 60 -> 30), so each frame gets twice
// the bits and text stays sharp; further cuts lower the bitrate down to the floor, and when the
// symptoms go on at the floor the frame rate is halved once more (15 at least). Frame priority
// and link loss never change the frame rate; a switch to frame priority brings the stream's frame
// rate back at the next window. One action per window: the frame rate and the bitrate never
// change together.
// A decision needs 2 consecutive windows of the same kind (0.5 s); one action per 500 ms at
// most; a window of the other kind does not switch the regime until it repeats (no flapping);
// windows with fewer than 3 frames (a still picture) decide nothing and neither add to nor clear
// the clean windows.
// The usual round trip is tracked as a minimum that rises only in clean windows (0.5 ms per
// second), so a congestion that lasts is not mistaken for the new normal.
// Recovery: after 12 clean windows (3 s) the bitrate rises by 15% of the chosen value towards
// it, every 3 s; the first rise after a cut waits 6 s after that cut. Once the bitrate is back
// at the chosen value, the frame rate doubles back towards the stream's by the same rule.
// FEC goes down one step after 120 clean windows (30 s), 30 s after the last step down at the
// earliest: the third recording showed 4% loss that FEC 35 hid completely and FEC 20 turned into
// dropped frames within 0.5 s, so a step down after 6 s only found the loss again every 6.5 s.
// When loss raises FEC again within 10 s of a step down, the next step down waits twice as long
// (60 s, then 120 s at most); a step down that holds for 10 s brings the wait back to 30 s.
// A grey window (1-5% loss or a single dropped frame, or a round trip 16-40 ms above usual)
// counts half a clean window towards recovery, and 24 grey or clean windows in a row end a
// trouble regime, so a link with a steady 2% loss still recovers, slowly.
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
        int maxGapMs = 0;        // longest gap between two arrivals (a stall still going on counts)
        int rttMs = 0;           // 0 when unknown
        int fps = 60;            // the stream's frame rate: what a gap is measured against
    };

    struct Action {
        int bitrateKbps = 0;     // 0: keep the bitrate
        int fecPercent = 0;      // 0: keep FEC; fecBase(): back to the host's default
        int fps = 0;             // 0: keep the frame rate; streamFps(): back to the stream's rate
        const char* reason = nullptr;  // "congested", "loss", "recover", "floor", "ceiling" or "policy"; null without an action
    };

    static constexpr int kWindowMs = 250;
    static constexpr int kFecDefault = 20;  // the usual host default, until the host tells its own
    static constexpr int kFecMid = 35;
    static constexpr int kFecMax = 50;
    static constexpr int kMinFramesPerWindow = 3;
    static constexpr int kMinDroppedForLoss = 2;
    static constexpr int kDecisionWindows = 2;
    static constexpr float kGapSymptomIntervals = 2.5f;
    static constexpr int kRttRisingMs = 10;
    static constexpr uint64_t kActionGapMs = 500;
    static constexpr uint64_t kCongestionCutGapMs = 1500;    // between cuts without a symptom
    static constexpr int kMaxCongestionCutsWithoutSymptom = 2;
    static constexpr int kLossWindowsBeforeCut = 8;
    static constexpr int kMaxLossCuts = 2;
    static constexpr int kRaiseAfterCleanWindows = 12;
    static constexpr int kGreyWindowsEndRegime = 24;
    static constexpr uint64_t kRaiseGapMs = 3000;
    static constexpr uint64_t kRaiseHoldAfterCutMs = 6000;
    static constexpr uint64_t kFecDownHoldMs = 30000;       // clean time (and gap) before a FEC step down
    static constexpr uint64_t kFecDownHoldMaxMs = 120000;   // after relapses
    static constexpr uint64_t kFecRelapseMs = 10000;        // loss this soon after a step down doubles the hold
    static constexpr int kMinFps = 15;

    // chosenKbps: the user's bitrate, the ceiling; startKbps: where the host streams now (0: at
    // the ceiling); fecAvailable: the host takes live FEC changes and its default is known
    // (otherwise loss goes straight to the bitrate rule). The FEC ladder starts at the base.
    // streamFps: the stream's frame rate; fpsAvailable: the host takes live frame rate changes.
    void reset(int chosenKbps, int startKbps, Policy policy, bool fecAvailable, int streamFps = 60, bool fpsAvailable = false)
    {
        m_Chosen = chosenKbps;
        m_Policy = policy;
        m_FecAvailable = fecAvailable;
        m_StreamFps = streamFps;
        m_Fps = streamFps;
        m_FpsAvailable = fpsAvailable;
        m_RestoreFps = false;
        m_RegimeFpsCuts = 0;
        m_Bitrate = startKbps > 0 ? (std::clamp)(startKbps, (std::min)(floorKbps(), chosenKbps), chosenKbps) : chosenKbps;
        m_Fec = m_FecBase;
        m_BaseRtt = -1;
        m_Regime = Regime::Clean;
        m_RaiseToFloor = false;
        m_LastActionMs = m_LastCutMs = m_LastRaiseMs = m_LastFecDownMs = 0;
        m_FecHoldMs = kFecDownHoldMs;
        m_FecDownWatch = false;
        clearStreaks();
        m_LossCuts = m_CongestionCuts = 0;
        m_SymptomCur = m_SymptomPrev = false;
    }

    int bitrate() const { return m_Bitrate; }
    int fec() const { return m_Fec; }
    int fecBase() const { return m_FecBase; }
    int fps() const { return m_Fps; }
    int streamFps() const { return m_StreamFps; }
    int chosen() const { return m_Chosen; }
    Policy policy() const { return m_Policy; }
    Regime regime() const { return m_Regime; }
    float baseRtt() const { return m_BaseRtt; }
    uint64_t fecHoldMs() const { return m_FecHoldMs; }

    // The policy follows the mouse mode; its floor applies from the next cut on. Quality priority
    // taken up below its floor raises the bitrate to the floor at the next clean window; frame
    // priority taken up at a lowered frame rate brings the stream's frame rate back at the next
    // window.
    void setPolicy(Policy policy)
    {
        if (policy == m_Policy) {
            return;
        }
        m_Policy = policy;
        m_RaiseToFloor = m_Bitrate < floorKbps();
        m_RestoreFps = policy == Policy::FramePriority && m_Fps < m_StreamFps;
    }

    void setFecAvailable(bool available) { m_FecAvailable = available; }

    // A host that refused a frame rate change streams at the stream's frame rate
    void setFpsAvailable(bool available)
    {
        m_FpsAvailable = available;
        if (!available) {
            m_Fps = m_StreamFps;
            m_RestoreFps = false;
        }
    }

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

    // A delivery symptom: frames went missing, or arrivals stalled for 2.5 frame intervals (of
    // the stream's rate or the lowered one, or of the window's own rate when fewer frames come,
    // as for a still picture that Shell sends at 10-12 frames a second)
    bool hasSymptom(const Window& w) const
    {
        if (w.framesDropped >= 1) {
            return true;
        }
        const int fps = (std::min)(w.fps, m_Fps);
        const float intervalMs = (std::max)(1000.0f / (std::max)(fps, 1), (float)kWindowMs / (std::max)(w.framesReceived, 1));
        return w.maxGapMs > kGapSymptomIntervals * intervalMs;
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
            m_LossCuts = m_CongestionCuts = 0;
            action.bitrateKbps = m_Bitrate;
            action.reason = "ceiling";
            return action;
        }
        if (m_RestoreFps) {
            // Frame priority taken up at a lowered frame rate
            m_RestoreFps = false;
            m_Fps = m_StreamFps;
            m_LastActionMs = nowMs;
            action.fps = m_Fps;
            action.reason = "policy";
            return action;
        }

        if (m_FecDownWatch && nowMs - m_LastFecDownMs >= kFecRelapseMs) {
            m_FecDownWatch = false;  // the step down held: the next one waits the usual time
            m_FecHoldMs = kFecDownHoldMs;
        }

        // Usual round trip: follows drops at once; rises (0.5 ms per second) only in clean
        // windows, so trouble that lasts does not become the new normal
        if (w.rttMs > 0 && (m_BaseRtt < 0 || w.rttMs < m_BaseRtt)) {
            m_BaseRtt = w.rttMs;
        }

        if (w.framesReceived < kMinFramesPerWindow) {
            return action;  // too few frames to judge: neither clean nor trouble
        }

        m_SymptomPrev = m_SymptomCur;
        m_SymptomCur = hasSymptom(w);

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
        if (m_CongestedStreak >= kDecisionWindows && m_Regime != Regime::Congested) {
            m_Regime = Regime::Congested;
            m_CongestionCuts = m_RegimeFpsCuts = 0;
        }
        else if (m_LossStreak >= kDecisionWindows && m_Regime != Regime::Loss) {
            m_Regime = Regime::Loss;
            m_LossCuts = 0;
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
            return congestionCut(w, nowMs, action);
        }
        if (m_Fec < fecMax() && m_FecAvailable) {
            if (m_FecDownWatch) {
                // Loss again soon after a step down: that step was too early
                m_FecDownWatch = false;
                m_FecHoldMs = (std::min)(kFecDownHoldMaxMs, m_FecHoldMs * 2);
            }
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
        if (lossPct >= 5.0f && w.framesDropped >= kMinDroppedForLoss && overBase <= 20) {
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
        m_LossCuts = m_CongestionCuts = m_RegimeFpsCuts = 0;
    }

    // The congestion regime's cuts: the first as a precaution; then only with a delivery symptom
    // in the last two windows (every 500 ms), or while the round trip is still rising (once more,
    // 1.5 s later at the earliest). A round trip that rises on its own and delivers every frame
    // is left alone after that. Quality priority on a live frame rate host halves the frame rate
    // at the first cut a symptom asks for after the precaution, and once more at the floor.
    Action& congestionCut(const Window& w, uint64_t nowMs, Action& action)
    {
        const float factor = m_Policy == Policy::FramePriority ? 0.7f : 0.8f;
        const bool symptom = m_SymptomCur || m_SymptomPrev;
        bool allowed;
        if (m_CongestionCuts == 0) {
            allowed = true;
        }
        else if (symptom) {
            allowed = true;
        }
        else {
            allowed = m_CongestionCuts < kMaxCongestionCutsWithoutSymptom &&
                      nowMs - m_LastCutMs >= kCongestionCutGapMs &&
                      w.rttMs > m_BaseRtt + 40 && w.rttMs >= m_RttAtLastCut + kRttRisingMs;
        }
        if (!allowed) {
            return action;
        }
        // After the precaution (or at the floor, where it cuts nothing): the frame rate first,
        // then the bitrate down to the floor, then the frame rate once more
        const bool atFloor = m_Bitrate <= floorKbps();
        if (symptom && canLowerFps() && (m_CongestionCuts > 0 || atFloor) &&
                (m_RegimeFpsCuts == 0 || (m_RegimeFpsCuts == 1 && atFloor))) {
            m_Fps = (std::max)(kMinFps, m_Fps / 2 / 5 * 5);
            m_LastActionMs = m_LastCutMs = nowMs;
            m_CongestionCuts++;
            m_RegimeFpsCuts++;
            m_RttAtLastCut = w.rttMs;
            action.fps = m_Fps;
            action.reason = "congested";
            return action;
        }
        cut(factor, nowMs, "congested", action);
        if (action.reason != nullptr) {
            m_CongestionCuts++;
            m_RttAtLastCut = w.rttMs;
        }
        return action;
    }

    bool canLowerFps() const
    {
        return m_Policy == Policy::QualityPriority && m_FpsAvailable && m_Fps > kMinFps;
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
        if (m_Fps < m_StreamFps && m_Bitrate >= m_Chosen && m_RaiseHalves >= 2 * kRaiseAfterCleanWindows &&
                nowMs - m_LastRaiseMs >= kRaiseGapMs && nowMs - m_LastCutMs >= kRaiseHoldAfterCutMs) {
            // The bitrate is back: then the frame rate, one doubling at a time
            m_Fps = (std::min)(m_StreamFps, m_Fps * 2);
            m_LastActionMs = m_LastRaiseMs = nowMs;
            m_RaiseHalves = 0;
            action.fps = m_Fps;
            action.reason = "recover";
            return action;
        }
        if (m_Fec > m_FecBase && m_FecAvailable && m_FecHalves >= 2 * (int)(m_FecHoldMs / kWindowMs) &&
                nowMs - m_LastFecDownMs >= m_FecHoldMs) {
            m_Fec = m_Fec > fecMid() ? fecMid() : m_FecBase;
            m_LastActionMs = m_LastFecDownMs = nowMs;
            m_FecHalves = 0;
            m_FecDownWatch = true;
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
    int m_StreamFps = 60;
    int m_Fps = 60;                // the frame rate asked of the host (the stream's when not lowered)
    bool m_FpsAvailable = false;
    bool m_RestoreFps = false;     // frame priority taken up at a lowered frame rate
    int m_RegimeFpsCuts = 0;       // frame rate halvings in the congestion regime since it began
    float m_BaseRtt = -1;
    Regime m_Regime = Regime::Clean;
    int m_CleanStreak = 0;
    int m_NonBadStreak = 0;        // clean or grey windows in a row
    int m_BadStreak = 0;           // windows that are not clean in a row
    int m_LossStreak = 0;
    int m_CongestedStreak = 0;
    int m_LossWindowsAtMaxFec = 0;
    int m_LossCuts = 0;            // bitrate cuts in the loss regime since it began
    int m_CongestionCuts = 0;      // bitrate cuts in the congestion regime since it began
    int m_RttAtLastCut = 0;
    bool m_SymptomCur = false;     // delivery symptom in this window and the one before
    bool m_SymptomPrev = false;
    int m_RaiseHalves = 0;         // clean windows (2) and grey windows (1) since the last raise
    int m_FecHalves = 0;           // and since the last FEC step down
    uint64_t m_LastActionMs = 0;
    uint64_t m_LastCutMs = 0;
    uint64_t m_LastRaiseMs = 0;
    uint64_t m_LastFecDownMs = 0;
    uint64_t m_FecHoldMs = kFecDownHoldMs;  // clean time a FEC step down needs (doubles after a relapse)
    bool m_FecDownWatch = false;   // a step down in the last 10 s: loss now doubles the hold
};
