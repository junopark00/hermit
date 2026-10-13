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
//   precaution, by 30% (frame priority, and quality priority on a live frame rate host) or 20%
//   (quality priority otherwise). Further cuts need a delivery
//   symptom (a dropped frame, or an arrival gap of 2.5 frame intervals and 1.5x the 90th
//   percentile of the gaps in the last 40 clean windows, once there are 12: a desktop sends
//   frames only when the screen changes, and the fourth recording showed the plain gap rule
//   firing in 11-30% of normal windows) in the last two windows, then every 500 ms down to the
//   floor; without a symptom one more cut comes only when the round trip is still rising (10 ms
//   above the previous cut's), 1.5 s later at the earliest. After two cuts in a congestion (the
//   precaution included) a symptom counts only with proof that cutting helps: dropped frames in
//   the last two windows, a severe stall in them (an arrival gap of 250 ms, twice the 90th
//   percentile of the clean gaps and 4 frame intervals or more: at 10 fps a plain symptom is
//   already 250 ms), or a round trip at least 10 ms below the one at the
//   first cut (the cuts are draining a queue). The fourth recording cut from 35 to 4 Mbps on gap
//   symptoms alone while the round trip stayed at 95-110 ms, a path delay that no cut could
//   change; the fifth held 28 Mbps for 4.2 s over a 20 Mbps cap while arrivals stalled for
//   339-433 ms, because the 40 ms path delay hid the queue from the round trip. The second
//   recording showed round trips of 50-79 ms for 4-6 s with perfect delivery that did not react
//   to cuts at all;
// - link loss (5% or more of the frames lost, at least two, while the round trip stays flat,
//   within 20 ms of usual) is not caused by the bitrate (the first recording showed the same loss
//   at 35 and at 7 Mbps), so forward error correction is raised first, from the host's default to
//   35 and 50% (at least). 50% parity puts 1.5x the bytes on the wire, so when loss goes on for
//   8 more windows (2 s) at 50% the bitrate is cut by 15%, at most twice (about 0.72x, which
//   brings the wire rate back near the original); further cuts come only from congestion.
//   Quality priority keeps a high floor and stays at 50% FEC there.
// Frame rate (quality priority on a host that takes live frame rate changes, /actions/fps): the
// frame rate follows the bitrate, so each frame keeps about the bits it had at the chosen
// bitrate: fps = the stream's x bitrate / chosen, rounded down to a multiple of 5, between 10
// (Shell's minimum) and the stream's (the stream's itself at the chosen bitrate or within 5 of
// it, so 144 and 24 fps streams come back whole). Every congestion cut carries both in one
// action (35000 at 60 fps: 24500 at 40, 17000 at 25, 11500 at 15, 8000 at 10, then the bitrate
// alone down to the floor), so bytes per second drop at once and the picture stays sharp until
// the frame rate is at 10; only then does a cut cost quality. The fifth recording showed why
// the frame rate alone does not help: at the same bitrate the frames only grow, and the queue
// over a bandwidth cap stays. Frame priority and link loss never change the frame rate; a
// switch to frame priority brings the stream's frame rate back at the next window, and a switch
// back to quality priority (or a host that takes live frame rate changes again) brings the
// formula's frame rate ("policy"; not while a congestion is still ending, where a fast recovery
// may bring everything back first). A new chosen bitrate brings the stream's rate back; during a
// congestion the bitrate stays where it is (or goes down to the new chosen one) and the frame
// rate stays at or below the formula's for the new chosen one, so frames never grow; the
// recovery lifts both. One action per window.
// A decision needs 2 consecutive windows of the same kind (0.5 s); one action per 500 ms at
// most; a window of the other kind does not switch the regime until it repeats (no flapping);
// windows with fewer than 3 frames received (2 at a frame rate below 12: 10 fps brings 2-3 a
// window) decide nothing and neither add to nor clear the clean windows (a still picture),
// unless 2 frames or more were lost in them: a stream that
// collapses (2 frames arrive, 6 are lost) is judged, a still picture with a single lost frame
// is not (the first recording's loss runs had many such windows, and judging them only kept the
// loss regime going for one more cut).
// The usual round trip is tracked as a minimum that rises only in clean windows (0.5 ms per
// second), so a congestion that lasts is not mistaken for the new normal.
// Fast recovery: a congestion regime that ends with 2 clean windows in a row without a dropped
// frame (not one that turns into link loss or ends after 24 grey windows) brings back the
// bitrate and frame rate from before the congestion in one action ("fast-recover") when the
// round trip 2 s after our last cut was still congested and had not fallen by more
// than a quarter of its rise above usual at that action, 10 ms at least (path delay jitters by
// 10-20 ms, and the rising round trip rule often cuts on a high sample). It fell on its own
// later: the cause outside went away, as when the fourth recording's 100 ms path delay stopped
// and the gradual recovery then took 15-25 s.
// For 2 s after it, a congested window or one with 2 dropped frames or more reverts to the
// values before the jump in one action ("probe-failed"; the frame rate only while it follows
// the bitrate, the bitrate not above the chosen one), and fast recovery stays off for
// 60 s; the gradual recovery goes on from there. A round trip that is back within 2 s of our
// cut, or already falling 2 s after it, means the cuts drained our own queue: gradual recovery.
// A new chosen bitrate ends the episode (and a probe in progress).
// Recovery: after 12 clean windows (3 s) the bitrate rises by 15% of the chosen value towards
// it, every 3 s; the first rise after a cut waits 6 s after that cut. Where the frame rate
// follows the bitrate, each step brings it up by the same rule in the same action (never down:
// the fourth recording dropped 10 frames when 35 Mbps at 15 fps made 290 KB frames).
// FEC goes down one step after 120 clean windows (30 s), 30 s after the last step down at the
// earliest: the third recording showed 4% loss that FEC 35 hid completely and FEC 20 turned into
// dropped frames within 0.5 s, so a step down after 6 s only found the loss again every 6.5 s.
// Only windows with frames count, so a still desktop keeps FEC up longer. When loss raises FEC
// again within 10 s of a step down, the next step down waits twice as long (60 s, then 120 s at
// most); a step down to the base that holds for 10 s brings the wait back to 30 s (one from 50
// to 35 that holds does not). After such a relapse the step that held before gets 1.25 s to take
// effect on the host before loss raises FEC further (loss in the meantime may still come from
// before the change), so a relapse costs two requests, not three.
// A grey window (1-5% loss or a single dropped frame, or a round trip 16-40 ms above usual)
// counts half a clean window towards recovery, and 24 grey or clean windows in a row end a
// trouble regime, so a link with a steady 2% loss still recovers, slowly.
// Floors: frame priority, and quality priority on a live frame rate host (the frame rate carries
// the reduction there), max(2 Mbps, chosen/8); quality priority otherwise max(4 Mbps, chosen/3);
// rounded down to 500 kbps. A switch to quality priority below its floor raises the bitrate to
// the floor at the next clean window.
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
        int fps = 60;            // the frame rate the host streams at: what a gap is measured against
    };

    struct Action {
        int bitrateKbps = 0;     // 0: keep the bitrate
        int fecPercent = 0;      // 0: keep FEC; fecBase(): back to the host's default
        int fps = 0;             // 0: keep the frame rate; streamFps(): back to the stream's rate
        // "congested", "loss", "recover", "floor", "ceiling", "policy", "fast-recover" or
        // "probe-failed"; null without an action
        const char* reason = nullptr;
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
    static constexpr uint64_t kFecRelapseSettleMs = 1250;   // after a relapse raise (a host 4 windows late)
    static constexpr int kMinFps = 10;                      // Shell's minimum
    static constexpr int kGapSamples = 40;                  // clean windows: the desktop's own gaps
    static constexpr int kGapMinSamples = 12;
    static constexpr float kGapSymptomOverP90 = 1.5f;
    static constexpr int kActionsBeforeProof = 2;           // then a symptom needs proof
    static constexpr int kSevereStallMs = 250;              // a stall that is proof (also 2x usual, 4 intervals)
    static constexpr int kRttFallingMs = 10;
    static constexpr uint64_t kFastRecoveryQuietMs = 2000;  // our last cut before the round trip fell
    static constexpr uint64_t kProbeMs = 2000;
    static constexpr uint64_t kFastRecoveryOffMs = 60000;   // after a failed probe

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
        m_RestoreFps = m_FollowFps = false;
        m_Bitrate = startKbps > 0 ? (std::clamp)(startKbps, (std::min)(floorKbps(), chosenKbps), chosenKbps) : chosenKbps;
        m_Fec = m_FecBase;
        m_BaseRtt = -1;
        m_Regime = Regime::Clean;
        m_RaiseToFloor = false;
        m_LastActionMs = m_LastCutMs = m_LastRaiseMs = m_LastFecDownMs = 0;
        m_FecHoldMs = kFecDownHoldMs;
        m_FecDownWatch = false;
        m_FecSettleUntilMs = 0;
        clearStreaks();
        m_LossCuts = m_CongestionCuts = 0;
        m_SymptomCur = m_SymptomPrev = false;
        m_DropCur = m_DropPrev = false;
        m_StallCur = m_StallPrev = false;
        m_GapCount = m_GapNext = 0;
        m_RttAtFirstAction = 0;
        m_PreKbps = 0;
        m_FastArmed = false;
        m_FirstCleanMs = 0;
        m_QuietRttMs = -1;
        m_ProbeUntilMs = 0;
        m_FastOffUntilMs = 0;
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
        m_FollowFps = fpsFollows() && fpsFor(m_Bitrate) < m_Fps;
    }

    void setFecAvailable(bool available) { m_FecAvailable = available; }

    // A host that refused a frame rate change streams at the stream's frame rate (and quality
    // priority's higher floor applies again)
    void setFpsAvailable(bool available)
    {
        if (available == m_FpsAvailable) {
            return;
        }
        m_FpsAvailable = available;
        if (!available) {
            m_Fps = m_StreamFps;
            m_RestoreFps = m_FollowFps = false;
            m_RaiseToFloor = m_Bitrate < floorKbps();
        }
        else {
            m_FollowFps = fpsFollows() && fpsFor(m_Bitrate) < m_Fps;
        }
    }

    // The frame rate that keeps each frame's bits at what the chosen bitrate gives at the stream's
    // rate (quality priority on a live frame rate host); the stream's otherwise
    int fpsFor(int kbps) const
    {
        if (!fpsFollows() || m_Chosen <= 0 || kbps >= m_Chosen) {
            return m_StreamFps;
        }
        const int fps = (int)((int64_t)m_StreamFps * kbps / m_Chosen) / 5 * 5;
        if (m_StreamFps - fps < 5) {
            return m_StreamFps;   // 140 of 144: the stream's
        }
        return (std::clamp)(fps, (std::min)(kMinFps, m_StreamFps), m_StreamFps);
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
        return m_Policy == Policy::FramePriority || fpsFollows() ? (std::max)(2000, m_Chosen / 8 / 500 * 500)
                                                                 : (std::max)(4000, m_Chosen / 3 / 500 * 500);
    }

    // A delivery symptom: frames went missing, or arrivals stalled for 2.5 frame intervals (of
    // the rate the host streams at, or of the window's own rate when fewer frames come, as for a
    // still picture that Shell sends at 10-12 frames a second) and, once 12 clean windows are
    // known, for 1.5x the 90th percentile of their longest gaps (the desktop's own pauses)
    bool hasSymptom(const Window& w) const
    {
        if (w.framesDropped >= 1) {
            return true;
        }
        const float intervalMs = (std::max)(1000.0f / (std::max)(w.fps, 1), (float)kWindowMs / (std::max)(w.framesReceived, 1));
        float thresholdMs = kGapSymptomIntervals * intervalMs;
        if (m_GapCount >= kGapMinSamples) {
            thresholdMs = (std::max)(thresholdMs, kGapSymptomOverP90 * usualGapMs());
        }
        return w.maxGapMs > thresholdMs;
    }

    // The 90th percentile of the longest gaps in the last 40 clean windows (0 without any)
    int usualGapMs() const
    {
        if (m_GapCount == 0) {
            return 0;
        }
        int sorted[kGapSamples];
        std::copy(m_GapRing, m_GapRing + m_GapCount, sorted);
        std::sort(sorted, sorted + m_GapCount);
        return sorted[(std::min)(m_GapCount - 1, m_GapCount * 9 / 10)];
    }

    // One 250 ms window ending at nowMs. chosenKbps: the user's current choice; a new one
    // restarts the bitrate there (the ceiling moved; during a congestion the bitrate only goes
    // down to it) and keeps the FEC step.
    Action update(const Window& w, uint64_t nowMs, int chosenKbps)
    {
        Action action;
        if (chosenKbps != m_Chosen) {
            // The stream's rate at the new ceiling. During a congestion a higher ceiling does not
            // push more bytes into the link: the bitrate stays (or goes down to a lower ceiling)
            // and the frame rate keeps each frame's bits for the new chosen bitrate
            const bool congested = m_Regime == Regime::Congested;
            m_Chosen = chosenKbps;
            m_Bitrate = congested ? (std::min)(m_Bitrate, chosenKbps) : chosenKbps;
            const int fps = congested ? (std::min)(m_Fps, fpsFor(m_Bitrate)) : fpsFor(m_Bitrate);
            if (fpsFollows() && m_Fps != fps) {
                m_Fps = fps;
                action.fps = m_Fps;
            }
            m_LastActionMs = nowMs;
            m_LastCutMs = 0;
            m_RaiseToFloor = false;
            clearStreaks();
            m_LossCuts = m_CongestionCuts = 0;
            // The user's new value is where things start again: no episode to come back to
            m_PreKbps = 0;
            m_FastArmed = false;
            m_ProbeUntilMs = 0;
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

        if (m_ProbeUntilMs != 0 && nowMs > m_ProbeUntilMs) {
            // The fast recovery held: the congestion episode is over
            m_ProbeUntilMs = 0;
            m_PreKbps = 0;
        }

        if (m_FecDownWatch && nowMs - m_LastFecDownMs >= kFecRelapseMs) {
            m_FecDownWatch = false;
            if (m_Fec == m_FecBase) {
                m_FecHoldMs = kFecDownHoldMs;  // the step down to the base held: the usual wait again
            }
        }

        // Usual round trip: follows drops at once; rises (0.5 ms per second) only in clean
        // windows, so trouble that lasts does not become the new normal
        if (w.rttMs > 0 && (m_BaseRtt < 0 || w.rttMs < m_BaseRtt)) {
            m_BaseRtt = w.rttMs;
        }

        const int minFrames = (std::min)(kMinFramesPerWindow, (std::max)(2, w.fps / 4));
        if (w.framesReceived < minFrames && w.framesDropped < kMinDroppedForLoss) {
            return action;  // a still picture: too few frames to judge, neither clean nor trouble
        }

        m_SymptomPrev = m_SymptomCur;
        m_SymptomCur = hasSymptom(w);
        m_DropPrev = m_DropCur;
        m_DropCur = w.framesDropped >= 1;
        m_StallPrev = m_StallCur;
        m_StallCur = w.maxGapMs >= (std::max)((std::max)(kSevereStallMs, 2 * usualGapMs()),
                                              4 * 1000 / (std::max)(w.fps, 1));

        const Class c = classify(w);
        if (c == Class::Clean) {
            if (w.rttMs > 0) {
                m_BaseRtt = (std::min<float>)(w.rttMs, m_BaseRtt + 0.125f);
            }
            m_GapRing[m_GapNext] = w.maxGapMs;   // the desktop's own gaps
            m_GapNext = (m_GapNext + 1) % kGapSamples;
            m_GapCount = (std::min)(kGapSamples, m_GapCount + 1);
            m_CleanStreak++;
            if (m_CleanStreak == 1) {
                m_FirstCleanMs = nowMs;
            }
            m_NonBadStreak++;
            m_RaiseHalves += 2;
            m_FecHalves += 2;
            m_BadStreak = m_LossStreak = m_CongestedStreak = 0;
            m_LossWindowsAtMaxFec = 0;
            const bool wasCongested = m_Regime == Regime::Congested;
            if (m_CleanStreak >= kDecisionWindows || m_NonBadStreak >= kGreyWindowsEndRegime) {
                endRegime();
            }
            // The congestion ended with clean windows long after our last cut: the round trip
            // fell on its own
            if (wasCongested && m_Regime == Regime::Clean && m_CleanStreak >= kDecisionWindows &&
                    !m_DropCur && !m_DropPrev && episodeOpen() && nowMs >= m_FastOffUntilMs &&
                    m_FirstCleanMs - kWindowMs >= lastCutMs() + kFastRecoveryQuietMs &&
                    m_QuietRttMs >= 0 &&
                    m_QuietRttMs >= m_RttAtLastCut - (std::max)((float)kRttFallingMs, (m_RttAtLastCut - m_BaseRtt) / 4)) {
                m_FastArmed = true;
            }
            if (m_FastArmed && nowMs - m_LastActionMs >= kActionGapMs) {
                return fastRecover(nowMs, action);
            }
            if (followFps(nowMs, action)) {
                return action;
            }
            if (m_RaiseToFloor && nowMs - m_LastActionMs >= kActionGapMs) {
                // Quality priority taken up below its floor: one step up to the floor
                m_RaiseToFloor = false;
                m_Bitrate = (std::min)(m_Chosen, floorKbps());
                m_LastActionMs = m_LastRaiseMs = nowMs;
                action.bitrateKbps = m_Bitrate;
                raiseFps(action);
                action.reason = "floor";
                return action;
            }
            return recover(nowMs, action);
        }

        m_CleanStreak = 0;
        m_FastArmed = false;
        m_BadStreak++;
        if (c == Class::Congested && m_QuietRttMs < 0 && nowMs >= lastCutMs() + kFastRecoveryQuietMs) {
            m_QuietRttMs = w.rttMs;  // the round trip 2 s after our last cut: still up, or falling?
        }
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
            m_CongestionCuts = 0;
            if (!episodeOpen()) {
                // A new congestion episode: what fast recovery brings back
                m_PreKbps = m_Bitrate;
                m_PreFps = m_Fps;
            }
        }
        else if (m_LossStreak >= kDecisionWindows && m_Regime != Regime::Loss) {
            m_Regime = Regime::Loss;
            m_LossCuts = 0;
        }
        if (c == Class::Loss && m_Regime == Regime::Loss && (m_Fec >= fecMax() || !m_FecAvailable)) {
            m_LossWindowsAtMaxFec++;  // loss going on at the top of the ladder (grey windows do not count)
        }
        if (m_ProbeUntilMs != 0 && (c == Class::Congested || w.framesDropped >= 2)) {
            return probeFailed(nowMs, action);
        }

        if (followFps(nowMs, action)) {
            return action;
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
        if (m_Fec < fecMax() && m_FecAvailable && nowMs >= m_FecSettleUntilMs) {
            if (m_FecDownWatch) {
                // Loss again soon after a step down: that step was too early. The step it goes
                // back to held before: give it time to reach the host before going higher.
                m_FecDownWatch = false;
                m_FecHoldMs = (std::min)(kFecDownHoldMaxMs, m_FecHoldMs * 2);
                m_FecSettleUntilMs = nowMs + kFecRelapseSettleMs;
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

    uint64_t lastCutMs() const { return m_LastCutMs; }

    // Quality priority taken up (or live frame rate changes back) at a lowered bitrate: the frame
    // rate that keeps each frame's bits. Not while a congestion is still on or ending (its next
    // cut sets the frame rate anyway, and a fast recovery may bring everything back first).
    bool followFps(uint64_t nowMs, Action& action)
    {
        if (!m_FollowFps || m_Regime == Regime::Congested) {
            return false;
        }
        m_FollowFps = false;
        if (!fpsFollows() || fpsFor(m_Bitrate) >= m_Fps) {
            return false;
        }
        m_Fps = fpsFor(m_Bitrate);
        m_LastActionMs = nowMs;
        action.fps = m_Fps;
        action.reason = "policy";
        return true;
    }

    // A congestion episode that has not come back to the values from before it
    bool episodeOpen() const
    {
        return m_PreKbps > 0 && (m_Bitrate < (std::min)(m_PreKbps, m_Chosen) || m_Fps < m_PreFps);
    }

    Action& fastRecover(uint64_t nowMs, Action& action)
    {
        m_FastArmed = false;
        m_ProbeKbps = m_Bitrate;
        m_ProbeFps = m_Fps;
        const int target = (std::min)(m_PreKbps, m_Chosen);
        if (target > m_Bitrate) {
            m_Bitrate = target;
            action.bitrateKbps = target;
        }
        if (m_PreFps > m_Fps) {
            m_Fps = m_PreFps;
            action.fps = m_Fps;
        }
        m_LastActionMs = m_LastRaiseMs = nowMs;
        m_RaiseHalves = 0;
        m_ProbeUntilMs = nowMs + kProbeMs;
        action.reason = "fast-recover";
        return action;
    }

    // The jump was too early: back to the values before it (the bitrate not above the chosen
    // one; the frame rate only while quality priority can lower it), gradual recovery from there
    Action& probeFailed(uint64_t nowMs, Action& action)
    {
        m_ProbeUntilMs = 0;
        m_FastOffUntilMs = nowMs + kFastRecoveryOffMs;
        const int kbps = (std::min)(m_ProbeKbps, m_Chosen);
        if (m_Bitrate != kbps) {
            m_Bitrate = kbps;
            action.bitrateKbps = m_Bitrate;
        }
        if (m_Fps != m_ProbeFps && fpsFollows()) {
            m_Fps = m_ProbeFps;
            action.fps = m_Fps;
        }
        m_LastActionMs = m_LastCutMs = nowMs;
        m_QuietRttMs = -1;
        action.reason = "probe-failed";
        return action;
    }

    void endRegime()
    {
        m_Regime = Regime::Clean;
        m_LossStreak = m_CongestedStreak = m_LossWindowsAtMaxFec = 0;
        m_LossCuts = m_CongestionCuts = 0;
    }

    // The congestion regime's cuts: the first as a precaution; then only with a delivery symptom
    // in the last two windows (every 500 ms), or while the round trip is still rising (once more,
    // 1.5 s later at the earliest). A round trip that rises on its own and delivers every frame
    // is left alone after that. Where the frame rate follows the bitrate, each cut carries both.
    Action& congestionCut(const Window& w, uint64_t nowMs, Action& action)
    {
        const float factor = m_Policy == Policy::FramePriority || fpsFollows() ? 0.7f : 0.8f;
        // After two cuts a symptom needs proof that cutting helps: dropped frames or a severe
        // stall, or a round trip that follows the cuts down
        const bool drops = m_DropCur || m_DropPrev || m_StallCur || m_StallPrev;
        const bool rttFalling = m_CongestionCuts > 0 && w.rttMs > 0 && w.rttMs <= m_RttAtFirstAction - kRttFallingMs;
        const bool symptom = (m_SymptomCur || m_SymptomPrev) &&
                             (m_CongestionCuts < kActionsBeforeProof || drops || rttFalling);
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
        cut(factor, nowMs, "congested", action);
        if (action.reason != nullptr) {
            // The frame rate follows the bitrate down: bytes per second drop, frames keep their bits
            const int fps = fpsFor(m_Bitrate);
            if (fps < m_Fps) {
                m_Fps = fps;
                action.fps = fps;
            }
            if (m_CongestionCuts == 0) {
                m_RttAtFirstAction = w.rttMs;
            }
            m_CongestionCuts++;
            m_RttAtLastCut = w.rttMs;
        }
        return action;
    }

    // Quality priority on a host that takes live frame rate changes: the frame rate follows the
    // bitrate
    bool fpsFollows() const { return m_Policy == Policy::QualityPriority && m_FpsAvailable; }

    // A raise brings the frame rate up with the bitrate (never down)
    void raiseFps(Action& action)
    {
        const int fps = fpsFor(m_Bitrate);
        if (fpsFollows() && fps > m_Fps) {
            m_Fps = fps;
            action.fps = fps;
        }
    }

    Action& cut(float factor, uint64_t nowMs, const char* reason, Action& action)
    {
        const int next = (std::max)(floorKbps(), (int)(m_Bitrate * factor) / 500 * 500);
        if (next < m_Bitrate) {
            m_Bitrate = next;
            m_LastActionMs = m_LastCutMs = nowMs;
            m_QuietRttMs = -1;
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
            raiseFps(action);
            action.reason = "recover";
            return action;
        }
        if (fpsFollows() && m_Fps < fpsFor(m_Bitrate) && m_RaiseHalves >= 2 * kRaiseAfterCleanWindows &&
                nowMs - m_LastRaiseMs >= kRaiseGapMs && nowMs - m_LastCutMs >= kRaiseHoldAfterCutMs) {
            // A frame rate below what the bitrate gives (a ceiling during a congestion, or a
            // switch back to quality priority before frame priority's restore ran): up by itself
            m_Fps = fpsFor(m_Bitrate);
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
    bool m_FollowFps = false;      // quality priority taken up at a lowered bitrate
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
    bool m_DropCur = false;        // dropped frames in this window and the one before
    bool m_DropPrev = false;
    bool m_StallCur = false;       // a severe stall in this window and the one before
    bool m_StallPrev = false;
    int m_RttAtFirstAction = 0;    // the round trip at the congestion regime's first action
    int m_GapRing[kGapSamples] = {};  // the longest gaps of the last clean windows
    int m_GapCount = 0;
    int m_GapNext = 0;
    int m_PreKbps = 0;             // before the congestion episode (0: no episode)
    int m_PreFps = 0;
    bool m_FastArmed = false;      // fast recovery due at the next window the action gap allows
    uint64_t m_FirstCleanMs = 0;   // the end of the clean streak's first window
    int m_QuietRttMs = -1;         // the first congested round trip 2 s after our last cut (-1: none)
    uint64_t m_ProbeUntilMs = 0;   // after a fast recovery (0: no probe)
    int m_ProbeKbps = 0;           // the values before the jump
    int m_ProbeFps = 0;
    uint64_t m_FastOffUntilMs = 0; // fast recovery off until then (after a failed probe)
    int m_RaiseHalves = 0;         // clean windows (2) and grey windows (1) since the last raise
    int m_FecHalves = 0;           // and since the last FEC step down
    uint64_t m_LastActionMs = 0;
    uint64_t m_LastCutMs = 0;
    uint64_t m_LastRaiseMs = 0;
    uint64_t m_LastFecDownMs = 0;
    uint64_t m_FecHoldMs = kFecDownHoldMs;  // clean time a FEC step down needs (doubles after a relapse)
    bool m_FecDownWatch = false;   // a step down in the last 10 s: loss now doubles the hold
    uint64_t m_FecSettleUntilMs = 0;  // no FEC raise before this (after a relapse raise)
};
