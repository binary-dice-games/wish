// MIT License © 2025 Binary Dice Games
/**
 * @file Program.cs
 * @brief Calculator example using the wish C# binding.
 *
 * Mirrors examples/calculator/main.cpp and
 * bindings/python/examples/calculator_example.py, minus the in-memory
 * server/renderer wiring: this program is a *client only*. Start a wish
 * server first (it owns the window/renderer), then point this program at
 * it -- matching whichever transport the server was started with:
 *
 *   build/app/wish server --transport=tcp --port=7070 --renderer=sdl3
 *   dotnet run --project bindings/csharp/examples/CalculatorExample -- --transport=tcp --host=127.0.0.1 --port=7070
 *
 * Run with:  dotnet run --project bindings/csharp/examples/CalculatorExample -- [--transport=tcp|pipe|term] [--host=HOST] [--port=PORT] [--name=PATH] [--theme=dark|light|classic]
 */

using Bdg.Bison;
using Bdg.Bison.Rmi;
using Bdg.Wish;

const string CalcDesc = """
{
  "type": "Window",
  "title": "Calculator",
  "width": 328,
  "height": 420,
  "closable": true,
  "children": {
    "display": { "type": "Label", "text": "0" },
    "sep":     { "type": "Separator" },
    "row0": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "c":   { "type": "Button", "label": "C",   "width": 72, "height": 52 },
        "div": { "type": "Button", "label": "/",   "width": 72, "height": 52 },
        "mul": { "type": "Button", "label": "*",   "width": 72, "height": 52 },
        "bsp": { "type": "Button", "label": "<-",  "width": 72, "height": 52 }
      }
    },
    "row1": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n7":  { "type": "Button", "label": "7", "width": 72, "height": 52 },
        "n8":  { "type": "Button", "label": "8", "width": 72, "height": 52 },
        "n9":  { "type": "Button", "label": "9", "width": 72, "height": 52 },
        "sub": { "type": "Button", "label": "-", "width": 72, "height": 52 }
      }
    },
    "row2": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n4":  { "type": "Button", "label": "4", "width": 72, "height": 52 },
        "n5":  { "type": "Button", "label": "5", "width": 72, "height": 52 },
        "n6":  { "type": "Button", "label": "6", "width": 72, "height": 52 },
        "add": { "type": "Button", "label": "+", "width": 72, "height": 52 }
      }
    },
    "row3": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n1": { "type": "Button", "label": "1", "width": 72, "height": 52 },
        "n2": { "type": "Button", "label": "2", "width": 72, "height": 52 },
        "n3": { "type": "Button", "label": "3", "width": 72, "height": 52 },
        "eq": { "type": "Button", "label": "=", "width": 72, "height": 52 }
      }
    },
    "row4": {
      "type": "HorizontalLayout",
      "spacing": 6,
      "children": {
        "n0":  { "type": "Button", "label": "0",   "width": 72, "height": 52 },
        "dot": { "type": "Button", "label": ".",   "width": 72, "height": 52 },
        "pm":  { "type": "Button", "label": "+/-", "width": 72, "height": 52 },
        "pct": { "type": "Button", "label": "%",   "width": 72, "height": 52 }
      }
    }
  }
}
""";

var options = CliOptions.Parse(args, defaultHost: "127.0.0.1", defaultPort: 7070);

Client client;
if (options.Transport == "tcp")
{
    Console.WriteLine($"[Client] connecting to {options.Host}:{options.Port} ...");
    client = Client.Tcp(options.Host, (ushort)options.Port);
}
else if (options.Transport == "pipe")
{
    Console.WriteLine($"[Client] connecting to pipe {options.Name} ...");
    client = Client.Pipe(options.Name);
}
else
{
    Console.WriteLine("[Client] connecting via inherited stdio (--transport=term) ...");
    client = Client.Term();
}

var calc = new Calculator(options.Verbose, options.Theme);

// client.Run() calls wish_client_run(), which invokes RunSession() on the
// library's own RMI worker thread and blocks *this* call until it returns.
// Run it on a background thread so the main thread can catch Ctrl+C and
// call client.Quit() to unblock RunSession()'s Wait() the same way clicking
// the window's close button (X) does.
Exception? threadError = null;
var worker = new Thread(() =>
{
    try
    {
        client.Run(calc.RunSession);
    }
    catch (Exception exc)
    {
        threadError = exc;
    }
});
worker.IsBackground = true;

Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    Console.WriteLine("\n[Client] Ctrl+C -- quitting ...");
    client.Quit();
};

worker.Start();
worker.Join();

if (threadError is not null)
{
    throw threadError;
}
Console.WriteLine("[Client] done.");

/// <summary>Port of calc_client from examples/calculator/main.cpp.</summary>
internal sealed class Calculator
{
    private readonly bool _verbose;
    private readonly string _theme;
    private string _display = "0";
    private double _operand;
    private string? _pendingOp;
    private bool _fresh = true;

    // Every proxy handed to OnEvent() must be kept alive for the whole
    // session: without a surviving reference the delegate trampoline it
    // holds could be collected before the next server-side click fires it.
    // This dict is the C# analogue of the proxy map (`pm`) the C++ example
    // keeps alive via instantiate_template().
    private readonly Dictionary<string, Proxy> _buttons = new();

    public Calculator(bool verbose, string theme)
    {
        _verbose = verbose;
        _theme = theme;
    }

    private void VLog(string msg)
    {
        if (_verbose)
        {
            Console.WriteLine($"[calc] {msg}");
        }
    }

    private void UpdateDisplay(Proxy disp)
    {
        VLog($"update_display -> \"{_display}\"");
        disp["text"] = _display;
    }

