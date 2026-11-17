package main

import "fmt"

const N = 26000

type Item struct {
	t    int
	cost int
}

func simulate(steps int) int {
	pending := make([]Item, 0, 64)
	done := 0
	for t := 0; t < steps; t++ {
		if t%3 == 0 {
			pending = append(pending, Item{t, t%7 + 1})
		}
		i := 0
		for i < len(pending) {
			item := pending[i]
			if item.t+item.cost <= t {
				done++
				pending = append(pending[:i], pending[i+1:]...)
			} else {
				i++
			}
		}
	}
	return done + len(pending)
}

func main() {
	total := 0
	for r := 0; r < N; r++ {
		total += simulate(200)
	}
	fmt.Println(total)
}
