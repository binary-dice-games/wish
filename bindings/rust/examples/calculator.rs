// MIT License © 2025 Binary Dice Games
//
// Calculator example using the wish Rust client binding.
//
// Port of bindings/cpp/examples/calculator.cpp / bindings/python/examples/
// calculator_example.py -- a *client only*. Start a wish server first (it
// owns the window/renderer), then point this program at it:
//
//   build/app/wish server --transport=tcp --port=7070 --renderer=console
//   cargo run --example calculator -- --transport=tcp --host=127.0.0.1 --port=7070
//
// Usage: calculator [--transport=tcp|pipe|term] [--host=HOST] [--port=PORT]
//                    [--name=PATH] [--theme=dark|light|classic]

use std::sync::{Arc, Mutex};

use wish::{Client, Proxy, Value};

const CALC_DESC: &str = r#"{
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
}"#;

struct Args {
    transport: String,
    host: String,
    port: u16,
    name: String,
    theme: String,
}

fn parse_args() -> Args {
    let mut args = Args {
        transport: "tcp".to_string(),
        host: "127.0.0.1".to_string(),
        port: 7070,
        name: String::new(),
        theme: "wish".to_string(),
    };
    for arg in std::env::args().skip(1) {
        if let Some(v) = arg.strip_prefix("--transport=") {
            args.transport = v.to_string();
        } else if let Some(v) = arg.strip_prefix("--host=") {
            args.host = v.to_string();
        } else if let Some(v) = arg.strip_prefix("--port=") {
            args.port = v.parse().expect("--port must be a valid u16");
        } else if let Some(v) = arg.strip_prefix("--name=") {
            args.name = v.to_string();
        } else if let Some(v) = arg.strip_prefix("--theme=") {
            args.theme = v.to_string();
        }
    }
    args
}

// Calculator state shared between event handlers via `Arc<Mutex<_>>` --
// `Proxy::on_event`'s closure must be `'static`, so it cannot borrow the
// stack-local state that `client.run()`'s closure body owns.
struct CalcState {
    display: String,
    operand: f64,
    pending_op: char,
    fresh: bool,
}

fn update_display(disp: &Proxy, state: &Mutex<CalcState>) {
    let mut f = Value::new();
    f.set_string("text", &state.lock().unwrap().display)
        .unwrap();
    disp.set(&f, -1).unwrap();
}

