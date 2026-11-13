package main

import "fmt"

const N = 350
const MAXITER = 50

func run(size, maxiter int) int64 {
	var total int64
	for y := 0; y < size; y++ {
		ci := 2.0*float64(y)/float64(size) - 1.0
		for x := 0; x < size; x++ {
			cr := 2.0*float64(x)/float64(size) - 1.5
			zr := 0.0
			zi := 0.0
			i := 0
			for i < maxiter && zr*zr+zi*zi <= 4.0 {
				t := zr*zr - zi*zi + cr
				zi = 2.0*zr*zi + ci
				zr = t
				i++
			}
			total += int64(i)
		}
	}
	return total
}

func main() {
	fmt.Println(run(N, MAXITER))
}
