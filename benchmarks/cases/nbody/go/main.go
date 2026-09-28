package main

import (
	"fmt"
	"math"
	"os"
	"strconv"
)

// Body stores position, velocity and mass in normalized units.
type Body struct{ x, y, z, vx, vy, vz, mass float64 }

// initial constructs the system and cancels its total momentum.
func initial() [5]Body {
	bodies := [5]Body{
		{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0},
		{4.841431442464721, -1.1603200440274284, -0.10362204447112311, 0.001660076642744037, 0.007699011184197404, -0.0000690460016972063, 0.0009547919384243266},
		{8.34336671824458, 4.124798564124305, -0.4035234171143214, -0.002767425107268624, 0.004998528012349172, 0.000023041729757376393, 0.0002858859806661308},
		{12.894369562139131, -15.111151401698631, -0.22330757889265573, 0.002964601375647616, 0.0023784717395948095, -0.000029658956854023756, 0.00004366244043351563},
		{15.379697114850917, -25.919314609987964, 0.17925877295037118, 0.0026806777249038932, 0.001628241700382423, -0.00009515922545197159, 0.000051513890204661145},
	}
	solar := 4.0 * math.Pi * math.Pi
	px, py, pz := 0.0, 0.0, 0.0
	for i := range bodies {
		b := &bodies[i]
		b.vx *= 365.24
		b.vy *= 365.24
		b.vz *= 365.24
		b.mass *= solar
		px += b.vx * b.mass
		py += b.vy * b.mass
		pz += b.vz * b.mass
	}
	bodies[0].vx = -px / solar
	bodies[0].vy = -py / solar
	bodies[0].vz = -pz / solar
	return bodies
}

// energy computes kinetic and pairwise potential energy.
func energy(bodies *[5]Body) float64 {
	total := 0.0
	for i, a := range bodies {
		total += 0.5 * a.mass * (a.vx*a.vx + a.vy*a.vy + a.vz*a.vz)
		for j := i + 1; j < 5; j++ {
			b := bodies[j]
			dx, dy, dz := a.x-b.x, a.y-b.y, a.z-b.z
			total -= a.mass * b.mass / math.Sqrt(dx*dx+dy*dy+dz*dz)
		}
	}
	return total
}

// advance updates all velocities, followed by all positions.
func advance(bodies *[5]Body) {
	for i := 0; i < 5; i++ {
		for j := i + 1; j < 5; j++ {
			a, b := bodies[i], bodies[j]
			dx, dy, dz := a.x-b.x, a.y-b.y, a.z-b.z
			d2 := dx*dx + dy*dy + dz*dz
			magnitude := 0.01 / (d2 * math.Sqrt(d2))
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
	for i := range bodies {
		b := &bodies[i]
		b.x += 0.01 * b.vx
		b.y += 0.01 * b.vy
		b.z += 0.01 * b.vz
	}
}

// main validates input and emits the energy checksum after simulation.
func main() {
	if len(os.Args) != 2 {
		os.Exit(2)
	}
	steps, err := strconv.Atoi(os.Args[1])
	if err != nil || steps < 0 || steps > 100000000 {
		os.Exit(2)
	}
	bodies := initial()
	before := energy(&bodies)
	for i := 0; i < steps; i++ {
		advance(&bodies)
	}
	fmt.Printf("%.17g %.17g\n", before, energy(&bodies))
}
