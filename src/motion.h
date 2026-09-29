#pragma once

#include <windows.h>

#include <vector>

#include "bounds.h"
#include "region.h"

namespace fb {

// One rounded island in screen coordinates (only left/right matter; the height
// comes from the style). `id` says which island it is, so animation can match an
// island across frames and grow or shrink islands that appear or disappear.
struct Span {
    RECT rect{};
    int id = 0;
    // The edge is following explorer's slide reading by reading (keep up closely)
    // rather than jumping to a new layout (glide).
    bool trackLeft = false, trackRight = false;
};

// Which filtered edges moved by following explorer's slide in the last reading.
struct TrackedEdges {
    bool appLeft = false, appRight = false, trayLeft = false, trayRight = false, split = false;
};

// Turns raw UI Automation readings into island bounds that never cut an icon.
//
// Measured on the Windows 11 taskbar (per compositor frame, see tools/probe):
// right after buttons are added or removed, UI Automation reports the FINAL
// layout while the icons are still drawn at their OLD positions. Some 150-300 ms
// later the readings jump back to the old positions and from then on track the
// slide animation closely (within a frame). So:
//   * an edge that grows follows the reading at once (extra room never cuts);
//   * an edge that shrinks only follows readings that move continuously from
//     where the edge already is - that is the slide being tracked - and never
//     past the final layout (the first reading after the change);
//   * when readings inside the final layout start moving back out towards it,
//     the edge's own button is gone and its neighbour slides from its old slot,
//     so the final layout is safe to take right away;
//   * anything still unconfirmed is taken once readings are stable and enough
//     time has passed since the button set last changed (Windows animations
//     turned off, very large moves).
// When several apps open or close at once, explorer starts each slide from
// where the icons are drawn, so buttons can jump well outside the last layout
// (growth: followed), slides get long and fast (followed by their direction,
// not just small steps), and readings taken while buttons are rebuilt miss
// buttons (a reading that lost more than one at once is ignored).
// This is about correctness, not looks, so it runs even with FloatBar's own
// animation turned off.
class ReadingFilter {
public:
    // Folds one fresh reading, taken at `nowMs` (NowMs()), in and returns the
    // islands to display. `centre2` is the taskbar's left + right (twice its centre).
    Islands Apply(const Islands& fresh, UINT dpi, double nowMs, LONG centre2);
    // True while explorer is still moving buttons: keep reading quickly.
    bool Transitioning() const { return transition_; }
    // The readings have been stable for a while (or never will be): safe to
    // clip for the first time. The first reading can be taken in the middle of
    // explorer moving buttons (e.g. apps starting together at sign-in).
    bool Settled(double nowMs) const;
    // Edges the last reading moved by following explorer's slide (as opposed to
    // a jump): their drawn edge must keep up closely.
    const TrackedEdges& Tracked() const { return tracked_; }
    void Reset() {
        valid_ = transition_ = false;
        tracked_ = {};
    }

private:
    enum class Grow { Left, Right, None };
    enum class Move { None, Jump, Track };
    struct Edge {
        LONG value = 0;           // what the islands use
        LONG final = 0;           // final-layout candidate of the current transition
        bool finalKnown = false;  // the first reading of the change differed from `value`
        LONG prev = 0;            // previous raw reading
        int dir = 0;              // which way the raw reading last moved
    };
    void Init(Edge& e, LONG reading) {
        e.value = e.final = e.prev = reading;
        e.finalKnown = false;
        e.dir = 0;
    }
    void InitAll(const Islands& fresh);  // trust `fresh` completely; ends any transition
    Move Step(Edge& e, LONG reading, Grow grow, bool restart, bool plausible) const;

    bool valid_ = false;
    bool transition_ = false;
    TrackedEdges tracked_;
    double firstAt_ = 0;      // when the first reading came
    double lastRestart_ = 0;  // when the transition began or the button set last changed
    double lastChange_ = 0;   // when a reading last differed from the one before
    LONG continuity_ = 12;
    LONG centre2_ = 0;        // the taskbar's left + right
    LONG mirror_ = 0;         // app left + right when the buttons were last centred, else 0
    LONG settledLeft_ = 0;    // the app island's edges and split when readings last settled
    LONG settledRight_ = 0;
    LONG settledSplit_ = 0;
    bool splitArmed_ = false; // the split's slide is about to start
    Islands last_;            // the previous reading
    Islands trusted_;         // the last plausible reading: which buttons exist
    Edge appLeft_, appRight_, trayLeft_, trayRight_, split_;
};

// Moves the drawn islands towards their target at constant speeds, once per
// display frame: no easing ramps. Every island edge has its own speed, chosen
// when that edge's target changes: a jump glides over `glideMs`; a tracked edge
// (Span::trackLeft/Right) never goes slower than `trackSpeed`, so it keeps up.
class Pursuit {
public:
    void SetTarget(const std::vector<Span>& target, const SpanStyle& style, bool snap, double glideMs, double trackSpeed, double nowMs);
    // Advances to `nowMs`; returns true if anything moved.
    bool Step(double nowMs);
    bool Moving() const { return moving_; }

    std::vector<Span> Shown() const;  // rounded, zero-width islands dropped
    SpanStyle ShownStyle() const;
    const std::vector<Span>& Target() const { return target_; }
    // Replaces what is drawn without animating (e.g. the start of a morph).
    void Place(const std::vector<Span>& spans, const SpanStyle& style);
    void Clear();

private:
    struct Drawn {
        int id;
        double left, right;        // drawn edges
        double goalLeft, goalRight;
        double speedLeft, speedRight;  // px per ms
        LONG top, bottom;
        bool leaving;              // no longer in the target: shrinking into its centre
    };
    std::vector<Drawn> shown_;
    std::vector<Span> target_;
    double style_[3] = {0, 0, 0};
    double styleGoal_[3] = {0, 0, 0};
    double styleSpeed_[3] = {1, 1, 1};
    double lastMs_ = 0;
    bool moving_ = false;
};

// Current time on the high-resolution clock, in milliseconds.
double NowMs();

}  // namespace fb
