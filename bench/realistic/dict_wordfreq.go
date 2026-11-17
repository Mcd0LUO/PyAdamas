package main

import (
	"fmt"
	"sort"
)

const N = 12000

var WORDS = []string{"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta",
	"iota", "kappa", "lambda", "mu", "nu", "xi", "omicron", "pi"}

func makeText(seed int, n int) []string {
	out := make([]string, 0, n)
	x := seed
	for i := 0; i < n; i++ {
		x = (x*1103515245 + 12345) % 2147483648
		out = append(out, WORDS[x%16])
	}
	return out
}

func countWords(words []string) map[string]int {
	counts := make(map[string]int)
	for _, w := range words {
		counts[w]++
	}
	return counts
}

func topN(counts map[string]int, n int) []string {
	type kv struct {
		c int
		k string
	}
	items := make([]kv, 0, len(counts))
	for k, v := range counts {
		items = append(items, kv{v, k})
	}
	sort.Slice(items, func(i, j int) bool {
		if items[i].c != items[j].c {
			return items[i].c > items[j].c
		}
		return items[i].k < items[j].k
	})
	res := make([]string, 0, n)
	for i := 0; i < n; i++ {
		res = append(res, items[i].k)
	}
	return res
}

func main() {
	total := 0
	for r := 0; r < N; r++ {
		words := makeText(r, 400)
		counts := countWords(words)
		tops := topN(counts, 3)
		total += len(tops)
	}
	fmt.Println(total)
}
