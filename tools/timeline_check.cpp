/* Synthetic check: steps -> pause gap (no steps, plain frames) -> resume.
 * Mirrors SceneCompositor's record intake, stall reset, K-record hold and
 * draw selection around the shared SceneTimeline. Exit code 0 when the displayed
 * step never decreases and never jumps by more than one step. */
#include <stdio.h>
#include "../src/dll/scene_timeline.h"

/* Same values as scene.h / scene_gpu.cpp. */
static const unsigned RING = 4;
static const unsigned RECOVERY = 3;
static const unsigned EVENTS = 4096;
static const double FREQUENCY = 1000000.0;
static const double FRAME_MS = 1000.0 / 60.0;
static const double EPSILON = 1e-9;

struct Entry {
    unsigned step;
    double qpc;
};

struct Event {
    double time;
    int record;
    LONG serial;
    unsigned step;
};

struct Variant {
    const char *name;
    double step_ms;
    double plain_lag_ms;
    double pause_ms;
    int resume_pass;
    int banner;
    int sleep;
    int blocked;
};

struct Result {
    double max_rewind;
    double max_jump;
    double resume_jump;
    double max_pair_ms;
    unsigned rebases;
    unsigned resets;
    unsigned frames;
};

/* UI thread: one record and its plain capture per step; plain frames only while paused. */
class Producer
{
  public:
    void build(const Variant &v)
    {
        count_ = 0;
        double time = 0;
        unsigned step = 0;
        LONG serial = 0;
        for (unsigned i = 0; i < 60; ++i) {
            time += v.step_ms;
            publish(time, ++serial, ++step, v.plain_lag_ms);
        }

        pause_start_ = time;
        pause_end_ = time + v.pause_ms;
        if (v.banner) {
            add(time + 5, 0, ++serial, step);
            add(time + 600, 0, ++serial, step);
        }
        time = pause_end_;
        if (v.resume_pass) {
            /* Unpause redraw before the first new step: a record with the paused step. */
            publish(time, ++serial, step, v.plain_lag_ms);
        }
        for (unsigned i = 0; i < 60; ++i) {
            time += v.step_ms;
            publish(time, ++serial, ++step, v.plain_lag_ms);
        }
        end_ = time + 200;
    }

    unsigned step_of(LONG serial) const
    {
        for (unsigned i = 0; i < count_; ++i) {
            if (events_[i].serial == serial) {
                return events_[i].step;
            }
        }
        return 0;
    }

    unsigned count_ = 0;
    Event events_[EVENTS] = {};
    double pause_start_ = 0;
    double pause_end_ = 0;
    double end_ = 0;

  private:
    void add(double time, int record, LONG serial, unsigned step)
    {
        if (count_ < EVENTS) {
            events_[count_++] = {time, record, serial, step};
        }
    }

    void publish(double time, LONG serial, unsigned step, double lag)
    {
        add(time, 1, serial, step);
        add(time + lag, 0, serial, step);
    }
};

/* Render thread bookkeeping of SceneCompositor reduced to steps and times. */
class Consumer
{
  public:
    Consumer(const Producer &producer, const Variant &variant, bool fixed)
        : producer_(producer), variant_(variant), fixed_(fixed)
    {
    }

    Result run(void)
    {
        Result result = {};
        double last_frame = -1e9;
        double shown = -1;
        bool after_resume = false;
        bool blocked = false;
        for (double now = FRAME_MS; now < producer_.end_; now += FRAME_MS) {
            bool fresh = pending(now);
            /* Inactive renderer sleeps until a new frame or the 1 s timeout. */
            if (variant_.sleep && !valid_ && !fresh && now - last_frame < 1000) {
                continue;
            }
            /* Render thread blocked across the pause: old and new records queue together. */
            if (variant_.blocked && now > producer_.pause_start_ - variant_.step_ms / 2 &&
                now < producer_.pause_end_ + variant_.step_ms * 1.5) {
                blocked = true;
                continue;
            }
            last_frame = now;

            update(now);
            double step = present(now);
            if (step < 0) {
                continue;
            }

            ++result.frames;
            if (shown >= 0) {
                double change = step - shown;
                if (-change > result.max_rewind) {
                    result.max_rewind = -change;
                }
                if (!blocked && change > result.max_jump) {
                    result.max_jump = change;
                }
                if (!blocked && after_resume && change > result.resume_jump) {
                    result.resume_jump = change;
                }
            }
            shown = step;
            blocked = false;
            after_resume = now >= producer_.pause_end_;
        }
        double ignored = 0;
        result.rebases = timeline_.take_rebases(&ignored);
        result.resets = resets_;
        result.max_pair_ms = max_pair_ms_;
        return result;
    }

