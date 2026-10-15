using System;
using System.Globalization;

/// <summary>One body's position, velocity and mass in normalized units.</summary>
struct Body
{
    public double X, Y, Z, Vx, Vy, Vz, Mass;

    /// <summary>Initialize the seven components of a body.</summary>
    public Body(double x, double y, double z, double vx, double vy, double vz, double mass)
    { X = x; Y = y; Z = z; Vx = vx; Vy = vy; Vz = vz; Mass = mass; }
}

/// <summary>Run the five-body simulation with ordinary managed arrays.</summary>
static class Program
{
    /// <summary>Construct the initial system and cancel its total momentum.</summary>
    static Body[] Initial()
    {
        Body[] bodies = {
            new Body(0, 0, 0, 0, 0, 0, 1),
            new Body(4.841431442464721, -1.1603200440274284, -0.10362204447112311, 0.001660076642744037, 0.007699011184197404, -0.0000690460016972063, 0.0009547919384243266),
            new Body(8.34336671824458, 4.124798564124305, -0.4035234171143214, -0.002767425107268624, 0.004998528012349172, 0.000023041729757376393, 0.0002858859806661308),
            new Body(12.894369562139131, -15.111151401698631, -0.22330757889265573, 0.002964601375647616, 0.0023784717395948095, -0.000029658956854023756, 0.00004366244043351563),
            new Body(15.379697114850917, -25.919314609987964, 0.17925877295037118, 0.0026806777249038932, 0.001628241700382423, -0.00009515922545197159, 0.000051513890204661145)
        };
        double solar = 4.0 * Math.PI * Math.PI;
        double px = 0, py = 0, pz = 0;
        for (int i = 0; i < bodies.Length; ++i)
        {
            var b = bodies[i];
            b.Vx *= 365.24; b.Vy *= 365.24; b.Vz *= 365.24; b.Mass *= solar;
            px += b.Vx * b.Mass; py += b.Vy * b.Mass; pz += b.Vz * b.Mass;
            bodies[i] = b;
        }
        bodies[0].Vx = -px / solar; bodies[0].Vy = -py / solar; bodies[0].Vz = -pz / solar;
        return bodies;
    }

    /// <summary>Compute kinetic and pairwise potential energy.</summary>
    static double Energy(Body[] bodies)
    {
        double total = 0;
        for (int i = 0; i < bodies.Length; ++i)
        {
            var a = bodies[i];
            total += 0.5 * a.Mass * (a.Vx * a.Vx + a.Vy * a.Vy + a.Vz * a.Vz);
            for (int j = i + 1; j < bodies.Length; ++j)
            {
                var b = bodies[j];
                double dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
                total -= a.Mass * b.Mass / Math.Sqrt(dx * dx + dy * dy + dz * dz);
            }
        }
        return total;
    }

    /// <summary>Advance velocities, then positions by one time step.</summary>
    static void Advance(Body[] bodies)
    {
        for (int i = 0; i < bodies.Length; ++i)
        {
            for (int j = i + 1; j < bodies.Length; ++j)
            {
                var a = bodies[i]; var b = bodies[j];
                double dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
                double d2 = dx * dx + dy * dy + dz * dz;
                double magnitude = 0.01 / (d2 * Math.Sqrt(d2));
                a.Vx -= dx * b.Mass * magnitude; a.Vy -= dy * b.Mass * magnitude; a.Vz -= dz * b.Mass * magnitude;
                b.Vx += dx * a.Mass * magnitude; b.Vy += dy * a.Mass * magnitude; b.Vz += dz * a.Mass * magnitude;
                bodies[i] = a; bodies[j] = b;
            }
        }
        for (int i = 0; i < bodies.Length; ++i)
        {
            var b = bodies[i];
            b.X += 0.01 * b.Vx; b.Y += 0.01 * b.Vy; b.Z += 0.01 * b.Vz;
            bodies[i] = b;
        }
    }

    /// <summary>Validate input and emit full-precision energy checksums.</summary>
    static int Main(string[] args)
    {
        if (args.Length != 1 || !int.TryParse(args[0], NumberStyles.None, CultureInfo.InvariantCulture, out int steps)
            || steps < 0 || steps > 100000000) return 2;
        var bodies = Initial();
        double before = Energy(bodies);
        for (int step = 0; step < steps; ++step) Advance(bodies);
        Console.WriteLine(before.ToString("R", CultureInfo.InvariantCulture) + " " +
            Energy(bodies).ToString("R", CultureInfo.InvariantCulture));
        return 0;
    }
}