fn main() {
    let args = parse_args();

    let client = match args.transport.as_str() {
        "pipe" => {
            println!("[Client] connecting to pipe {} ...", args.name);
            Client::pipe(&args.name)
        }
        "term" => {
            println!("[Client] connecting via inherited stdio (--transport=term) ...");
            Client::term()
        }
        _ => {
            println!("[Client] connecting to {}:{} ...", args.host, args.port);
            Client::tcp(&args.host, args.port)
        }
    };

    let result = client.run(|c| {
        c.set_style_preset(&args.theme).unwrap();
        c.register_template("calc", CALC_DESC).unwrap();

        let mut root = c.instantiate_template("calc", "calc").unwrap();
        let quit = c.quit_handle();
        root.on_event("closed", move |_| {
            println!("[calc] window closed -- quitting");
            quit.quit();
        })
        .unwrap();

        let disp = Arc::new(c.proxy_get("calc.display").unwrap());
        let state = Arc::new(Mutex::new(CalcState {
            display: "0".to_string(),
            operand: 0.0,
            pending_op: '\0',
            fresh: true,
        }));

        let mut buttons: Vec<Proxy> = Vec::new();
        let button = |path: &str| c.proxy_get(&format!("calc.{path}")).unwrap();

        for (path, ch) in [
            ("row1.n7", '7'),
            ("row1.n8", '8'),
            ("row1.n9", '9'),
            ("row2.n4", '4'),
            ("row2.n5", '5'),
            ("row2.n6", '6'),
            ("row3.n1", '1'),
            ("row3.n2", '2'),
            ("row3.n3", '3'),
            ("row4.n0", '0'),
        ] {
            let mut b = button(path);
            let (disp, state) = (disp.clone(), state.clone());
            b.on_event("clicked", move |_| {
                println!("[calc] digit '{ch}' clicked");
                {
                    let mut s = state.lock().unwrap();
                    if s.fresh {
                        s.display = ch.to_string();
                        s.fresh = false;
                    } else {
                        s.display.push(ch);
                    }
                }
                update_display(&disp, &state);
            })
            .unwrap();
            buttons.push(b);
        }

        for (path, op) in [
            ("row0.div", '/'),
            ("row0.mul", '*'),
            ("row1.sub", '-'),
            ("row2.add", '+'),
        ] {
            let mut b = button(path);
            let state = state.clone();
            b.on_event("clicked", move |_| {
                println!("[calc] op '{op}' clicked");
                let mut s = state.lock().unwrap();
                s.operand = s.display.parse().unwrap_or(0.0);
                s.pending_op = op;
                s.fresh = true;
            })
            .unwrap();
            buttons.push(b);
        }

        let mut c_btn = button("row0.c");
        {
            let (disp, state) = (disp.clone(), state.clone());
            c_btn
                .on_event("clicked", move |_| {
                    println!("[calc] C (clear) clicked");
                    {
                        let mut s = state.lock().unwrap();
                        s.display = "0".to_string();
                        s.operand = 0.0;
                        s.pending_op = '\0';
                        s.fresh = true;
                    }
                    update_display(&disp, &state);
                })
                .unwrap();
        }
        buttons.push(c_btn);

        let mut bsp = button("row0.bsp");
        {
            let (disp, state) = (disp.clone(), state.clone());
            bsp.on_event("clicked", move |_| {
                println!("[calc] <- (backspace) clicked");
                {
                    let mut s = state.lock().unwrap();
                    if s.display.len() > 1 {
                        s.display.pop();
                    } else {
                        s.display = "0".to_string();
                    }
                }
                update_display(&disp, &state);
            })
            .unwrap();
        }
        buttons.push(bsp);

        let mut eq = button("row3.eq");
        {
            let (disp, state) = (disp.clone(), state.clone());
            eq.on_event("clicked", move |_| {
                println!("[calc] = (equals) clicked");
                {
                    let mut s = state.lock().unwrap();
                    let rhs: f64 = s.display.parse().unwrap_or(0.0);
                    let result = match s.pending_op {
                        '+' => s.operand + rhs,
                        '-' => s.operand - rhs,
                        '*' => s.operand * rhs,
                        '/' => {
                            if rhs != 0.0 {
                                s.operand / rhs
                            } else {
                                0.0
                            }
                        }
                        _ => rhs,
                    };
                    s.display = if result.fract() == 0.0 && result.abs() < 1e12 {
                        format!("{}", result as i64)
                    } else {
                        format!("{result}")
                    };
                    s.pending_op = '\0';
                    s.fresh = true;
                }
                update_display(&disp, &state);
            })
            .unwrap();
        }
        buttons.push(eq);

        let mut dot = button("row4.dot");
        {
            let (disp, state) = (disp.clone(), state.clone());
            dot.on_event("clicked", move |_| {
                println!("[calc] . (dot) clicked");
                {
                    let mut s = state.lock().unwrap();
                    if !s.display.contains('.') {
                        s.display.push('.');
                    }
                    s.fresh = false;
                }
                update_display(&disp, &state);
            })
            .unwrap();
        }
        buttons.push(dot);

        let mut pm = button("row4.pm");
        {
            let (disp, state) = (disp.clone(), state.clone());
            pm.on_event("clicked", move |_| {
                println!("[calc] +/- clicked");
                {
                    let mut s = state.lock().unwrap();
                    if !s.display.is_empty() && s.display != "0" {
                        if let Some(stripped) = s.display.strip_prefix('-') {
                            s.display = stripped.to_string();
                        } else {
                            s.display = format!("-{}", s.display);
                        }
                    }
                }
                update_display(&disp, &state);
            })
            .unwrap();
        }
        buttons.push(pm);

        let mut pct = button("row4.pct");
        {
            let (disp, state) = (disp.clone(), state.clone());
            pct.on_event("clicked", move |_| {
                println!("[calc] % clicked");
                {
                    let mut s = state.lock().unwrap();
                    let v: f64 = s.display.parse().unwrap_or(0.0) / 100.0;
                    s.display = format!("{v}");
                }
                update_display(&disp, &state);
            })
            .unwrap();
        }
        buttons.push(pct);

        println!("[calc] ready -- waiting for quit()");
        c.wait();
        println!("[calc] session ending");

        buttons.clear();
        drop(disp);
        c.release("calc").unwrap();
    });

    if let Err(e) = result {
        eprintln!("[Client] error: {e}");
        std::process::exit(1);
    }
    println!("[Client] done.");
}
