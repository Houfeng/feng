package main

import (
	"fmt"
	"os"
)

// main reads, validates and aggregates fixed-format ASCII log records.
func main() {
	if len(os.Args) != 2 {
		os.Exit(2)
	}
	data, err := os.ReadFile(os.Args[1])
	if err != nil || len(data)%8 != 0 {
		os.Exit(2)
	}
	var lines, warnings, errors, sum uint64
	for i := 0; i < len(data); i += 8 {
		level := data[i]
		if (level != 'I' && level != 'W' && level != 'E') || data[i+1] != ',' || data[i+7] != '\n' {
			os.Exit(2)
		}
		value := uint64(0)
		for j := i + 2; j < i+7; j++ {
			digit := data[j]
			if digit < '0' || digit > '9' {
				os.Exit(2)
			}
			value = value*10 + uint64(digit-'0')
		}
		lines++
		if level == 'W' {
			warnings++
		}
		if level == 'E' {
			errors++
		}
		sum += value
	}
	fmt.Println(lines, warnings, errors, sum)
}