    private Proxy Button(Client client, string path)
    {
        var proxy = client.ProxyGet($"calc.{path}");
        _buttons[path] = proxy;
        return proxy;
    }

    public void RunSession(Client client)
    {
        VLog($"applying {_theme} theme");
        client.SetStylePreset(_theme);

        VLog("registering template 'calc'");
        client.RegisterTemplate("calc", CalcDesc);

        VLog("instantiating template 'calc'");
        var root = client.InstantiateTemplate("calc", "calc");

        root.OnEvent("closed", _ =>
        {
            VLog("window closed -- quitting");
            client.Quit();
        });

        var disp = client.ProxyGet("calc.display");

        Action<Dynamic> DigitHandler(string ch) => _ =>
        {
            VLog($"digit '{ch}' clicked");
            if (_fresh)
            {
                _display = ch;
                _fresh = false;
            }
            else
            {
                _display += ch;
            }
            UpdateDisplay(disp);
        };

        Action<Dynamic> OpHandler(string op) => _ =>
        {
            VLog($"op '{op}' clicked");
            _operand = double.Parse(_display);
            _pendingOp = op;
            _fresh = true;
            UpdateDisplay(disp);
        };

        VLog("registering button handlers");

        Button(client, "row0.c").OnEvent("clicked", _ =>
        {
            VLog("C (clear) clicked");
            _display = "0";
            _operand = 0.0;
            _pendingOp = null;
            _fresh = true;
            UpdateDisplay(disp);
        });
        Button(client, "row0.div").OnEvent("clicked", OpHandler("/"));
        Button(client, "row0.mul").OnEvent("clicked", OpHandler("*"));

        Button(client, "row0.bsp").OnEvent("clicked", _ =>
        {
            VLog("<- (backspace) clicked");
            _display = _display.Length > 1 ? _display[..^1] : "0";
            UpdateDisplay(disp);
        });

        Button(client, "row1.n7").OnEvent("clicked", DigitHandler("7"));
        Button(client, "row1.n8").OnEvent("clicked", DigitHandler("8"));
        Button(client, "row1.n9").OnEvent("clicked", DigitHandler("9"));
        Button(client, "row1.sub").OnEvent("clicked", OpHandler("-"));

        Button(client, "row2.n4").OnEvent("clicked", DigitHandler("4"));
        Button(client, "row2.n5").OnEvent("clicked", DigitHandler("5"));
        Button(client, "row2.n6").OnEvent("clicked", DigitHandler("6"));
        Button(client, "row2.add").OnEvent("clicked", OpHandler("+"));

        Button(client, "row3.n1").OnEvent("clicked", DigitHandler("1"));
        Button(client, "row3.n2").OnEvent("clicked", DigitHandler("2"));
        Button(client, "row3.n3").OnEvent("clicked", DigitHandler("3"));

        Button(client, "row3.eq").OnEvent("clicked", _ =>
        {
            VLog("= (equals) clicked");
            var rhs = double.Parse(_display);
            var result = _pendingOp switch
            {
                "+" => _operand + rhs,
                "-" => _operand - rhs,
                "*" => _operand * rhs,
                "/" => rhs != 0.0 ? _operand / rhs : 0.0,
                _ => rhs,
            };

            _display = result == Math.Floor(result) && Math.Abs(result) < 1e12
                ? ((long)result).ToString()
                : result.ToString();
            _pendingOp = null;
            _fresh = true;
            VLog($"result: \"{_display}\"");
            UpdateDisplay(disp);
        });

        Button(client, "row4.n0").OnEvent("clicked", DigitHandler("0"));

        Button(client, "row4.dot").OnEvent("clicked", _ =>
        {
            VLog(". (dot) clicked");
            if (!_display.Contains('.'))
            {
                _display += ".";
            }
            _fresh = false;
            UpdateDisplay(disp);
        });

        Button(client, "row4.pm").OnEvent("clicked", _ =>
        {
            VLog("+/- clicked");
            if (_display.Length > 0 && _display != "0")
            {
                _display = _display[0] == '-' ? _display[1..] : "-" + _display;
            }
            UpdateDisplay(disp);
        });

        Button(client, "row4.pct").OnEvent("clicked", _ =>
        {
            VLog("% clicked");
            _display = (double.Parse(_display) / 100.0).ToString();
            UpdateDisplay(disp);
        });

        VLog("ready -- waiting for quit()");
        client.Wait();
        VLog("session ending");

        client.Release("calc");
        root.Release();
        disp.Release();
        foreach (var proxy in _buttons.Values)
        {
            proxy.Release();
        }
        _buttons.Clear();
    }
}

/// <summary>--transport/--host/--port/--name/--theme/--verbose flag parsing.</summary>
internal sealed record CliOptions(string Transport, string Host, int Port, string Name, string Theme, bool Verbose)
{
    public static CliOptions Parse(string[] args, string defaultHost, int defaultPort)
    {
        var transport = "tcp";
        var host = defaultHost;
        var port = defaultPort;
        var name = "";
        var theme = "dark";
        var verbose = false;

        foreach (var arg in args)
        {
            if (arg.StartsWith("--transport=")) transport = arg["--transport=".Length..];
            else if (arg.StartsWith("--host=")) host = arg["--host=".Length..];
            else if (arg.StartsWith("--port=")) port = int.Parse(arg["--port=".Length..]);
            else if (arg.StartsWith("--name=")) name = arg["--name=".Length..];
            else if (arg.StartsWith("--theme=")) theme = arg["--theme=".Length..];
            else if (arg == "--verbose") verbose = true;
        }
        return new CliOptions(transport, host, port, name, theme, verbose);
    }
}
