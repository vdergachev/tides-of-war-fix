#ifndef TOW_SCENE_TIMELINE_H
#define TOW_SCENE_TIMELINE_H
#include <windows.h>

/* Delayed display clock; render thread only. The clock never starts or
 * continues earlier than a step already presented, including the plain frames
 * shown while smoothing is held after a pause or stall. */
class SceneTimeline
{
  public:
    /* Taken records remembered to map a plain capture serial to its step. */
    static const unsigned HISTORY = 8;

    /* Stall, invalid record or texture failure: restart the clock, keep the shown step. */
    void reset_clock(void)
    {
        time_ = 0;
        now_ = 0;
    }

    /* New epoch: steps shown before belong to another scene. */
    void reset(void)
    {
        reset_clock();
        shown_step_ = 0;
        plain_serial_ = 0;
        history_count_ = 0;
        history_next_ = 0;
    }

    /* Every taken record of this epoch; its plain frame may already be on screen. */
    void record(LONG serial, unsigned step)
    {
        history_[history_next_].serial = serial;
        history_[history_next_].step = step;
        history_next_ = (history_next_ + 1) % HISTORY;
        if (history_count_ < HISTORY) {
            ++history_count_;
        }

        if (plain_serial_ && serial <= plain_serial_) {
            raise(step);
        }
    }

    /* A plain frame with this capture serial was presented. */
    void show_plain(LONG serial)
    {
        if (serial > plain_serial_) {
            plain_serial_ = serial;
        }

        for (unsigned i = 0; i < history_count_; ++i) {
            if (history_[i].serial <= serial) {
                raise(history_[i].step);
            }
        }
    }

    /* Approach one interval behind now at 0.75-1.25x, hold the newest entry,
     * then lift the clock to the newest step already shown (rebase). */
    template <typename Entry> void advance(const Entry *entries, unsigned count, double now, double interval)
    {
        double elapsed = now_ && now > now_ ? now - now_ : 0;
        now_ = now;
        if (!time_) {
            time_ = entries[0].qpc;
        }

        double target = now - interval;
        double rate = limit(1 + (target - time_) / (4 * interval), .75, 1.25);
        double next = time_ + elapsed * rate;
        if (next > entries[count - 1].qpc) {
            next = entries[count - 1].qpc;
        }
        if (next > time_) {
            time_ = next;
        }

        double floor = floor_time(entries, count);
        if (time_ < floor) {
            if (floor - time_ > rebase_max_) {
                rebase_max_ = floor - time_;
            }
            time_ = floor;
            ++rebases_;
        }
    }

    /* First entry not older than time; the newest when time passed all. */
    template <typename Entry> static unsigned pair(const Entry *entries, unsigned count, double time)
    {
        for (unsigned i = 1; i < count; ++i) {
            if (entries[i].qpc >= time) {
                return i;
            }
        }
        return count - 1;
    }

    double time(void) const
    {
        return time_;
    }

    double shown_step(void) const
    {
        return shown_step_;
    }

    /* Rebases and the largest lift in QPC ticks since the last call. */
    unsigned take_rebases(double *max_ticks)
    {
        unsigned result = rebases_;
        *max_ticks = rebase_max_;
        rebases_ = 0;
        rebase_max_ = 0;
        return result;
    }

  private:
    struct Shown {
        LONG serial;
        unsigned step;
    };

    static double limit(double value, double low, double high)
    {
        if (value < low) {
            return low;
        }
        if (value > high) {
            return high;
        }
        return value;
    }

    void raise(unsigned step)
    {
        if (step > shown_step_) {
            shown_step_ = step;
        }
    }

    /* Capture time of the shown step on this timeline; 0 when nothing newer was shown. */
    template <typename Entry> double floor_time(const Entry *entries, unsigned count) const
    {
        if (!shown_step_ || shown_step_ <= entries[0].step) {
            return 0;
        }

        for (unsigned i = 1; i < count; ++i) {
            if (shown_step_ <= entries[i].step) {
                const Entry &before = entries[i - 1];
                double fraction = (shown_step_ - before.step) / (entries[i].step - before.step);
                return before.qpc + fraction * (entries[i].qpc - before.qpc);
            }
        }
        return entries[count - 1].qpc;
    }

    double time_ = 0;
    double now_ = 0;
    double shown_step_ = 0;
    LONG plain_serial_ = 0;
    Shown history_[HISTORY] = {};
    unsigned history_count_ = 0;
    unsigned history_next_ = 0;
    unsigned rebases_ = 0;
    double rebase_max_ = 0;
};
#endif
