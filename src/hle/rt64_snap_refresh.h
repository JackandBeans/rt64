//
// Pokemon Snap port: the rate gamescope really shows frames at.
//
// Gamescope tells a game one rate whatever Steam's refresh slider says: an OLED
// Deck reported 90 Hz with the slider at 60 (Oct 29 2026). The port took that
// as the display's rate for its frame rate cap and for how many frames it makes
// between two of the game's, so it made frames for a rate the screen could not
// show. No call the port has found gives the real rate, so it is measured here.
//
// Going down. While the port asks for more presents than the screen takes, the
// display holds each one back, and the time between presents is the display's
// own period. A window of about two seconds counts when most intervals sit
// within a millisecond of their median and most presents were held back, at a
// whole rate. A loop running behind the screen (a heavy scene) is not held back,
// so its presents say nothing about the display, however evenly they come; a
// jittery window is thrown away. Nothing here reacts to such a window, so a
// heavy scene cannot move the rate back and forth.
//
// Going up. Once the port paces by a lower rate, nothing holds its presents back
// and their spacing says nothing: a raised slider would never show. So while a
// lower rate is in force, about once a second one pair of frames waits for the
// screen, and the time between the two arriving is the display's own period
// (present queue, snapWaitForLastPresent). The wait costs no frame: the second
// frame goes out at the first one's flip, in time for the next. Most of a set of
// pairs must agree on a faster rate. A rise the presents then disprove spaces
// the pairs out further, each time.
//
// The present thread only; no locks. No dependency on the renderer, so the
// logic can be run on its own.
//

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace RT64 {
namespace snaprefresh {
    constexpr double WindowMs = 2000.0;         // a window of presents runs this long
    constexpr uint32_t WindowMost = 1024;       // or to this many presents
    constexpr uint32_t WindowLeast = 60;        // fewer are below any rate taken
    constexpr double NearMs = 1.0;              // this close to the median is steady
    constexpr double SteadyShare = 0.80;        // and most intervals must be
    constexpr double HeldPart = 0.15;           // held back for this part of its interval
    constexpr double HeldShare = 0.80;          // and most presents must be
    constexpr double StallMs = 100.0;           // an interval this long ends the window
    constexpr uint32_t LowestHz = 40;           // below this is not taken for a display
    constexpr double SameShare = 0.03;          // rates this close are the same rate
    constexpr double WholeHz = 0.25;            // this close to a whole number is a whole rate
    constexpr uint32_t SettlePresents = 30;     // after a change, the target catches up
    constexpr uint32_t FlipPairs = 8;           // pairs in a set
    constexpr uint32_t FlipAgree = 5;           // pairs of the set that must agree
    constexpr double FlipEveryMs = 1000.0;      // time between pairs
    constexpr double FlipEveryMostMs = 64000.0; // and the most it can grow to
    constexpr double DisprovedMs = 20000.0;     // a fall this soon after a rise disproves it

    struct Window {
        uint32_t count = 0;
        double medianMs = 0.0;
        double steady = 0.0;    // the share near the median
        double held = 0.0;      // the share held back by the display
        double hz = 0.0;        // from the mean of those near the median
        uint32_t wholeHz = 0;   // the rate it shows; zero when it shows none
    };

    struct Meter {
        uint32_t reported = 0;  // the rate gamescope reports, set by the present thread
        uint32_t measured = 0;  // the rate in force below it; zero when it stands
        uint32_t seenHz = 0;    // what the last change measured
        Window window;          // the last window closed
        bool windowClosed = false;  // by the last addInterval
        bool flipsClosed = false;   // by the last addFlip
        uint32_t flipsAgreed = 0;   // the largest group in that set
        double flipsMs = 0.0;       // and its mean
        bool flipPairOpen = false;  // the first frame of a pair has flipped
        double flipFirstMs = 0.0;   // when

        double intervals[WindowMost];
        uint32_t count = 0;
        uint32_t heldCount = 0;
        double total = 0.0;
        uint32_t settle = 0;
        double flips[FlipPairs];
        uint32_t flipCount = 0;
        double flipEveryMs = FlipEveryMs;
        double nextFlipMs = 0.0;
        double raisedAtMs = -1.0;

        // The rate to pace by.
        uint32_t paceBy() const {
            return active() ? measured : reported;
        }

        bool active() const {
            return (measured != 0) && (measured < reported);
        }

        // The target follows a change at the workload's next frame; until then
        // the two do not match and must not move the swap chain's switch.
        bool settling() const {
            return settle > 0;
        }

        void restartWindow() {
            count = 0;
            heldCount = 0;
            total = 0.0;
        }

        // One interval between two presents, how long the display held the later
        // one back, and whether the presents are in step with the display at all
        // (vsync on, the target at or above the rate, more than the game's own).
        // True when the rate to pace by has changed.
        bool addInterval(double intervalMs, double heldMs, bool paced, double nowMs) {
            windowClosed = false;
            if (settle > 0) {
                settle--;
                restartWindow();
                return false;
            }
            if (!paced || !(intervalMs > 0.0) || (intervalMs >= StallMs)) {
                restartWindow();
                return false;
            }
            intervals[count++] = intervalMs;
            total += intervalMs;
            if (heldMs >= (intervalMs * HeldPart)) {
                heldCount++;
            }
            if ((total < WindowMs) && (count < WindowMost)) {
                return false;
            }
            window = judge();
            windowClosed = true;
            restartWindow();
            return (window.wholeHz != 0) && adopt(window.wholeHz, nowMs);
        }

        // Whether a pair of frames should wait for the screen now.
        bool flipSampleDue(double nowMs) const {
            return active() && (nowMs >= nextFlipMs);
        }

        // A wait that did not end in a flip: the next try is a while off.
        void putOffFlips(double nowMs) {
            flipPairOpen = false;
            nextFlipMs = nowMs + flipEveryMs;
        }

        // The time between a pair's two flips. True when the rate to pace by has
        // changed.
        bool addFlip(double flipMs, double nowMs) {
            flipsClosed = false;
            flipPairOpen = false;
            nextFlipMs = nowMs + flipEveryMs;
            if (!active() || !(flipMs > 0.0)) {
                return false;
            }
            flips[flipCount++] = flipMs;
            if (flipCount < FlipPairs) {
                return false;
            }
            flipCount = 0;
            flipsClosed = true;
            // The largest group within the span a steady interval may have; a
            // second frame that misses its flip lands in another group, at a
            // multiple of the period, and says nothing.
            std::sort(flips, flips + FlipPairs);
            uint32_t best = 0;
            uint32_t bestAt = 0;
            for (uint32_t i = 0; i < FlipPairs; i++) {
                uint32_t j = i;
                while ((j < FlipPairs) && ((flips[j] - flips[i]) <= (NearMs * 2.0))) {
                    j++;
                }
                if ((j - i) > best) {
                    best = j - i;
                    bestAt = i;
                }
            }
            double sum = 0.0;
            for (uint32_t i = bestAt; i < (bestAt + best); i++) {
                sum += flips[i];
            }
            flipsAgreed = best;
            flipsMs = sum / double(best);
            if (best < FlipAgree) {
                return false;
            }
            const uint32_t hz = uint32_t(std::lround(1000.0 / flipsMs));
            if (double(hz) <= (double(measured) * (1.0 + SameShare))) {
                return false;
            }
            return adopt(hz, nowMs);
        }

    private:
        // Reorders the window's intervals; they are not used again.
        Window judge() {
            Window w;
            w.count = count;
            if (count < WindowLeast) {
                return w;
            }
            std::nth_element(intervals, intervals + (count / 2), intervals + count);
            w.medianMs = intervals[count / 2];
            uint32_t near = 0;
            double sum = 0.0;
            for (uint32_t i = 0; i < count; i++) {
                if (std::fabs(intervals[i] - w.medianMs) <= NearMs) {
                    near++;
                    sum += intervals[i];
                }
            }
            w.steady = double(near) / double(count);
            w.held = double(heldCount) / double(count);
            w.hz = (near > 0) ? (1000.0 / (sum / double(near))) : 0.0;
            const double whole = std::round(w.hz);
            if ((w.steady >= SteadyShare) && (w.held >= HeldShare) && (std::fabs(w.hz - whole) <= WholeHz) && (whole >= double(LowestHz))) {
                w.wholeHz = uint32_t(whole);
            }
            return w;
        }

        bool adopt(uint32_t hz, double nowMs) {
            if ((reported == 0) || (double(hz) > (double(reported) * (1.0 + SameShare)))) {
                return false;
            }
            const uint32_t before = paceBy();
            if (std::fabs(double(hz) - double(before)) <= (double(before) * SameShare)) {
                return false;
            }
            if ((hz < before) && (raisedAtMs >= 0.0) && ((nowMs - raisedAtMs) < DisprovedMs)) {
                flipEveryMs = std::min(flipEveryMs * 2.0, FlipEveryMostMs);
            }
            raisedAtMs = (hz > before) ? nowMs : -1.0;
            measured = (std::fabs(double(hz) - double(reported)) <= (double(reported) * SameShare)) ? 0 : hz;
            seenHz = hz;
            settle = SettlePresents;
            restartWindow();
            flipCount = 0;
            flipPairOpen = false;
            nextFlipMs = nowMs + flipEveryMs;
            return true;
        }
    };
}
}
