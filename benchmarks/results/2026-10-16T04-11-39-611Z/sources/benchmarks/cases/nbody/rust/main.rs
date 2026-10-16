/** One body's position, velocity and mass in normalized units. */
#[derive(Clone, Copy)]
struct Body {
    x: f64,
    y: f64,
    z: f64,
    vx: f64,
    vy: f64,
    vz: f64,
    mass: f64,
}

/** Construct the initial system and cancel its total momentum. */
fn initial() -> [Body; 5] {
    let mut bodies = [
        Body {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            vx: 0.0,
            vy: 0.0,
            vz: 0.0,
            mass: 1.0,
        },
        Body {
            x: 4.841431442464721,
            y: -1.1603200440274284,
            z: -0.10362204447112311,
            vx: 0.001660076642744037,
            vy: 0.007699011184197404,
            vz: -0.0000690460016972063,
            mass: 0.0009547919384243266,
        },
        Body {
            x: 8.34336671824458,
            y: 4.124798564124305,
            z: -0.4035234171143214,
            vx: -0.002767425107268624,
            vy: 0.004998528012349172,
            vz: 0.000023041729757376393,
            mass: 0.0002858859806661308,
        },
        Body {
            x: 12.894369562139131,
            y: -15.111151401698631,
            z: -0.22330757889265573,
            vx: 0.002964601375647616,
            vy: 0.0023784717395948095,
            vz: -0.000029658956854023756,
            mass: 0.00004366244043351563,
        },
        Body {
            x: 15.379697114850917,
            y: -25.919314609987964,
            z: 0.17925877295037118,
            vx: 0.0026806777249038932,
            vy: 0.001628241700382423,
            vz: -0.00009515922545197159,
            mass: 0.000051513890204661145,
        },
    ];
    let solar = 4.0 * std::f64::consts::PI * std::f64::consts::PI;
    let (mut px, mut py, mut pz) = (0.0, 0.0, 0.0);
    for b in &mut bodies {
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
    bodies
}

/** Compute kinetic and pairwise potential energy. */
fn energy(bodies: &[Body; 5]) -> f64 {
    let mut total = 0.0;
    for (i, a) in bodies.iter().enumerate() {
        total += 0.5 * a.mass * (a.vx * a.vx + a.vy * a.vy + a.vz * a.vz);
        for b in &bodies[i + 1..] {
            let (dx, dy, dz) = (a.x - b.x, a.y - b.y, a.z - b.z);
            total -= a.mass * b.mass / (dx * dx + dy * dy + dz * dz).sqrt();
        }
    }
    total
}

/** Advance all velocities, then all positions by one time step. */
fn advance(bodies: &mut [Body; 5]) {
    for i in 0..5 {
        for j in i + 1..5 {
            let (mut a, mut b) = (bodies[i], bodies[j]);
            let (dx, dy, dz) = (a.x - b.x, a.y - b.y, a.z - b.z);
            let d2 = dx * dx + dy * dy + dz * dz;
            let magnitude = 0.01 / (d2 * d2.sqrt());
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
    for b in bodies {
        b.x += 0.01 * b.vx;
        b.y += 0.01 * b.vy;
        b.z += 0.01 * b.vz;
    }
}

/** Validate input, simulate the requested steps and emit the checksum. */
fn main() {
    let args: Vec<String> = std::env::args().collect();
    assert_eq!(args.len(), 2);
    let steps: u32 = args[1].parse().expect("invalid step count");
    assert!(steps <= 100000000);
    let mut bodies = initial();
    let before = energy(&bodies);
    for _ in 0..steps {
        advance(&mut bodies);
    }
    println!("{:.17} {:.17}", before, energy(&bodies));
}
