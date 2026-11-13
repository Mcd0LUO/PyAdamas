package main

import "fmt"

const N = 8

func fannkuch(n int) (int, int) {
	perm := make([]int, 0, n)
	count := make([]int, 0, n)
	for i := 0; i < n; i++ {
		perm = append(perm, i)
		count = append(count, 0)
	}
	maxFlips := 0
	checksum := 0
	permCount := 0
	for {
		k := perm[0]
		if k != 0 {
			temp := make([]int, n)
			copy(temp, perm)
			flips := 0
			for temp[0] != 0 {
				m := temp[0]
				i := 0
				for i < m {
					t := temp[i]
					temp[i] = temp[m]
					temp[m] = t
					i++
					m--
				}
				flips++
			}
			if flips > maxFlips {
				maxFlips = flips
			}
			if permCount%2 == 0 {
				checksum += flips
			} else {
				checksum -= flips
			}
		}
		permCount++
		i := 1
		for {
			if i >= n {
				return checksum, maxFlips
			}
			if count[i] < i {
				break
			}
			count[i] = 0
			i++
		}
		first := perm[0]
		for j := 0; j < i; j++ {
			perm[j] = perm[j+1]
		}
		perm[i] = first
		count[i]++
	}
}

func main() {
	c, m := fannkuch(N)
	fmt.Println(c)
	fmt.Println(m)
}
