#include "wake_field.h"
#include <cmath>
#include <new>

bool WakeField::align(RECT view, const double *camera, int resolution)
{
    int width = (view.right - view.left + 128 + resolution - 1) / resolution + 2;
    int height = (view.bottom - view.top + 128 + resolution - 1) / resolution + 2;
    int x = (int)floor((camera[0] - 64) / resolution);
    int y = (int)floor((camera[1] - 64) / resolution);
    if (width != width_ || height != height_ || !cells_ || !scratch_ || !shades_) {
        std::unique_ptr<Cell[]> cells(new (std::nothrow) Cell[width * height]{});
        std::unique_ptr<Cell[]> scratch(new (std::nothrow) Cell[width * height]{});
        std::unique_ptr<short[]> shades(new (std::nothrow) short[width * height * 2]{});
        if (!cells || !scratch || !shades) {
            return false;
        }
        cells_.swap(cells);
        scratch_.swap(scratch);
        shades_.swap(shades);
        width_ = width;
        height_ = height;
    } else if (x != x_ || y != y_) {
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                int sx = col + x - x_;
                int sy = row + y - y_;
                scratch_[row * width + col] =
                    sx >= 0 && sx < width && sy >= 0 && sy < height ? cells_[sy * width + sx] : Cell{};
            }
        }
        cells_.swap(scratch_);
    }
    x_ = x;
    y_ = y;
    return true;
}

void WakeField::clear()
{
    for (int i = 0; i < width_ * height_; ++i) {
        cells_[i] = {};
    }
}

void WakeField::inject(double x, double y, int strength, int resolution)
{
    int col = (int)floor(x / resolution) - x_;
    int row = (int)floor(y / resolution) - y_;
    if (col >= 1 && col < width_ - 1 && row >= 1 && row < height_ - 1) {
        cells_[row * width_ + col].current = (short)strength;
    }
}

void WakeField::step(int damping)
{
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            int i = y * width_ + x;
            const Cell &old = cells_[i];
            short next = old.previous;
            if (x && y && x < width_ - 1 && y < height_ - 1) {
                int total = cells_[i - 1].current + cells_[i + 1].current + cells_[i - width_].current +
                            cells_[i + width_].current;
                /* x86 narrowing wraps BEFORE the arithmetic damping shift. */
                short h = (short)(total / 2 - old.previous);
                next = (short)(h - (h >> damping));
            }
            scratch_[i] = {next, old.current, old.after, (short)(old.current >> 2)};
        }
    }
    cells_.swap(scratch_);
}

const short *WakeField::shades()
{
    for (int i = 0; i < width_ * height_; ++i) {
        shades_[2 * i] = cells_[i].before;
        shades_[2 * i + 1] = cells_[i].after;
    }
    return shades_.get();
}
