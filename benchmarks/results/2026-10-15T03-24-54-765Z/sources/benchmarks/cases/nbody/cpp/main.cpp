#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string_view>

/** One body's position, velocity and mass in normalized units. */
struct Body {
  double x, y, z, vx, vy, vz, mass;
};
using Bodies = std::array<Body, 5>;

/** Construct the initial system and cancel its total momentum. */
Bodies initial() {
  Bodies bodies{{{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0},
                 {4.841431442464721, -1.1603200440274284, -0.10362204447112311,
                  0.001660076642744037, 0.007699011184197404,
                  -0.0000690460016972063, 0.0009547919384243266},
                 {8.34336671824458, 4.124798564124305, -0.4035234171143214,
                  -0.002767425107268624, 0.004998528012349172,
                  0.000023041729757376393, 0.0002858859806661308},
                 {12.894369562139131, -15.111151401698631, -0.22330757889265573,
                  0.002964601375647616, 0.0023784717395948095,
                  -0.000029658956854023756, 0.00004366244043351563},
                 {15.379697114850917, -25.919314609987964, 0.17925877295037118,
                  0.0026806777249038932, 0.001628241700382423,
                  -0.00009515922545197159, 0.000051513890204661145}}};
  const double solar = 4.0 * 3.141592653589793 * 3.141592653589793;
  double px = 0, py = 0, pz = 0;
  for (auto &b : bodies) {
    b.vx *= 365.24;
    b.vy *= 365.24;
    b.vz *= 365.24;
    b.mass *= solar;
    px += b.vx * b.mass;
    py += b.vy * b.mass;
    pz += b.vz * b.mass;
  }
  bodies[0].vx = -px / solar;
  bodies[0].vy = -py / solar;
  bodies[0].vz = -pz / solar;
  return bodies;
}

/** Compute kinetic and pairwise potential energy. */
double energy(const Bodies &bodies) {
  double total = 0;
  for (size_t i = 0; i < bodies.size(); ++i) {
    const auto a = bodies[i];
    total += 0.5 * a.mass * (a.vx * a.vx + a.vy * a.vy + a.vz * a.vz);
    for (size_t j = i + 1; j < bodies.size(); ++j) {
      const auto b = bodies[j];
      const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
      total -= a.mass * b.mass / std::sqrt(dx * dx + dy * dy + dz * dz);
    }
  }
  return total;
}

/** Advance all velocities, then all positions by one time step. */
void advance(Bodies &bodies) {
  for (size_t i = 0; i < bodies.size(); ++i) {
    for (size_t j = i + 1; j < bodies.size(); ++j) {
      auto a = bodies[i];
      auto b = bodies[j];
      const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
      const double d2 = dx * dx + dy * dy + dz * dz;
      const double magnitude = 0.01 / (d2 * std::sqrt(d2));
      a.vx -= dx * b.mass * magnitude;
      a.vy -= dy * b.mass * magnitude;
      a.vz -= dz * b.mass * magnitude;
      b.vx += dx * a.mass * magnitude;
      b.vy += dy * a.mass * magnitude;
      b.vz += dz * a.mass * magnitude;
      bodies[i] = a;
      bodies[j] = b;
    }
  }
  for (auto &b : bodies) {
    b.x += 0.01 * b.vx;
    b.y += 0.01 * b.vy;
    b.z += 0.01 * b.vz;
  }
}

/** Validate input, simulate the requested steps and emit the checksum. */
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const std::string_view input(argv[1]);
  unsigned steps = 0;
  const auto parsed =
      std::from_chars(input.data(), input.data() + input.size(), steps);
  if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
      steps > 100000000)
    return 2;
  auto bodies = initial();
  const double before = energy(bodies);
  for (unsigned i = 0; i < steps; ++i)
    advance(bodies);
  std::cout << std::setprecision(17) << before << ' ' << energy(bodies) << '\n';
}
