#pragma once

#include <algorithm>
#include <cstdint>

// Calls are written (std::min)(...) so the min/max macros of <windows.h> cannot break them.

// Hermit: automatic bitrate for streams to a Shell host (live bitrate changes). Fed once per
// second with the network figures of the last stats window:
// - congestion (more than 2% of frames lost, or the round trip clearly above its usual value)
//   lowers the bitrate to 80%, at most every 2 seconds, down to a fifth of the chosen bitrate
//   (at least 2 Mbps);
// - 10 calm windows (no loss, usual round trip), at least 10 seconds after the last change,
//   raise it by a tenth of the chosen bitrate (at least 1 Mbps), up to the chosen bitrate;
// - a window with too few frames to judge loss (lossPct < 0, a still picture: Shell then sends
//   only 10-12 frames a second) counts only its round trip; it neither adds to nor clears the
//   calm windows, so desktop work with pauses still recovers.
// The same rules are used by Hermit for Android (hermit/AutoBitrate.java).
class AutoBitrate
{
public:
    // startKbps: where the host streams now (0: at the ceiling)
    void reset(int maxKbps, int startKbps = 0)
    {
        m_Max = maxKbps;
        m_Current = startKbps > 0 ? (std::clamp)(startKbps, (std::min)(minKbps(), maxKbps), maxKbps) : maxKbps;
        m_LastChangeMs = 0;
        m_CalmWindows = 0;
        m_BaseRtt = -1;
    }

    int current() const { return m_Current; }

    // Returns the new bitrate in kbps, or 0 to keep the current one.
    int update(float lossPct, int rttMs, uint64_t nowMs, int maxKbps)
    {
        if (maxKbps != m_Max) {
            // The user picked another bitrate: that is the new ceiling (and the new start)
            reset(maxKbps);
            m_LastChangeMs = nowMs;
            return m_Current;
        }

        // Usual round trip: follows drops at once and rises only slowly
        if (rttMs > 0) {
            m_BaseRtt = m_BaseRtt < 0 ? rttMs : (std::min<float>)(rttMs, m_BaseRtt + 0.5f);
        }
        const bool rttHigh = m_BaseRtt > 0 && rttMs > m_BaseRtt + 40 && rttMs > m_BaseRtt * 1.5f;

        if (lossPct > 2.0f || rttHigh) {
            m_CalmWindows = 0;
            if (m_Current > minKbps() && nowMs - m_LastChangeMs >= 2000) {
                m_Current = (std::max)(minKbps(), (int)(m_Current * 0.8f) / 500 * 500);
                m_LastChangeMs = nowMs;
                return m_Current;
            }
            return 0;
        }

        if (lossPct < 0) {
            return 0;  // too few frames to tell: neither calm nor trouble
        }

        if (lossPct < 0.3f && !(m_BaseRtt > 0 && rttMs > m_BaseRtt + 20)) {
            m_CalmWindows++;
            if (m_Current < m_Max && m_CalmWindows >= 10 && nowMs - m_LastChangeMs >= 10000) {
                m_Current = (std::min)(m_Max, m_Current + (std::max)(1000, m_Max / 10));
                m_LastChangeMs = nowMs;
                m_CalmWindows = 0;
                return m_Current;
            }
        }
        else {
            m_CalmWindows = 0;
        }
        return 0;
    }

private:
    int minKbps() const { return (std::max)(2000, m_Max / 5); }

    int m_Max = 0;
    int m_Current = 0;
    uint64_t m_LastChangeMs = 0;
    int m_CalmWindows = 0;
    float m_BaseRtt = -1;
};