  private:
    double ticks(double ms) const
    {
        return ms * FREQUENCY / 1000.0;
    }

    double stall_ms(void) const
    {
        return 2.0 * variant_.step_ms;
    }

    bool pending(double now) const
    {
        for (unsigned i = next_; i < producer_.count_; ++i) {
            if (producer_.events_[i].time <= now) {
                return true;
            }
        }
        return false;
    }

    void reset_timeline(void)
    {
        timeline_.reset_clock();
        count_ = 0;
        interval_ = 0;
        valid_ = false;
        complete_ = 0;
        stalled_ = false;
        stall_qpc_ = 0;
        ++resets_;
    }

    void check_stall(double now)
    {
        double latest = count_ ? entries_[count_ - 1].qpc : stall_qpc_;
        if (!latest || (ticks(now) - latest) * 1000.0 / FREQUENCY <= stall_ms()) {
            return;
        }
        if (!stalled_ || count_) {
            reset_timeline();
            stalled_ = true;
            stall_qpc_ = latest;
        }
    }

    void check_gap(double qpc)
    {
        if (!fixed_ || !count_ || (qpc - entries_[count_ - 1].qpc) * 1000.0 / FREQUENCY <= stall_ms()) {
            return;
        }
        double latest = entries_[count_ - 1].qpc;
        reset_timeline();
        stalled_ = true;
        stall_qpc_ = latest;
    }

    /* scene_take keeps only the newest two queued records. */
    void update(double now)
    {
        check_stall(now);
        const Event *queue[EVENTS];
        unsigned size = 0;
        for (; next_ < producer_.count_ && producer_.events_[next_].time <= now; ++next_) {
            const Event &event = producer_.events_[next_];
            if (event.record) {
                queue[size++] = &event;
            } else {
                plain_serial_ = event.serial;
            }
        }
        unsigned first = size > 2 ? size - 2 : 0;
        for (unsigned i = first; i < size; ++i) {
            take(*queue[i]);
        }
        check_stall(now);
    }

    void take(const Event &event)
    {
        timeline_.record(event.serial, event.step);
        double qpc = ticks(event.time);
        check_gap(qpc);
        if (count_) {
            const Entry &last = entries_[count_ - 1];
            if (event.step <= last.step || qpc <= last.qpc) {
                complete_ = 0;
                return;
            }
            if (event.step != last.step + 1) {
                complete_ = 0;
            }
            double sample = (qpc - last.qpc) / (event.step - last.step);
            interval_ = interval_ ? interval_ + .1 * (sample - interval_) : sample;
        }
        if (count_ == RING) {
            discard_first();
        }

        entries_[count_++] = {event.step, qpc};
        if (stalled_) {
            stall_qpc_ = qpc;
        }
        if (complete_ < RECOVERY) {
            ++complete_;
        }
        if (!valid_ && complete_ >= RECOVERY) {
            valid_ = true;
            stalled_ = false;
        }
    }

    void discard_first(void)
    {
        --count_;
        for (unsigned i = 0; i < count_; ++i) {
            entries_[i] = entries_[i + 1];
        }
    }

