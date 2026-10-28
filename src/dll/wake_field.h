#ifndef TOW_WAKE_FIELD_H
#define TOW_WAKE_FIELD_H
#include <windows.h>
#include <memory>

/* Integer game wave, bounded to the view plus a 64-pixel world margin. */
class WakeField
{
  public:
    bool align(RECT view, const double *camera, int resolution);
    void clear();
    void inject(double x, double y, int strength, int resolution);
    void step(int damping);
    const short *shades();

    int width() const
    {
        return width_;
    }

    int height() const
    {
        return height_;
    }

    int x() const
    {
        return x_;
    }

    int y() const
    {
        return y_;
    }

  private:
    struct Cell {
        short current;
        short previous;
        short before;
        short after;
    };

    std::unique_ptr<Cell[]> cells_;
    std::unique_ptr<Cell[]> scratch_;
    std::unique_ptr<short[]> shades_;
    int width_ = 0;
    int height_ = 0;
    int x_ = 0;
    int y_ = 0;
};

#endif
