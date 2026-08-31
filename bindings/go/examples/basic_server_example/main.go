// MIT License © 2025 Binary Dice Games

// Basic wish server, runnable purely from Go, using the wishserver package.
//
// Port of bindings/python/examples/basic_server_example.py and
// bindings/rust/wish-server/examples/basic_server.rs -- hosts a real wish
// session (the same bdg::wish::server the "wish server" CLI uses: real
// per-widget proxies, real events) and renders it with -renderer=sdl3 (a
// real window), -renderer=web (a browser tab), or -renderer=console (a
// text dump to stdout, no display needed). Any ABI-based client can
// connect to it exactly as it would to the compiled "wish server" binary:
//
//	go run ./examples/basic_server_example -transport=tcp -port=7070 -renderer=console
//	# in another terminal:
//	go run ./examples/calculator_example -transport=tcp -host=127.0.0.1 -port=7070
//
// Run with: go run ./examples/basic_server_example [-transport=tcp|pipe]
//
//	[-host=HOST] [-port=PORT] [-name=PATH] [-renderer=sdl3|web|console]
//	[-title=TITLE] [-width=W] [-height=H] [-web_bind=HOST] [-web_port=PORT]
//	[-verbose]
package main

import (
	"bufio"
	"flag"
	"log"
	"os"
	"time"

	"github.com/binary-dice-games/wish/bindings/go/wishserver"
)

func main() {
	transport := flag.String("transport", "tcp", "tcp or pipe")
	host := flag.String("host", "127.0.0.1", "bind address (tcp)")
	port := flag.Int("port", 7070, "port (tcp)")
	name := flag.String("name", "", "pipe path (pipe)")
	renderer := flag.String("renderer", "sdl3", "sdl3, web, or console")
	title := flag.String("title", "wish", "window title (sdl3/web)")
	width := flag.Int("width", 1280, "window width (sdl3/web)")
	height := flag.Int("height", 720, "window height (sdl3/web)")
	fontSize := flag.Int("font_size", 16, "font size (sdl3/web)")
	webBind := flag.String("web_bind", "127.0.0.1", "web renderer bind address")
	webPort := flag.Int("web_port", 8080, "web renderer port")
	verbose := flag.Bool("verbose", false, "trace-level server logging")
	flag.Parse()

	var server *wishserver.Server
	var err error
	switch *transport {
	case "tcp":
		server, err = wishserver.NewTCPServer(*host, uint16(*port))
	case "pipe":
		server, err = wishserver.NewPipeServer(*name)
	default:
		log.Fatalf("unknown -transport=%s (expected tcp or pipe)", *transport)
	}
	if err != nil {
		log.Fatal(err)
	}
	defer server.Destroy()

	if *verbose {
		if err := server.SetLogLevel("trace"); err != nil {
			log.Fatal(err)
		}
	}

	params := wishserver.NewParams().
		SetString("title", *title).
		SetInt("width", int32(*width)).
		SetInt("height", int32(*height)).
		SetInt("font_size", int32(*fontSize)).
		SetString("web_bind", *webBind).
		SetInt("web_port", int32(*webPort))
	defer params.Close()

	if err := server.Start(*renderer, params); err != nil {
		log.Fatalf("server failed to start: %v", err)
	}

	if *transport == "tcp" {
		log.Printf("[wish] listening on %s:%d", *host, *port)
	} else {
		log.Printf("[wish] listening on pipe %s", *name)
	}
	if *renderer == "web" {
		log.Printf("[wish] open http://%s:%d in a browser", *webBind, *webPort)
	}

	if *renderer == "sdl3" {
		// The SDL3 window drives the quit signal itself.
		for !server.ShouldQuit() {
			time.Sleep(50 * time.Millisecond)
		}
	} else {
		log.Print("[wish] Press Enter to stop...")
		bufio.NewReader(os.Stdin).ReadString('\n')
	}

	if err := server.Stop(); err != nil {
		log.Fatal(err)
	}
	log.Print("[wish] stopped.")
}
