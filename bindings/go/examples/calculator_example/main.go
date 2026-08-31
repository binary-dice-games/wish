// MIT License © 2025 Binary Dice Games

// Calculator example using the wish Go client binding. Mirrors
// bindings/cpp/examples/calculator.cpp / bindings/rust/examples/
// calculator.rs -- a *client only*. Start a wish server first (it owns the
// window/renderer), then point this program at it:
//
//	build/app/wish server --transport=tcp --port=7070 --renderer=console
//	go run ./examples/calculator_example -transport=tcp -host=127.0.0.1 -port=7070
//
// Run with: go run ./examples/calculator_example [-transport=tcp|pipe|term]
//
//	[-host=HOST] [-port=PORT] [-name=PATH] [-theme=dark|light|classic]
package main

import (
	"flag"
	"fmt"
	"strconv"
	"strings"
	"sync"

	"github.com/binary-dice-games/wish/bindings/go/wish"
)

const calcDesc = `{
  "type": "Window",
  "title": "Calculator",
  "width": 328,
  "height": 420,
  "closable": true,
  "children": {
    "display": { "type": "Label", "text": "0" },
    "sep":     { "type": "Separator" },
    "row0": {
      "type": "HorizontalLayout", "spacing": 6,
      "children": {
        "c":   { "type": "Button", "label": "C",   "width": 72, "height": 52 },
        "div": { "type": "Button", "label": "/",   "width": 72, "height": 52 },
        "mul": { "type": "Button", "label": "*",   "width": 72, "height": 52 },
        "bsp": { "type": "Button", "label": "<-",  "width": 72, "height": 52 }
      }
    },
    "row1": {
      "type": "HorizontalLayout", "spacing": 6,
      "children": {
        "n7":  { "type": "Button", "label": "7", "width": 72, "height": 52 },
        "n8":  { "type": "Button", "label": "8", "width": 72, "height": 52 },
        "n9":  { "type": "Button", "label": "9", "width": 72, "height": 52 },
        "sub": { "type": "Button", "label": "-", "width": 72, "height": 52 }
      }
    },
    "row2": {
      "type": "HorizontalLayout", "spacing": 6,
      "children": {
        "n4":  { "type": "Button", "label": "4", "width": 72, "height": 52 },
        "n5":  { "type": "Button", "label": "5", "width": 72, "height": 52 },
        "n6":  { "type": "Button", "label": "6", "width": 72, "height": 52 },
        "add": { "type": "Button", "label": "+", "width": 72, "height": 52 }
      }
    },
    "row3": {
      "type": "HorizontalLayout", "spacing": 6,
      "children": {
        "n1": { "type": "Button", "label": "1", "width": 72, "height": 52 },
        "n2": { "type": "Button", "label": "2", "width": 72, "height": 52 },
        "n3": { "type": "Button", "label": "3", "width": 72, "height": 52 },
        "eq": { "type": "Button", "label": "=", "width": 72, "height": 52 }
      }
    },
    "row4": {
      "type": "HorizontalLayout", "spacing": 6,
      "children": {
        "n0":  { "type": "Button", "label": "0",   "width": 72, "height": 52 },
        "dot": { "type": "Button", "label": ".",   "width": 72, "height": 52 },
        "pm":  { "type": "Button", "label": "+/-", "width": 72, "height": 52 },
        "pct": { "type": "Button", "label": "%",   "width": 72, "height": 52 }
      }
    }
  }
}`

func check(err error) {
	if err != nil {
		panic(err)
	}
}

// calcState is shared between button event handlers, which each run on the
// RMI worker thread (possibly concurrently with the main goroutine still
// setting up later handlers), so access is guarded by a mutex.
type calcState struct {
	mu          sync.Mutex
	display     string
	operand     float64
	pendingOp   byte
	fresh       bool
	displayProx *wish.Proxy
}

func (s *calcState) updateDisplay() {
	s.mu.Lock()
	text := s.display
	s.mu.Unlock()
	f, _ := wish.NewValue()
	defer f.Close()
	check(f.SetString("text", text))
	check(s.displayProx.Set(f, -1))
}

