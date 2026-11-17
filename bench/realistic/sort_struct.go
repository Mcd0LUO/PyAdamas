package main

import (
	"fmt"
	"sort"
)

const N = 3900

type Rec struct {
	a, b, c int
}

func makeRecords(seed int, n int) []Rec {
	out := make([]Rec, 0, n)
	x := seed
	for i := 0; i < n; i++ {
		x = (x*1103515245 + 12345) % 2147483648
		a := x % 1000
		x = (x*1103515245 + 12345) % 2147483648
		b := x % 1000
		out = append(out, Rec{a, b, a + b})
	}
	return out
}

func aggregate(recs []Rec) int {
	s := 0
	for _, r := range recs {
		s += r.c
	}
	return s
}

func main() {
	total := 0
	for r := 0; r < N; r++ {
		recs := makeRecords(r, 500)
		sort.Slice(recs, func(i, j int) bool {
			if recs[i].a != recs[j].a {
				return recs[i].a < recs[j].a
			}
			if recs[i].b != recs[j].b {
				return recs[i].b < recs[j].b
			}
			return recs[i].c < recs[j].c
		})
		total += aggregate(recs)
	}
	fmt.Println(total)
}