    /* Displayed step: interpolated pair, or the newest plain capture. */
    double present(double now)
    {
        if (count_ && valid_) {
            double interval = interval_ ? interval_ : ticks(78);
            timeline_.advance(entries_, count_, ticks(now), interval);
            double display = timeline_.time();
            while (count_ > 2 && entries_[1].qpc <= display) {
                discard_first();
            }
            unsigned current = SceneTimeline::pair(entries_, count_, display);
            if (!current) {
                return entries_[0].step;
            }
            const Entry &before = entries_[current - 1];
            const Entry &entry = entries_[current];
            if (entry.step != before.step + 1) {
                return entry.step;
            }
            double pair_ms = (entry.qpc - before.qpc) * 1000.0 / FREQUENCY;
            if (pair_ms > max_pair_ms_) {
                max_pair_ms_ = pair_ms;
            }
            double alpha = (display - before.qpc) / (entry.qpc - before.qpc);
            alpha = alpha < 0 ? 0 : (alpha > 1 ? 1 : alpha);
            return before.step + alpha;
        }

        if (!plain_serial_) {
            return -1;
        }
        if (fixed_) {
            timeline_.show_plain(plain_serial_);
        }
        return producer_.step_of(plain_serial_);
    }

    const Producer &producer_;
    const Variant &variant_;
    bool fixed_;
    SceneTimeline timeline_;
    Entry entries_[RING] = {};
    unsigned count_ = 0;
    unsigned next_ = 0;
    LONG plain_serial_ = 0;
    double interval_ = 0;
    bool valid_ = false;
    unsigned complete_ = 0;
    bool stalled_ = false;
    double stall_qpc_ = 0;
    unsigned resets_ = 0;
    double max_pair_ms_ = 0;
};

static Producer producer;

static Result simulate(const Variant &variant, bool fixed)
{
    producer.build(variant);
    Consumer consumer(producer, variant, fixed);
    return consumer.run();
}

int main(void)
{
    static const Variant variants[] = {
        {"pause_2s", 78, 0, 2000, 0, 0, 0, 0},           {"pause_plain_lag", 78, 3, 2000, 0, 0, 0, 0},
        {"pause_banner_sleep", 78, 0, 2000, 0, 1, 1, 0}, {"resume_redraw", 78, 0, 1500, 1, 1, 1, 0},
        {"speed_100_lag", 62, 5, 700, 0, 0, 0, 0},       {"resume_redraw_lag", 62, 4, 3000, 1, 0, 0, 0},
        {"render_blocked", 78, 0, 2000, 0, 0, 0, 1},     {"render_blocked_redraw", 62, 2, 900, 1, 0, 1, 1},
    };
    int failed = 0;
    unsigned legacy_rewinds = 0;
    for (const Variant &variant : variants) {
        Result legacy = simulate(variant, false);
        Result fixed = simulate(variant, true);
        /* No rewind, at most one step forward, no interpolation across the pause gap. */
        bool ok = fixed.max_rewind <= EPSILON && fixed.max_jump <= 1 + EPSILON && fixed.frames > 100 &&
                  fixed.max_pair_ms <= 2.0 * variant.step_ms;
        if (legacy.max_rewind > EPSILON) {
            ++legacy_rewinds;
        }
        printf("%-21s legacy rewind=%.3f jump=%.3f pair_ms=%.0f | w94 rewind=%.3f jump=%.3f "
               "resume_jump=%.3f pair_ms=%.0f rebases=%u resets=%u frames=%u %s\n",
               variant.name, legacy.max_rewind, legacy.max_jump, legacy.max_pair_ms, fixed.max_rewind,
               fixed.max_jump, fixed.resume_jump, fixed.max_pair_ms, fixed.rebases, fixed.resets,
               fixed.frames, ok ? "ok" : "FAIL");
        if (!ok) {
            failed = 1;
        }
    }

    /* The model must reproduce the rewind without the fix, or it proves nothing. */
    if (legacy_rewinds != sizeof(variants) / sizeof(variants[0])) {
        printf("legacy model did not rewind in every variant (%u)\n", legacy_rewinds);
        failed = 1;
    }
    printf(failed ? "timeline_check: FAIL\n" : "timeline_check: ok\n");
    return failed;
}
