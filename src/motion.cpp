#include "motion.h"

#include <algorithm>
#include <cmath>

namespace fb {
namespace {

// Largest per-read step of explorer's own slide: 22-44 px over ~200 ms, read
// about every 11 ms. The "final layout first" readings jump by a whole half or
// full button (22+ px), so they never pass as continuous.
constexpr int kContinuityLogicalPx = 12;
// How long readings must stay unchanged to end a transition. Explorer's slide
// moves its last pixels up to ~90 ms apart, and reads come every ~11 ms, so a
// read count would end transitions mid-slide.
constexpr double kStableMs = 150;
// Unconfirmed shrinks wait at least this long after the button set last
// changed; the "final layout first" phase lasted up to ~270 ms in measurements
// (bursts of apps opening or closing included).
constexpr double kMinHoldMs = 450;
// A reading that lost more than one button at once must stay unchanged this
// long before it is believed.
constexpr double kDoubtfulStableMs = 600;
constexpr double kMaxTransitionMs = 2000;
// A tracked edge reaches each new reading within this long (about one read
// interval), so it keeps up with slides of any speed. A shrinking one can't
// cut an icon by trailing, so it smooths out readings that lag and then jump.
constexpr double kCatchUpMs = 12;
constexpr double kShrinkCatchUpMs = 24;
// The icons either side of the Start gap have 11 px of button margin; the gap
// takes 4 of them on each side, leaving 7 px of play for its readings' lag.
constexpr int kSplitLeadLogicalPx = 4;

bool Structural(const Islands& a, const Islands& b) {
    return a.appCount != b.appCount || a.trayCount != b.trayCount || a.hasTray != b.hasTray || a.hasSplit != b.hasSplit ||
           a.extras.size() != b.extras.size();
}

bool SameReading(const Islands& a, const Islands& b) {
    if (a.app.left != b.app.left || a.app.right != b.app.right) return false;
    if (a.hasTray && (a.tray.left != b.tray.left || a.tray.right != b.tray.right)) return false;
    if (a.hasSplit && a.split != b.split) return false;
    for (size_t i = 0; i < a.extras.size() && i < b.extras.size(); ++i) {
        if (!EqualRect(&a.extras[i], &b.extras[i])) return false;
    }
    return true;
}

// Moves `value` towards `goal` by at most `maxStep`; returns true if it moved.
bool Toward(double& value, double goal, double maxStep) {
    if (value == goal) return false;
    if (std::abs(goal - value) <= maxStep) value = goal;
    else value += goal > value ? maxStep : -maxStep;
    return true;
}

const Span* FindSpan(const std::vector<Span>& spans, int id) {
    for (const Span& s : spans) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

}  // namespace

double NowMs() {
    static const double ticksPerMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / ticksPerMs;
}

// ------------------------------------------------------------------ ReadingFilter

ReadingFilter::Move ReadingFilter::Step(Edge& e, LONG r, Grow grow, bool restart, bool plausible) const {
    // Taken while buttons were being rebuilt: some are missing and others can be
    // anywhere (even at their final slots long before they get there).
    if (!plausible) return Move::None;
    if (restart) {
        // The first reading of a change: the final layout if it differs from what
        // is drawn (see the class comment); no information if it doesn't.
        e.final = r;
        e.finalKnown = r != e.value;
    }
    Move move = Move::None;
    const int dir = r > e.prev ? 1 : r < e.prev ? -1 : 0;
    // Explorer's slide: small steps, starting from where the edge already is.
    const bool continuous = std::abs(r - e.prev) <= continuity_ && std::abs(e.prev - e.value) <= continuity_;
    // A long, fast slide (many apps at once) keeps going the same way in bigger
    // steps from the reading the edge last followed. A reading of a changed
    // button set is a new final layout, never a step of a slide.
    const bool sliding =
        dir != 0 && (continuous || (!restart && e.value == e.prev && dir == e.dir && std::abs(r - e.prev) <= 4 * continuity_));
    if (grow == Grow::None) {
        if (r != e.value && continuous) {
            e.value = r;
            move = Move::Track;
        }
    } else {
        const int out = grow == Grow::Left ? -1 : 1;  // the way that adds room
        auto inside = [&](LONG v) { return e.finalKnown && (v - e.final) * out < 0; };
        // The neighbour of a removed button sliding from its old slot (inside
        // the final layout) back out towards it. The final layout alone can
        // arrive in steps that also land inside it, but never moves outward.
        const bool neighbourSlides = inside(r) && inside(e.prev) && dir == out && std::abs(r - e.prev) <= continuity_;
        if ((r - e.value) * out > 0) {
            e.value = r;  // more room never cuts an icon
            move = sliding ? Move::Track : Move::Jump;
        } else if (r != e.value && sliding) {
            // Follow the slide, but not past the final layout.
            const LONG v = !e.finalKnown ? r : grow == Grow::Left ? std::min(r, e.final) : std::max(r, e.final);
            if (v != e.value) move = Move::Track;
            e.value = v;
        } else if (neighbourSlides && e.value != e.final) {
            // The edge's own button is gone (its icon vanishes before the
            // slide starts) and nothing is drawn beyond the final layout again.
            e.value = e.final;
            move = Move::Jump;
        }
    }
    if (dir) e.dir = dir;
    e.prev = r;
    return move;
}

bool ReadingFilter::Settled(double now) const {
    return valid_ && ((!transition_ && now - lastChange_ >= kStableMs) || now - firstAt_ >= kMaxTransitionMs);
}

void ReadingFilter::InitAll(const Islands& fresh) {
    Init(appLeft_, fresh.app.left);
    Init(appRight_, fresh.app.right);
    Init(trayLeft_, fresh.tray.left);
    Init(trayRight_, fresh.tray.right);
    Init(split_, fresh.split);
    trusted_ = fresh;
    transition_ = false;
    const LONG sum = fresh.app.left + fresh.app.right;
    mirror_ = std::abs(sum - centre2_) <= 2 ? sum : 0;
    settledLeft_ = fresh.app.left;
    settledRight_ = fresh.app.right;
    settledSplit_ = fresh.split;
    splitArmed_ = false;
}

Islands ReadingFilter::Apply(const Islands& fresh, UINT dpi, double now, LONG centre2) {
    tracked_ = {};
    centre2_ = centre2;
    if (!valid_) {
        InitAll(fresh);
        last_ = fresh;
        valid_ = true;
        firstAt_ = lastChange_ = now;
        return fresh;
    }
    continuity_ = MulDiv(kContinuityLogicalPx, static_cast<int>(dpi ? dpi : 96), 96);
    const bool changed = Structural(fresh, last_) || !SameReading(fresh, last_);
    // A reading that lost most of the buttons at once was taken while they were
    // being rebuilt (closing several apps together loses a few). It is ignored
    // unless it stays unchanged for a long time.
    const bool plausible = fresh.appCount * 2 >= trusted_.appCount;
    if (changed && !transition_) {
        transition_ = true;
        lastRestart_ = now;
        // Positions that change without the button set changing (a slide
        // continuing after a transition ended, a label resizing) say nothing
        // about the final layout.
        for (Edge* e : {&appLeft_, &appRight_, &trayLeft_, &trayRight_, &split_}) e->finalKnown = false;
    }
    // The first plausible reading after the button set changed is the final layout.
    const bool restart = plausible && Structural(fresh, trusted_);
    if (restart) lastRestart_ = now;
    if (changed) lastChange_ = now;

    auto track = [](Move m) { return m == Move::Track; };
    const bool startRushes = plausible && appLeft_.prev - fresh.app.left > continuity_;  // Start sliding left fast
    tracked_.appLeft = track(Step(appLeft_, fresh.app.left, Grow::Left, restart, plausible));
    tracked_.appRight = track(Step(appRight_, fresh.app.right, Grow::Right, restart, plausible));
    // A centred taskbar stays centred: once the right edge has grown to where
    // the last button ends up, Start's slide will end at its mirror image, and
    // the left edge makes that room ahead of it (at once when Start moves fast,
    // as it does when many apps open together and readings trail it).
    if (transition_ && mirror_ && appRight_.value > settledRight_ && mirror_ - appRight_.value < appLeft_.value) {
        appLeft_.value = mirror_ - appRight_.value;
        tracked_.appLeft = startRushes;
    }
    // The buttons right of Start slide in step with it, mirrored, but readings
    // often show them at their final slots already. The left edge only ever
    // trails Start, so the right edge can shrink with its mirror image, down to
    // the final layout.
    if (transition_ && mirror_ && appRight_.finalKnown) {
        const LONG right = std::max(mirror_ - appLeft_.value, appRight_.final);
        if (right < appRight_.value) {
            appRight_.value = right;
            tracked_.appRight = tracked_.appLeft;
        }
    }
    if (fresh.hasTray && trusted_.hasTray) {
        tracked_.trayLeft = track(Step(trayLeft_, fresh.tray.left, Grow::Left, restart, plausible));
        tracked_.trayRight = track(Step(trayRight_, fresh.tray.right, Grow::Right, restart, plausible));
    } else if (plausible) {
        Init(trayLeft_, fresh.tray.left);
        Init(trayRight_, fresh.tray.right);
    }
    if (fresh.hasSplit && trusted_.hasSplit) {
        const LONG before = split_.prev;
        tracked_.split = track(Step(split_, fresh.split, Grow::None, restart, plausible));
        // Its reading jumps to the final layout, then back to where it was just
        // before the icons start to slide.
        if (restart) splitArmed_ = false;
        if (plausible && std::abs(fresh.split - settledSplit_) <= 2 && std::abs(before - settledSplit_) > 2) splitArmed_ = true;
    } else if (plausible) {
        Init(split_, fresh.split);
    }
    if (plausible) trusted_ = fresh;

    if (transition_) {
        const bool caughtUp = appLeft_.value == fresh.app.left && appRight_.value == fresh.app.right &&
                              (!fresh.hasTray || (trayLeft_.value == fresh.tray.left && trayRight_.value == fresh.tray.right)) &&
                              (!fresh.hasSplit || split_.value == fresh.split);
        const double quiet = now - lastChange_;
        const bool believable = plausible || quiet >= kDoubtfulStableMs;
        const bool held = now - lastRestart_ >= kMinHoldMs;
        if ((quiet >= kStableMs && believable && (caughtUp || held)) || now - lastRestart_ >= kMaxTransitionMs) {
            InitAll(fresh);
            tracked_ = {};  // whatever is left to go is a glide, not a slide being followed
        }
    }
    last_ = fresh;

    Islands out = trusted_;
    out.app.left = appLeft_.value;
    out.app.right = appRight_.value;
    if (out.hasTray) {
        out.tray.left = trayLeft_.value;
        out.tray.right = trayRight_.value;
    }
    if (out.hasSplit) {
        // Icons sit close on both sides of the split, and while it slides its
        // readings trail the icons by a few pixels: lead it towards the final
        // layout a little (not before the slide starts).
        const LONG lead = MulDiv(kSplitLeadLogicalPx, static_cast<int>(dpi ? dpi : 96), 96);
        const bool sliding = transition_ && (splitArmed_ || std::abs(split_.value - settledSplit_) > 2);
        // Where it ends up: its first reading after the change, unless that was
        // missed; then Start's final position plus the (unchanging) width of the
        // buttons before the first app.
        const LONG final = split_.finalKnown && std::abs(split_.final - settledSplit_) > 2 ? split_.final
                                                                                         : appLeft_.value + (settledSplit_ - settledLeft_);
        out.split = split_.value + (sliding ? std::clamp(final - split_.value, -lead, lead) : 0);
    }
    return out;
}

// ------------------------------------------------------------------ Pursuit

void Pursuit::Place(const std::vector<Span>& spans, const SpanStyle& style) {
    shown_.clear();
    for (const Span& s : spans) {
        const double l = s.rect.left, r = s.rect.right;
        shown_.push_back({s.id, l, r, l, r, 1, 1, s.rect.top, s.rect.bottom, false});
    }
    style_[0] = styleGoal_[0] = style.marginTop;
    style_[1] = styleGoal_[1] = style.marginBottom;
    style_[2] = styleGoal_[2] = style.radius;
}

void Pursuit::Clear() {
    shown_.clear();
    target_.clear();
    moving_ = false;
}

void Pursuit::SetTarget(const std::vector<Span>& target, const SpanStyle& style, bool snap, double glideMs, double trackSpeed,
                        double nowMs) {
    target_ = target;
    if (snap) {
        Place(target, style);
        moving_ = false;
        return;
    }
    // An edge's speed is set when its goal changes: jumps glide over `glideMs`;
    // tracked edges catch up with each reading quickly (growing ones faster
    // still), and never go slower than `trackSpeed`.
    auto retarget = [&](double pos, double& goal, double& speed, double to, bool tracked, int out) {
        if (goal == to) return;
        goal = to;
        const double distance = std::abs(to - pos);
        const double catchUp = (to - pos) * out > 0 ? kCatchUpMs : kShrinkCatchUpMs;
        speed = tracked ? std::max(trackSpeed, distance / catchUp) : std::max(distance / glideMs, 1e-3);
    };
    for (const Span& t : target) {
        auto it = std::find_if(shown_.begin(), shown_.end(), [&](const Drawn& d) { return d.id == t.id; });
        if (it == shown_.end()) {
            // New island: grows out of its centre.
            const double c = (t.rect.left + t.rect.right) / 2.0;
            shown_.push_back({t.id, c, c, c, c, 1, 1, t.rect.top, t.rect.bottom, false});
            it = shown_.end() - 1;
        }
        it->leaving = false;
        it->top = t.rect.top;
        it->bottom = t.rect.bottom;
        retarget(it->left, it->goalLeft, it->speedLeft, t.rect.left, t.trackLeft, -1);
        retarget(it->right, it->goalRight, it->speedRight, t.rect.right, t.trackRight, 1);
    }
    for (Drawn& d : shown_) {
        if (d.leaving || FindSpan(target, d.id)) continue;
        // Gone from the target: shrinks into its centre.
        d.leaving = true;
        const double c = (d.left + d.right) / 2;
        retarget(d.left, d.goalLeft, d.speedLeft, c, false, -1);
        retarget(d.right, d.goalRight, d.speedRight, c, false, 1);
    }
    const double goals[3] = {static_cast<double>(style.marginTop), static_cast<double>(style.marginBottom), static_cast<double>(style.radius)};
    for (int i = 0; i < 3; ++i) {
        if (styleGoal_[i] == goals[i]) continue;
        styleGoal_[i] = goals[i];
        styleSpeed_[i] = std::max(std::abs(goals[i] - style_[i]) / glideMs, 1e-3);
    }
    if (!moving_) lastMs_ = nowMs;
    moving_ = true;
}

bool Pursuit::Step(double nowMs) {
    const double dt = std::clamp(nowMs - lastMs_, 0.0, 50.0);
    lastMs_ = nowMs;
    if (!moving_) return false;
    bool moved = false, done = true;
    for (auto it = shown_.begin(); it != shown_.end();) {
        moved |= Toward(it->left, it->goalLeft, it->speedLeft * dt);
        moved |= Toward(it->right, it->goalRight, it->speedRight * dt);
        if (it->leaving && it->right - it->left <= 0.5) {
            it = shown_.erase(it);
            moved = true;
            continue;
        }
        done = done && it->left == it->goalLeft && it->right == it->goalRight;
        ++it;
    }
    for (int i = 0; i < 3; ++i) {
        moved |= Toward(style_[i], styleGoal_[i], styleSpeed_[i] * dt);
        done = done && style_[i] == styleGoal_[i];
    }
    if (done) moving_ = false;
    return moved;
}

std::vector<Span> Pursuit::Shown() const {
    std::vector<Span> out;
    for (const Drawn& d : shown_) {
        const LONG l = std::lround(d.left), r = std::lround(d.right);
        if (r > l) out.push_back({{l, d.top, r, d.bottom}, d.id});
    }
    return out;
}

SpanStyle Pursuit::ShownStyle() const {
    return {static_cast<int>(std::lround(style_[0])), static_cast<int>(std::lround(style_[1])), static_cast<int>(std::lround(style_[2]))};
}

}  // namespace fb
