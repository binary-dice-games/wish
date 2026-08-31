// MIT License © 2025 Binary Dice Games
//
// Basic wish server, runnable purely from Rust, using the wish-server crate.
//
// Port of bindings/python/examples/basic_server_example.py -- hosts a real
// wish session (the same bdg::wish::server the `wish server` CLI uses: real
// per-widget proxies, real events) and renders it with --renderer=sdl3 (a
// real window), --renderer=web (a browser tab), or --renderer=console (a
// text dump to stdout, no display needed). Any ABI-based client can connect
// to it exactly as it would to the compiled `wish server` binary:
//
//   cargo run --example basic_server -- --transport=tcp --port=7070 --renderer=console
//   # in another terminal (from bindings/rust/):
//   cargo run --example calculator -- --transport=tcp --host=127.0.0.1 --port=7070
//
// Usage: basic_server [--transport=tcp|pipe] [--host=HOST] [--port=PORT]
//                     [--name=PATH] [--renderer=sdl3|web|console]
//                     [--title=TITLE] [--width=W] [--height=H]
//                     [--web_bind=HOST] [--web_port=PORT] [--verbose]

use std::io::Read;

use wish_server::{Params, Server};

fn arg(args: &[String], key: &str, default: &str) -> String {
    let prefix = format!("--{key}=");
    args.iter()
        .find_map(|a| a.strip_prefix(&prefix).map(str::to_string))
        .unwrap_or_else(|| default.to_string())
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    let transport = arg(&args, "transport", "tcp");
    let host = arg(&args, "host", "127.0.0.1");
    let port: u16 = arg(&args, "port", "7070").parse().expect("invalid --port");
    let name = arg(&args, "name", "");
    let renderer = arg(&args, "renderer", "sdl3");
    let title = arg(&args, "title", "wish");
    let width: i32 = arg(&args, "width", "1280")
        .parse()
        .expect("invalid --width");
    let height: i32 = arg(&args, "height", "720")
        .parse()
        .expect("invalid --height");
    let font_size: i32 = arg(&args, "font_size", "16")
        .parse()
        .expect("invalid --font_size");
    let web_bind = arg(&args, "web_bind", "127.0.0.1");
    let web_port: i32 = arg(&args, "web_port", "8080")
        .parse()
        .expect("invalid --web_port");
    let verbose = args.iter().any(|a| a == "--verbose");

    let mut server = match transport.as_str() {
        "tcp" => Server::tcp(&host, port),
        "pipe" => Server::pipe(&name),
        other => panic!("unknown --transport={other} (expected tcp or pipe)"),
    };

    if verbose {
        server.set_log_level("trace").unwrap();
    }

    let mut params = Params::new();
    params.set_string("title", &title);
    params.set_int("width", width);
    params.set_int("height", height);
    params.set_int("font_size", font_size);
    params.set_string("web_bind", &web_bind);
    params.set_int("web_port", web_port);

    server
        .start(&renderer, Some(&params))
        .expect("server failed to start");

    if transport == "tcp" {
        println!("[wish] listening on {host}:{port}");
    } else {
        println!("[wish] listening on pipe {name}");
    }
    if renderer == "web" {
        println!("[wish] open http://{web_bind}:{web_port} in a browser");
    }

    if renderer == "sdl3" {
        // The SDL3 window drives the quit signal itself.
        while !server.should_quit() {
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
    } else {
        println!("[wish] Press Enter to stop...");
        let _ = std::io::stdin().read(&mut [0u8]).ok();
    }

    server.stop().unwrap();
    println!("[wish] stopped.");
}
