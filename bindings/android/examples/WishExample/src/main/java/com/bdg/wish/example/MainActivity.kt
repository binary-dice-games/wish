// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish.example

import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.bdg.wish.Client
import com.bdg.wish.Dynamic
import com.bdg.wish.Proxy
import kotlin.concurrent.thread

/**
 * Exercises the `com.bdg.wish` Android binding end to end against a real
 * wish server: connect over TCP, register and instantiate a small UI
 * template, and drive it with button clicks in both directions -- the
 * server's own window (rendered on whatever machine `wish server` is
 * running on -- a wish client never renders UI itself, see docs/bindings.md)
 * and this app's own controls both update the same remote counter, with
 * [Proxy.onEvent] carrying the server-side clicks back here. See
 * docs/examples.md for how to start a matching server and read this app as
 * a validation step, not just a demo.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var hostInput: EditText
    private lateinit var portInput: EditText
    private lateinit var connectButton: Button
    private lateinit var statusText: TextView
    private lateinit var countText: TextView
    private lateinit var incrementButton: Button
    private lateinit var resetButton: Button

    private var client: Client? = null
    private var sessionThread: Thread? = null
    private var count = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        hostInput = findViewById(R.id.hostInput)
        portInput = findViewById(R.id.portInput)
        connectButton = findViewById(R.id.connectButton)
        statusText = findViewById(R.id.statusText)
        countText = findViewById(R.id.countText)
        incrementButton = findViewById(R.id.incrementButton)
        resetButton = findViewById(R.id.resetButton)

        // 10.0.2.2 is the Android emulator's alias for the host machine's
        // own localhost -- the common case for "wish server" running on the
        // same development machine as the emulator.
        hostInput.setText("10.0.2.2")
        portInput.setText("7070")

        connectButton.setOnClickListener { toggleConnection() }
        incrementButton.setOnClickListener { changeCount(1) }
        resetButton.setOnClickListener { changeCount(0, reset = true) }
        setControlsEnabled(false)
    }

    private fun toggleConnection() {
        if (client != null) {
            disconnect()
        } else {
            connect()
        }
    }

    private fun connect() {
        val host = hostInput.text.toString()
        val port = portInput.text.toString().toIntOrNull() ?: 7070
        setStatus("Connecting to $host:$port ...")
        connectButton.isEnabled = false

        val c = Client.tcp(host, port)
        client = c
        sessionThread = thread {
            try {
                // Bound method reference, not a trailing lambda: `c.run { ... }`
                // would collide with Kotlin's stdlib `T.run { }` scope
                // function of the same name.
                c.run(this::runSession)
            } catch (e: Exception) {
                runOnUiThread { setStatus("Disconnected: ${e.message}") }
            } finally {
                // Only safe once run() has fully returned -- wish_client_destroy()
                // must not be called while a session is active (see Client.close()'s
                // doc comment).
                c.close()
                runOnUiThread {
                    client = null
                    setControlsEnabled(false)
                    connectButton.text = "Connect"
                    connectButton.isEnabled = true
                }
            }
        }
    }

    private fun disconnect() {
        client?.quit()
    }

    // Runs on the library's internal RMI worker thread (see SessionCallback's
    // doc comment) -- never touch Android views directly here, only via
    // runOnUiThread.
    private fun runSession(session: Client) {
        session.setStylePreset("dark")
        session.registerTemplate("counter", COUNTER_TEMPLATE)
        val root = session.instantiateTemplate("counter", "counter")
        val display = session.proxyGet("counter.display")
        val incBtn = session.proxyGet("counter.row.inc")
        val resetBtn = session.proxyGet("counter.row.reset")

        root.onEvent("closed") { session.quit() }
        incBtn.onEvent("clicked") { onRemoteClick(display, 1) }
        resetBtn.onEvent("clicked") { onRemoteClick(display, 0, reset = true) }

        runOnUiThread {
            setStatus("Connected")
            setControlsEnabled(true)
            connectButton.text = "Disconnect"
            connectButton.isEnabled = true
            updateDisplay(display)
        }

        session.waitForQuit()

        incBtn.close()
        resetBtn.close()
        display.close()
        root.close()
    }

    // Fired from the server's own window when its "+1"/"Reset" button is
    // clicked -- mirror the change into this app's own TextView.
    private fun onRemoteClick(display: Proxy, delta: Int, reset: Boolean = false) {
        count = if (reset) 0 else count + delta
        updateDisplay(display)
        runOnUiThread { countText.text = count.toString() }
    }

    // Fired from this app's own "+1"/"Reset" buttons -- mirror the change
    // into the server's remote Label.
    private fun changeCount(delta: Int, reset: Boolean = false) {
        val c = client ?: return
        thread {
            try {
                c.proxyGet("counter.display").use { display ->
                    count = if (reset) 0 else count + delta
                    updateDisplay(display)
                }
            } catch (e: Exception) {
                runOnUiThread { setStatus("Error: ${e.message}") }
            }
            runOnUiThread { countText.text = count.toString() }
        }
    }

    private fun updateDisplay(display: Proxy) {
        Dynamic().use { fields ->
            fields.setString("text", count.toString())
            display.set(fields)
        }
    }

    private fun setStatus(text: String) {
        statusText.text = text
    }

    private fun setControlsEnabled(enabled: Boolean) {
        incrementButton.isEnabled = enabled
        resetButton.isEnabled = enabled
    }

    override fun onDestroy() {
        super.onDestroy()
        client?.quit()
        sessionThread?.join(2000)
    }

    companion object {
        private const val COUNTER_TEMPLATE = """
        {
          "type": "Window",
          "title": "Wish Android Counter",
          "width": 300,
          "height": 160,
          "closable": true,
          "children": {
            "display": { "type": "Label", "text": "0" },
            "row": {
              "type": "HorizontalLayout",
              "spacing": 8,
              "children": {
                "inc":   { "type": "Button", "label": "+1",    "width": 80, "height": 40 },
                "reset": { "type": "Button", "label": "Reset", "width": 80, "height": 40 }
              }
            }
          }
        }
        """
    }
}
