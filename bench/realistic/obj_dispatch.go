package main

import "fmt"

const N = 640000

type Shape interface {
	Area() int
}

type Rect struct {
	w, h int
}

func (r *Rect) Area() int { return r.w * r.h }

type Square struct {
	s int
}

func (s *Square) Area() int { return s.s * s.s }

type Empty struct{}

func (e *Empty) Area() int { return 0 }

func main() {
	shapes := make([]Shape, 0, 100)
	for i := 0; i < 100; i++ {
		if i%3 == 0 {
			shapes = append(shapes, &Empty{})
		} else if i%3 == 1 {
			shapes = append(shapes, &Rect{i, i + 1})
		} else {
			shapes = append(shapes, &Square{i})
		}
	}
	total := 0
	for r := 0; r < N; r++ {
		for _, s := range shapes {
			total += s.Area()
		}
	}
	fmt.Println(total)
}
