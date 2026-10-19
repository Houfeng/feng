'use strict';

/** A body's position, velocity and mass in normalized units. */
class Body {
  /** Initialize all components with a stable object shape. */
  constructor(x, y, z, vx, vy, vz, mass) {
    this.x = x; this.y = y; this.z = z;
    this.vx = vx; this.vy = vy; this.vz = vz; this.mass = mass;
  }
}

/** Construct the initial system and cancel its total momentum. */
function initial() {
  const bodies = [
            new Body(0, 0, 0, 0, 0, 0, 1),
            new Body(4.841431442464721, -1.1603200440274284, -0.10362204447112311, 0.001660076642744037, 0.007699011184197404, -0.0000690460016972063, 0.0009547919384243266),
            new Body(8.34336671824458, 4.124798564124305, -0.4035234171143214, -0.002767425107268624, 0.004998528012349172, 0.000023041729757376393, 0.0002858859806661308),
            new Body(12.894369562139131, -15.111151401698631, -0.22330757889265573, 0.002964601375647616, 0.0023784717395948095, -0.000029658956854023756, 0.00004366244043351563),
            new Body(15.379697114850917, -25.919314609987964, 0.17925877295037118, 0.0026806777249038932, 0.001628241700382423, -0.00009515922545197159, 0.000051513890204661145)
  ];
  const solar = 4.0 * Math.PI * Math.PI;
  let px = 0, py = 0, pz = 0;
  for (const b of bodies) {
    b.vx *= 365.24; b.vy *= 365.24; b.vz *= 365.24; b.mass *= solar;
    px += b.vx * b.mass; py += b.vy * b.mass; pz += b.vz * b.mass;
  }
  bodies[0].vx = -px / solar; bodies[0].vy = -py / solar; bodies[0].vz = -pz / solar;
  return bodies;
}

/** Compute kinetic and pairwise potential energy. */
function energy(bodies) {
  let total = 0;
  for (let i = 0; i < bodies.length; ++i) {
    const a = bodies[i];
    total += 0.5 * a.mass * (a.vx * a.vx + a.vy * a.vy + a.vz * a.vz);
    for (let j = i + 1; j < bodies.length; ++j) {
      const b = bodies[j];
      const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
      total -= a.mass * b.mass / Math.sqrt(dx * dx + dy * dy + dz * dz);
    }
  }
  return total;
}

/** Advance velocities, then positions by one time step. */
function advance(bodies) {
  for (let i = 0; i < bodies.length; ++i) {
    for (let j = i + 1; j < bodies.length; ++j) {
      const a = bodies[i], b = bodies[j];
      const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
      const d2 = dx * dx + dy * dy + dz * dz;
      const magnitude = 0.01 / (d2 * Math.sqrt(d2));
      a.vx -= dx * b.mass * magnitude; a.vy -= dy * b.mass * magnitude; a.vz -= dz * b.mass * magnitude;
      b.vx += dx * a.mass * magnitude; b.vy += dy * a.mass * magnitude; b.vz += dz * a.mass * magnitude;
    }
  }
  for (const b of bodies) {
    b.x += 0.01 * b.vx; b.y += 0.01 * b.vy; b.z += 0.01 * b.vz;
  }
}

if (process.argv.length !== 3 || !/^\d+$/.test(process.argv[2])) process.exit(2);
const steps = Number(process.argv[2]);
if (!Number.isInteger(steps) || steps < 0 || steps > 100000000) process.exit(2);
const bodies = initial();
const before = energy(bodies);
for (let step = 0; step < steps; ++step) advance(bodies);
console.log(before, energy(bodies));
