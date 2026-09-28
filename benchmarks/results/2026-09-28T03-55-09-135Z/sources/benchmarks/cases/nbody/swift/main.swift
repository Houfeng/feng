import Foundation

/// One body's position, velocity and mass in normalized units.
struct Body { var x, y, z, vx, vy, vz, mass: Double }

/// Construct the system and cancel its total momentum.
func initial() -> [Body] {
  var bodies = [
    Body(x: 0.0, y: 0.0, z: 0.0, vx: 0.0, vy: 0.0, vz: 0.0, mass: 1.0),
    Body(
      x: 4.841431442464721, y: -1.1603200440274284, z: -0.10362204447112311,
      vx: 0.001660076642744037, vy: 0.007699011184197404, vz: -0.0000690460016972063,
      mass: 0.0009547919384243266),
    Body(
      x: 8.34336671824458, y: 4.124798564124305, z: -0.4035234171143214, vx: -0.002767425107268624,
      vy: 0.004998528012349172, vz: 0.000023041729757376393, mass: 0.0002858859806661308),
    Body(
      x: 12.894369562139131, y: -15.111151401698631, z: -0.22330757889265573,
      vx: 0.002964601375647616, vy: 0.0023784717395948095, vz: -0.000029658956854023756,
      mass: 0.00004366244043351563),
    Body(
      x: 15.379697114850917, y: -25.919314609987964, z: 0.17925877295037118,
      vx: 0.0026806777249038932, vy: 0.001628241700382423, vz: -0.00009515922545197159,
      mass: 0.000051513890204661145),
  ]
  let solar = 4.0 * Double.pi * Double.pi
  var px = 0.0
  var py = 0.0
  var pz = 0.0
  for i in bodies.indices {
    bodies[i].vx *= 365.24
    bodies[i].vy *= 365.24
    bodies[i].vz *= 365.24
    bodies[i].mass *= solar
    px += bodies[i].vx * bodies[i].mass
    py += bodies[i].vy * bodies[i].mass
    pz += bodies[i].vz * bodies[i].mass
  }
  bodies[0].vx = -px / solar
  bodies[0].vy = -py / solar
  bodies[0].vz = -pz / solar
  return bodies
}

/// Compute kinetic and pairwise potential energy.
func energy(_ bodies: [Body]) -> Double {
  var total = 0.0
  for i in bodies.indices {
    let a = bodies[i]
    total += 0.5 * a.mass * (a.vx * a.vx + a.vy * a.vy + a.vz * a.vz)
    for j in (i + 1)..<bodies.count {
      let b = bodies[j]
      let dx = a.x - b.x
      let dy = a.y - b.y
      let dz = a.z - b.z
      total -= a.mass * b.mass / (dx * dx + dy * dy + dz * dz).squareRoot()
    }
  }
  return total
}

/// Advance all velocities, then all positions by one time step.
func advance(_ bodies: inout [Body]) {
  for i in bodies.indices {
    for j in (i + 1)..<bodies.count {
      var a = bodies[i]
      var b = bodies[j]
      let dx = a.x - b.x
      let dy = a.y - b.y
      let dz = a.z - b.z
      let d2 = dx * dx + dy * dy + dz * dz
      let magnitude = 0.01 / (d2 * d2.squareRoot())
      a.vx -= dx * b.mass * magnitude
      a.vy -= dy * b.mass * magnitude
      a.vz -= dz * b.mass * magnitude
      b.vx += dx * a.mass * magnitude
      b.vy += dy * a.mass * magnitude
      b.vz += dz * a.mass * magnitude
      bodies[i] = a
      bodies[j] = b
    }
  }
  for i in bodies.indices {
    bodies[i].x += 0.01 * bodies[i].vx
    bodies[i].y += 0.01 * bodies[i].vy
    bodies[i].z += 0.01 * bodies[i].vz
  }
}

/// Validate input, run the simulation and print the energy checksum.
func main() {
  guard CommandLine.arguments.count == 2, let steps = Int(CommandLine.arguments[1]),
    steps >= 0, steps <= 100_000_000
  else { exit(2) }
  var bodies = initial()
  let before = energy(bodies)
  for _ in 0..<steps { advance(&bodies) }
  print(before, energy(bodies))
}
main()
