package main

import (
	"fmt"
	"math"
)

const N = 600

func evalA(i, j int) float64 {
	return 1.0 / float64((i+j)*(i+j+1)/2+i+1)
}

func mulATA(v, out []float64, n int) {
	for i := 0; i < n; i++ {
		total := 0.0
		for j := 0; j < n; j++ {
			total += evalA(i, j) * v[j]
		}
		out[i] = total
	}
}

func mulA(u, out []float64, n int) {
	for i := 0; i < n; i++ {
		total := 0.0
		for j := 0; j < n; j++ {
			total += evalA(j, i) * u[j]
		}
		out[i] = total
	}
}

func main() {
	u := make([]float64, N)
	v := make([]float64, N)
	tmp := make([]float64, N)
	for i := 0; i < N; i++ {
		u[i] = 1.0
	}
	for k := 0; k < 10; k++ {
		mulATA(u, v, N)
		mulA(v, tmp, N)
	}
	vbv := 0.0
	vvv := 0.0
	for i := 0; i < N; i++ {
		vbv += u[i] * tmp[i]
		vvv += tmp[i] * tmp[i]
	}
	fmt.Println(int64(math.Sqrt(vbv/vvv) * 1000000.0))
}