func main() {
	transport := flag.String("transport", "tcp", "transport: tcp|pipe|term")
	host := flag.String("host", "127.0.0.1", "server host (tcp transport)")
	port := flag.Int("port", 7070, "server port (tcp transport)")
	name := flag.String("name", "", "pipe/socket path (pipe transport)")
	theme := flag.String("theme", "wish", "style preset: wish|dark|light|classic")
	flag.Parse()

	var client *wish.Client
	var err error
	switch *transport {
	case "pipe":
		fmt.Printf("[Client] connecting to pipe %s ...\n", *name)
		client, err = wish.NewPipeClient(*name)
	case "term":
		fmt.Println("[Client] connecting via inherited stdio (--transport=term) ...")
		client, err = wish.NewTermClient()
	default:
		fmt.Printf("[Client] connecting to %s:%d ...\n", *host, *port)
		client, err = wish.NewTCPClient(*host, uint16(*port))
	}
	check(err)
	defer client.Destroy()

	runErr := client.Run(func(c *wish.Client) {
		check(c.SetStylePreset(*theme))
		check(c.RegisterTemplate("calc", calcDesc))

		root, err := c.InstantiateTemplate("calc", "calc")
		check(err)
		check(root.OnEvent("closed", func(params *wish.Value) {
			fmt.Println("[calc] window closed -- quitting")
			c.Quit()
		}))

		disp, err := c.ProxyGet("calc.display")
		check(err)
		state := &calcState{display: "0", fresh: true, displayProx: disp}

		button := func(path string) *wish.Proxy {
			p, err := c.ProxyGet("calc." + path)
			check(err)
			return p
		}

		var buttons []*wish.Proxy
		digits := []struct {
			path string
			ch   byte
		}{
			{"row1.n7", '7'}, {"row1.n8", '8'}, {"row1.n9", '9'},
			{"row2.n4", '4'}, {"row2.n5", '5'}, {"row2.n6", '6'},
			{"row3.n1", '1'}, {"row3.n2", '2'}, {"row3.n3", '3'},
			{"row4.n0", '0'},
		}
		for _, d := range digits {
			b := button(d.path)
			ch := d.ch
			check(b.OnEvent("clicked", func(_ *wish.Value) {
				fmt.Printf("[calc] digit '%c' clicked\n", ch)
				state.mu.Lock()
				if state.fresh {
					state.display = string(ch)
					state.fresh = false
				} else {
					state.display += string(ch)
				}
				state.mu.Unlock()
				state.updateDisplay()
			}))
			buttons = append(buttons, b)
		}

		ops := []struct {
			path string
			op   byte
		}{
			{"row0.div", '/'}, {"row0.mul", '*'}, {"row1.sub", '-'}, {"row2.add", '+'},
		}
		for _, o := range ops {
			b := button(o.path)
			op := o.op
			check(b.OnEvent("clicked", func(_ *wish.Value) {
				fmt.Printf("[calc] op '%c' clicked\n", op)
				state.mu.Lock()
				state.operand, _ = strconv.ParseFloat(state.display, 64)
				state.pendingOp = op
				state.fresh = true
				state.mu.Unlock()
			}))
			buttons = append(buttons, b)
		}

		cBtn := button("row0.c")
		check(cBtn.OnEvent("clicked", func(_ *wish.Value) {
			fmt.Println("[calc] C (clear) clicked")
			state.mu.Lock()
			state.display = "0"
			state.operand = 0
			state.pendingOp = 0
			state.fresh = true
			state.mu.Unlock()
			state.updateDisplay()
		}))
		buttons = append(buttons, cBtn)

		bsp := button("row0.bsp")
		check(bsp.OnEvent("clicked", func(_ *wish.Value) {
			fmt.Println("[calc] <- (backspace) clicked")
			state.mu.Lock()
			if len(state.display) > 1 {
				state.display = state.display[:len(state.display)-1]
			} else {
				state.display = "0"
			}
			state.mu.Unlock()
			state.updateDisplay()
		}))
		buttons = append(buttons, bsp)

		eq := button("row3.eq")
		check(eq.OnEvent("clicked", func(_ *wish.Value) {
			fmt.Println("[calc] = (equals) clicked")
			state.mu.Lock()
			rhs, _ := strconv.ParseFloat(state.display, 64)
			var result float64
			switch state.pendingOp {
			case '+':
				result = state.operand + rhs
			case '-':
				result = state.operand - rhs
			case '*':
				result = state.operand * rhs
			case '/':
				if rhs != 0 {
					result = state.operand / rhs
				}
			default:
				result = rhs
			}
			if result == float64(int64(result)) && result < 1e12 && result > -1e12 {
				state.display = strconv.FormatInt(int64(result), 10)
			} else {
				state.display = strconv.FormatFloat(result, 'g', -1, 64)
			}
			state.pendingOp = 0
			state.fresh = true
			state.mu.Unlock()
			state.updateDisplay()
		}))
		buttons = append(buttons, eq)

		dot := button("row4.dot")
		check(dot.OnEvent("clicked", func(_ *wish.Value) {
			fmt.Println("[calc] . (dot) clicked")
			state.mu.Lock()
			if !strings.Contains(state.display, ".") {
				state.display += "."
			}
			state.fresh = false
			state.mu.Unlock()
			state.updateDisplay()
		}))
		buttons = append(buttons, dot)

		pm := button("row4.pm")
		check(pm.OnEvent("clicked", func(_ *wish.Value) {
			fmt.Println("[calc] +/- clicked")
			state.mu.Lock()
			if state.display != "" && state.display != "0" {
				if strings.HasPrefix(state.display, "-") {
					state.display = state.display[1:]
				} else {
					state.display = "-" + state.display
				}
			}
			state.mu.Unlock()
			state.updateDisplay()
		}))
		buttons = append(buttons, pm)

		pct := button("row4.pct")
		check(pct.OnEvent("clicked", func(_ *wish.Value) {
			fmt.Println("[calc] % clicked")
			state.mu.Lock()
			v, _ := strconv.ParseFloat(state.display, 64)
			state.display = strconv.FormatFloat(v/100.0, 'g', -1, 64)
			state.mu.Unlock()
			state.updateDisplay()
		}))
		buttons = append(buttons, pct)

		fmt.Println("[calc] ready -- waiting for quit()")
		c.Wait()
		fmt.Println("[calc] session ending")

		for _, b := range buttons {
			b.Close()
		}
		disp.Close()
		root.Close()
		check(c.Release("calc"))
	})

	if runErr != nil {
		fmt.Printf("[Client] error: %v\n", runErr)
		return
	}
	fmt.Println("[Client] done.")
}
