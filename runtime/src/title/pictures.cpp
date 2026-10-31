#include "title/pictures.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <vector>

namespace cafe::title::pictures {
namespace {

// ------------------------------------------------------------------ shapes

// A shape is its signed distance, in pixels: below 0 inside.
using Shape = std::function<double(double x, double y)>;

struct Point {
    double x = 0, y = 0;
};

double segment_distance(Point p, Point a, Point b) {
    const double dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
    const double t = length2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length2, 0.0, 1.0) : 0.0;
    return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}

Shape circle(Point c, double radius) {
    return [=](double x, double y) { return std::hypot(x - c.x, y - c.y) - radius; };
}

// From (x0, y0) to (x1, y1), the corners rounded by `radius`.
Shape box(double x0, double y0, double x1, double y1, double radius) {
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - radius, hy = (y1 - y0) / 2 - radius;
    return [=](double x, double y) {
        const double qx = std::abs(x - cx) - hx, qy = std::abs(y - cy) - hy;
        return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - radius;
    };
}

// Lines through the points, `half` either side, the ends round.
Shape path(std::vector<Point> points, double half) {
    return [=](double x, double y) {
        double d = 1e9;
        for (size_t i = 1; i < points.size(); ++i) d = std::min(d, segment_distance({x, y}, points[i - 1], points[i]));
        return d - half;
    };
}

Shape capsule(Point a, Point b, double half) { return path({a, b}, half); }

// Part of a circle from angle a0 to a1 (radians, clockwise from the right:
// y runs down), `half` either side.
Shape arc(Point c, double radius, double a0, double a1, double half) {
    std::vector<Point> points;
    for (int i = 0; i <= 24; ++i) {
        const double a = a0 + (a1 - a0) * i / 24;
        points.push_back({c.x + radius * std::cos(a), c.y + radius * std::sin(a)});
    }
    return path(std::move(points), half);
}

Shape polygon(std::vector<Point> points) {
    return [=](double x, double y) {
        double d = 1e9;
        bool inside = false;
        for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
            const Point& a = points[i];
            const Point& b = points[j];
            d = std::min(d, segment_distance({x, y}, a, b));
            if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
        }
        return inside ? -d : d;
    };
}

Shape operator|(Shape a, Shape b) {
    return [=](double x, double y) { return std::min(a(x, y), b(x, y)); };
}
// Joined, the corners between them filled round, about `k` pixels.
Shape blend(Shape a, Shape b, double k) {
    return [=](double x, double y) {
        const double da = a(x, y), db = b(x, y), h = std::clamp(0.5 + 0.5 * (db - da) / k, 0.0, 1.0);
        return db + (da - db) * h - k * h * (1 - h);
    };
}
// With its mirror image across the picture's middle.
Shape both_sides(Shape s) {
    return [=](double x, double y) { return std::min(s(x, y), s(kWidth - x, y)); };
}
// Turned by `angle` (radians, clockwise) about c.
Shape turned(Shape s, Point c, double angle) {
    const double co = std::cos(angle), si = std::sin(angle);
    return [=](double x, double y) {
        const double dx = x - c.x, dy = y - c.y;
        return s(c.x + dx * co + dy * si, c.y - dx * si + dy * co);
    };
}

// ------------------------------------------------------------------ painting

struct Colour {
    double r = 0, g = 0, b = 0, a = 1;
};
Colour rgb(uint32_t v, double alpha = 1.0) {
    return {((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0, (v & 0xFF) / 255.0, alpha};
}

// The title's: white parts, their edges and details in a dark grey.
const Colour kWhite = rgb(0xFFFFFF), kInk = rgb(0x5A5658);
constexpr double kEdge = 2.0, kLine = 1.5; // the body's outline, other lines

class Painter {
public:
    Painter(uint32_t width = kWidth, uint32_t height = kHeight)
        : width_(width), height_(height), rgba_(size_t{width} * height * 4, 0.0) {}

    void fill(const Shape& s, Colour c) {
        paint(s, c, [](double d) { return std::clamp(0.5 - d, 0.0, 1.0); });
    }
    void stroke(const Shape& s, double width, Colour c) {
        paint(s, c, [width](double d) { return std::clamp(width / 2 + 0.5 - std::abs(d), 0.0, 1.0); });
    }
    // A part: white, outlined.
    void part(const Shape& s, double line = kLine, Colour face = kWhite) {
        fill(s, face);
        stroke(s, line, kInk);
    }

    Rgba result() const {
        Rgba out = blank(width_, height_);
        for (size_t i = 0; i < size_t{width_} * height_; ++i) {
            const double* p = &rgba_[i * 4];
            uint8_t* o = &out.pixels[i * 4];
            for (int k = 0; k < 3; ++k) {
                o[k] = static_cast<uint8_t>(std::clamp(p[3] > 0 ? p[k] / p[3] * 255.0 + 0.5 : 0.0, 0.0, 255.0));
            }
            o[3] = static_cast<uint8_t>(std::clamp(p[3] * 255.0 + 0.5, 0.0, 255.0));
        }
        return out;
    }

private:
    // In blocks of 8 x 8 pixels: a block whose middle lies farther than
    // its half diagonal (and a stroke's width) from the shape's edge is
    // all inside or all outside it (a distance changes by at most the
    // distance moved).
    void paint(const Shape& s, Colour c, const std::function<double(double)>& coverage) {
        constexpr uint32_t kBlock = 8;
        for (uint32_t by = 0; by < height_; by += kBlock) {
            for (uint32_t bx = 0; bx < width_; bx += kBlock) {
                const double mid = s(bx + kBlock / 2.0, by + kBlock / 2.0);
                const bool uniform = std::abs(mid) > kBlock * 0.75 + 4;
                const double block_coverage = uniform ? coverage(mid) : 0;
                if (uniform && block_coverage == 0) continue;
                for (uint32_t y = by; y < std::min(by + kBlock, height_); ++y) {
                    for (uint32_t x = bx; x < std::min(bx + kBlock, width_); ++x) {
                        const double a = c.a * (uniform ? block_coverage : coverage(s(x + 0.5, y + 0.5)));
                        if (a <= 0) continue;
                        double* p = &rgba_[(size_t{y} * width_ + x) * 4];
                        p[0] = c.r * a + p[0] * (1 - a);
                        p[1] = c.g * a + p[1] * (1 - a);
                        p[2] = c.b * a + p[2] * (1 - a);
                        p[3] = a + p[3] * (1 - a);
                    }
                }
            }
        }
    }

    uint32_t width_, height_;
    std::vector<double> rgba_; // premultiplied
};

constexpr double kPi = std::numbers::pi;

// A stick seen from above: its well, the cap and the cap's dished top.
void stick(Painter& p, Point c) {
    p.part(circle(c, 20));
    p.part(circle(c, 15));
    p.stroke(circle(c, 10), 1.0, kInk);
}

// Shoulder buttons behind the body's top corners, as their fronts show.
void shoulders(Painter& p, double x0, double x1, double y0, double y1) {
    const Shape left = box(x0, y0, x1, y1, 6);
    p.part(both_sides(left));
}

// ------------------------------------------------------------------ the devices

// A PlayStation-style controller: grips swept down and out, the touch pad
// between the D-pad's four arrows and the symbol buttons, two sticks side
// by side below.
Rgba playstation() {
    Painter p;
    shoulders(p, 56, 112, 22, 42);
    const Shape middle = box(48, 32, 272, 120, 30);
    const Shape grip = capsule({92, 96}, {58, 158}, 33);
    const Shape body = blend(middle, both_sides(grip), 20);
    p.part(body, kEdge);
    p.part(box(122, 36, 198, 84, 7));                       // the touch pad
    p.part(both_sides(capsule({107, 42}, {109, 54}, 3.2))); // create, options
    // The D-pad: four arrows pointing in.
    const Point pad{84, 80};
    const Shape arrow = polygon({{pad.x - 7, pad.y - 23}, {pad.x + 7, pad.y - 23}, {pad.x + 7, pad.y - 12},
                                 {pad.x, pad.y - 5.5}, {pad.x - 7, pad.y - 12}});
    for (int k = 0; k < 4; ++k) p.part(turned(arrow, pad, k * kPi / 2));
    // The symbol buttons and their symbols.
    const Point face{236, 80};
    constexpr double kApart = 16.5, kButton = 8.8;
    const Point top{face.x, face.y - kApart}, right{face.x + kApart, face.y}, bottom{face.x, face.y + kApart},
        left{face.x - kApart, face.y};
    for (const Point b : {top, right, bottom, left}) p.part(circle(b, kButton));
    p.stroke(polygon({{top.x, top.y - 4.6}, {top.x + 4.6, top.y + 3.3}, {top.x - 4.6, top.y + 3.3}}), 1.4, kInk);
    p.stroke(circle(right, 4.3), 1.4, kInk);
    p.fill(capsule({bottom.x - 3.7, bottom.y - 3.7}, {bottom.x + 3.7, bottom.y + 3.7}, 0.7) |
               capsule({bottom.x - 3.7, bottom.y + 3.7}, {bottom.x + 3.7, bottom.y - 3.7}, 0.7),
           kInk);
    p.stroke(box(left.x - 3.9, left.y - 3.9, left.x + 3.9, left.y + 3.9, 0.8), 1.4, kInk);
    stick(p, {124, 118});
    stick(p, {196, 118});
    p.part(circle({160, 106}, 6));        // the home button
    p.part(box(153, 117, 167, 122, 2.5)); // the microphone's
    return p.result();
}

// An Xbox-style controller: a stick above the D-pad on the left, the
// lettered buttons above a stick on the right, the guide button between.
Rgba xbox() {
    Painter p;
    shoulders(p, 60, 116, 28, 48);
    const Shape middle = box(54, 40, 266, 122, 34) | circle({160, 62}, 30);
    const Shape grip = capsule({94, 98}, {64, 156}, 34);
    const Shape body = blend(middle, both_sides(grip), 22);
    p.part(body, kEdge);
    stick(p, {94, 78});
    stick(p, {196, 122});
    // The D-pad: a cross in a round well.
    const Point pad{124, 122};
    p.part(circle(pad, 18));
    p.part(box(pad.x - 5.5, pad.y - 14.5, pad.x + 5.5, pad.y + 14.5, 2.5) |
           box(pad.x - 14.5, pad.y - 5.5, pad.x + 14.5, pad.y + 5.5, 2.5));
    // The lettered buttons: Y, B, A, X clockwise from the top.
    const Point face{226, 78};
    constexpr double kApart = 16, kButton = 8.6;
    const Point top{face.x, face.y - kApart}, right{face.x + kApart, face.y}, bottom{face.x, face.y + kApart},
        left{face.x - kApart, face.y};
    for (const Point b : {top, right, bottom, left}) p.part(circle(b, kButton));
    constexpr double kStroke = 0.75;
    p.fill(path({{top.x - 4, top.y - 5}, {top.x, top.y}, {top.x + 4, top.y - 5}}, kStroke) |
               capsule({top.x, top.y}, {top.x, top.y + 5}, kStroke),
           kInk); // Y
    p.fill(capsule({right.x - 3, right.y - 5}, {right.x - 3, right.y + 5}, kStroke) |
               path({{right.x - 3, right.y - 5}, {right.x + 0.8, right.y - 5}}, kStroke) |
               arc({right.x + 0.8, right.y - 2.6}, 2.4, -kPi / 2, kPi / 2, kStroke) |
               path({{right.x - 3, right.y - 0.2}, {right.x + 1.2, right.y - 0.2}}, kStroke) |
               arc({right.x + 1.2, right.y + 2.4}, 2.6, -kPi / 2, kPi / 2, kStroke) |
               path({{right.x - 3, right.y + 5}, {right.x + 1.2, right.y + 5}}, kStroke),
           kInk); // B
    p.fill(path({{bottom.x - 4.2, bottom.y + 5}, {bottom.x, bottom.y - 5}, {bottom.x + 4.2, bottom.y + 5}}, kStroke) |
               capsule({bottom.x - 2.4, bottom.y + 1.4}, {bottom.x + 2.4, bottom.y + 1.4}, kStroke),
           kInk); // A
    p.fill(capsule({left.x - 4, left.y - 5}, {left.x + 4, left.y + 5}, kStroke) |
               capsule({left.x - 4, left.y + 5}, {left.x + 4, left.y - 5}, kStroke),
           kInk); // X
    // The guide button, and view and menu either side of it.
    p.part(circle({160, 56}, 11));
    p.stroke(circle({160, 56}, 7), 1.0, kInk);
    p.part(both_sides(circle({140, 82}, 4.5)));
    return p.result();
}

// A keyboard: the letter keys' rows, a space bar, the arrow keys apart on
// the right.
Rgba keyboard() {
    Painter p;
    p.part(box(18, 58, 302, 166, 10), kEdge);
    constexpr double kKey = 14.5, kGap = 3, kTop = 67, kLeft = 27;
    const auto key = [&](double x, double y, double w) { p.part(box(x, y, x + w, y + kKey, 3), 1.2); };
    // Four rows of the main block, from the second on with a wider key
    // first (tab, caps lock, shift), each ending in one as wide as the
    // block leaves (backspace, enter, shift).
    constexpr double kRight = 230, kFirst[] = {0, kKey, kKey + 4, kKey + 9};
    for (int row = 0; row < 4; ++row) {
        const double y = kTop + row * (kKey + kGap);
        double x = kLeft;
        if (kFirst[row] > 0) {
            key(x, y, kFirst[row]);
            x += kFirst[row] + kGap;
        }
        const int keys = static_cast<int>((kRight - x + kGap) / (kKey + kGap));
        for (int k = 0; k < keys; ++k) {
            const double w = k + 1 == keys ? kRight - x : kKey;
            key(x, y, w);
            x += w + kGap;
        }
    }
    const double bottom = kTop + 4 * (kKey + kGap);
    // Control, the system key, alt, the space bar, alt, control.
    constexpr double kMods[] = {22, 18, 22, 0, 22, 22};
    double mods = 0;
    for (const double w : kMods) mods += w + kGap;
    double x = kLeft;
    for (const double w : kMods) {
        const double width = w > 0 ? w : kRight - kLeft - mods;
        key(x, bottom, width);
        x += width + kGap;
    }
    // The arrows, and the keys above them.
    constexpr double kArrows = 243.5;
    for (int k = 0; k < 3; ++k) {
        key(kArrows + k * (kKey + kGap), kTop, kKey);
        key(kArrows + k * (kKey + kGap), kTop + kKey + kGap, kKey);
        key(kArrows + k * (kKey + kGap), bottom, kKey);
    }
    key(kArrows + kKey + kGap, bottom - kKey - kGap, kKey);
    const double ax = kArrows + kKey + kGap + kKey / 2, ay = bottom - kGap - kKey / 2;
    p.fill(polygon({{ax, ay - 4}, {ax + 3.5, ay + 2}, {ax - 3.5, ay + 2}}), kInk); // the up arrow
    for (int k = 0; k < 3; ++k) {
        const double cx = kArrows + k * (kKey + kGap) + kKey / 2, cy = bottom + kKey / 2;
        const double angle = k == 0 ? -kPi / 2 : k == 1 ? kPi : kPi / 2;
        p.fill(turned(polygon({{cx, cy - 4}, {cx + 3.5, cy + 2}, {cx - 3.5, cy + 2}}), {cx, cy}, angle), kInk);
    }
    return p.result();
}

} // namespace

Rgba white(Device device) {
    switch (device) {
    case Device::kKeyboard: return keyboard();
    case Device::kPlayStation: return playstation();
    case Device::kXbox: break;
    }
    return xbox();
}

// The title's lit pictures, measured: outside, the glow's alpha falls from
// about 190 at the edge to none 14 pixels out; inside, the picture is mixed
// 46 % with the tint at its edge, nothing 26 pixels in.
Rgba lit(const Rgba& picture, uint32_t outer, uint32_t inner) {
    constexpr double kGlow = 14, kGlowAlpha = 190 / 255.0, kTintDepth = 26, kTint = 0.46;
    const uint32_t w = picture.width, h = picture.height;
    std::vector<uint8_t> inside(size_t{w} * h), outside(size_t{w} * h);
    for (size_t i = 0; i < inside.size(); ++i) {
        inside[i] = picture.pixels[i * 4 + 3] >= 128;
        outside[i] = !inside[i];
    }
    const std::vector<float> to_inside = distance(inside, w, h), to_outside = distance(outside, w, h);
    const Colour glow = rgb(outer), tint = rgb(inner);
    Rgba out = blank(w, h);
    for (size_t i = 0; i < inside.size(); ++i) {
        const uint8_t* s = &picture.pixels[i * 4];
        const double a = s[3] / 255.0;
        const double out_d = std::max(0.0, to_inside[i] - 0.5);
        const double g = out_d < kGlow ? kGlowAlpha * std::pow(1 - out_d / kGlow, 1.6) : 0.0;
        const double m = kTint * std::max(0.0, 1 - std::max(0.0, to_outside[i] - 1.0) / kTintDepth);
        const double tinted[3] = {s[0] / 255.0 * (1 - m) + tint.r * m, s[1] / 255.0 * (1 - m) + tint.g * m,
                                  s[2] / 255.0 * (1 - m) + tint.b * m};
        const double base[3] = {glow.r, glow.g, glow.b};
        const double alpha = a + g * (1 - a);
        uint8_t* o = &out.pixels[i * 4];
        for (int k = 0; k < 3; ++k) {
            const double v = alpha > 0 ? (tinted[k] * a + base[k] * g * (1 - a)) / alpha : 0.0;
            o[k] = static_cast<uint8_t>(std::clamp(v * 255.0 + 0.5, 0.0, 255.0));
        }
        o[3] = static_cast<uint8_t>(std::clamp(alpha * 255.0 + 0.5, 0.0, 255.0));
    }
    return out;
}

// As the title's small pictures: the picture at 0.465 of its size, its
// body's thickness showing below it in a pale blue grey, on a soft black
// oval of shadow.
Rgba small(const Rgba& picture) {
    constexpr double kScale = 0.465, kThick = 6;
    const uint32_t sw = static_cast<uint32_t>(std::lround(picture.width * kScale));
    const uint32_t sh = static_cast<uint32_t>(std::lround(picture.height * kScale));
    const Rgba scaled = resize(picture, sw, sh);
    const int ox = static_cast<int>(kSmallWidth - sw) / 2, oy = -6;
    // The picture's silhouette in the small one, and how far its pixels are from it.
    std::vector<uint8_t> body(size_t{kSmallWidth} * kSmallHeight, 0);
    uint32_t lowest = 0;
    for (uint32_t y = 0; y < sh; ++y) {
        for (uint32_t x = 0; x < sw; ++x) {
            const int tx = ox + static_cast<int>(x), ty = oy + static_cast<int>(y);
            if (tx < 0 || ty < 0 || tx >= int(kSmallWidth) || ty >= int(kSmallHeight)) continue;
            if (scaled.at(x, y)[3] >= 128) {
                body[size_t(ty) * kSmallWidth + size_t(tx)] = 1;
                lowest = std::max(lowest, uint32_t(ty));
            }
        }
    }
    // The thickness: the silhouette moved down.
    std::vector<uint8_t> side(body.size(), 0);
    for (uint32_t y = 0; y < kSmallHeight; ++y) {
        for (uint32_t x = 0; x < kSmallWidth; ++x) {
            for (int k = 1; k <= int(kThick) && !side[size_t{y} * kSmallWidth + x]; ++k) {
                if (int(y) - k >= 0 && body[size_t(int(y) - k) * kSmallWidth + x]) side[size_t{y} * kSmallWidth + x] = 1;
            }
        }
    }
    std::vector<uint8_t> solid(body.size());
    for (size_t i = 0; i < solid.size(); ++i) solid[i] = body[i] || side[i];
    uint32_t left = kSmallWidth, right = 0;
    for (uint32_t y = 0; y < kSmallHeight; ++y) {
        for (uint32_t x = 0; x < kSmallWidth; ++x) {
            if (solid[size_t{y} * kSmallWidth + x]) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    const double left_right = right >= left ? right - left + 1.0 : 0.0;
    Rgba out = blank(kSmallWidth, kSmallHeight);
    for (uint32_t y = 0; y < kSmallHeight; ++y) {
        for (uint32_t x = 0; x < kSmallWidth; ++x) {
            const size_t i = size_t{y} * kSmallWidth + x;
            uint8_t* o = out.at(x, y);

            // The shadow: a long soft oval under the body.
            double r = 0, g = 0, b = 0, a = 0;
            if (!solid[i]) {
                const double dx = (x + 0.5 - kSmallWidth / 2.0) / (left_right / 2.0 + 4), dy = (y + 0.5 - lowest) / 14.0;
                const double e = 1 - dx * dx - dy * dy;
                a = e > 0 ? 0.75 * std::pow(e, 1.3) : 0.0;
            }
            if (side[i] && !body[i]) {
                r = 0xC5 / 255.0, g = 0xD6 / 255.0, b = 0xDB / 255.0, a = 1;
            }
            const int sx = int(x) - ox, sy = int(y) - oy;
            if (sx >= 0 && sy >= 0 && sx < int(sw) && sy < int(sh)) {
                const uint8_t* s = scaled.at(uint32_t(sx), uint32_t(sy));
                const double sa = s[3] / 255.0, na = sa + a * (1 - sa);
                if (na > 0) {
                    r = (s[0] / 255.0 * sa + r * a * (1 - sa)) / na;
                    g = (s[1] / 255.0 * sa + g * a * (1 - sa)) / na;
                    b = (s[2] / 255.0 * sa + b * a * (1 - sa)) / na;
                }
                a = na;
            }
            o[0] = static_cast<uint8_t>(std::lround(r * 255));
            o[1] = static_cast<uint8_t>(std::lround(g * 255));
            o[2] = static_cast<uint8_t>(std::lround(b * 255));
            o[3] = static_cast<uint8_t>(std::lround(a * 255));
        }
    }
    return out;
}

} // namespace cafe::title::pictures
