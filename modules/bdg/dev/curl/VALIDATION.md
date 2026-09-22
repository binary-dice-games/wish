# Curl Module — Validation Log

Reproducible checks for the `curl` module's request/response pipeline —
the same examples run live against real endpoints while fixing four bugs
in how responses were parsed and displayed. Work through them in the
running app to confirm the fixes on your own machine; each `curl` command
alongside a check is the equivalent request run in a terminal, for
cross-checking outside the app.

**Launch:** `wish client --run=curl` or `wish standalone --run=curl`

See [DESIGN.md](DESIGN.md) §6 ("Design Decisions") and §10
("Implementation Status") for the full technical writeup, and
[PLAN.md](PLAN.md)'s Verification checklist for what was and wasn't
re-driven live.

## What was found and fixed

Each item below maps directly to one or more checks further down this
document — re-running that check is what confirms the fix held.

| | Symptom | Cause | Found against |
|---|---|---|---|
| 1 | **Body dropped.** Trailing-newline bodies came back empty. | A JSON body ending in its own newline collided with the parser's own trailer marker, so the Body pane showed nothing at all. | `httpbin.org/get` |
| 2 | **Body truncated.** Large HTML bodies were cut and misread as headers. | A blank line occurring naturally inside a big page's HTML/JS was mistaken for a second response, chopping the real body short. | `www.google.com` |
| 3 | **HEAD failed.** HEAD requests reported "failed" despite succeeding. | `curl` expected body bytes that a HEAD response correctly never sends, so a working request showed as an error. | `httpbin.org/anything` |
| 4 | **Stale body.** The Body pane kept showing the very first response. | Every response reused the same filename, so later requests updated the status line and headers but never the body text. | cycling all 7 methods in one session |

## A — All seven HTTP methods

Same URL for every row on purpose — `httpbin.org/anything` accepts any
method and echoes it back, so switching only the Method dropdown isolates
bug 4 above: watch the Body pane actually change each time, not just the
status line.

- [ ] **GET**
  1. Method: `GET`, URL: `https://httpbin.org/anything`
  2. Click `Send`
  - Expect: `200` · 342 B · body shows `"method": "GET"`
  - ```sh
    curl -sS -i -X GET https://httpbin.org/anything
    ```

- [ ] **POST**
  1. Method: `POST`, URL: `https://httpbin.org/anything`
  2. Click `Send`
  - Expect: `200` · 343 B · body shows `"method": "POST"`
  - ```sh
    curl -sS -i -X POST https://httpbin.org/anything
    ```

- [ ] **PUT**
  1. Method: `PUT`, URL: `https://httpbin.org/anything`
  2. Click `Send`
  - Expect: `200` · 342 B · body shows `"method": "PUT"`
  - ```sh
    curl -sS -i -X PUT https://httpbin.org/anything
    ```

- [ ] **PATCH**
  1. Method: `PATCH`, URL: `https://httpbin.org/anything`
  2. Click `Send`
  - Expect: `200` · 344 B · body shows `"method": "PATCH"`
  - ```sh
    curl -sS -i -X PATCH https://httpbin.org/anything
    ```

- [ ] **DELETE**
  1. Method: `DELETE`, URL: `https://httpbin.org/anything`
  2. Click `Send`
  - Expect: `200` · 345 B · body shows `"method": "DELETE"`
  - ```sh
    curl -sS -i -X DELETE https://httpbin.org/anything
    ```

- [ ] **HEAD**
  1. Method: `HEAD`, URL: `https://httpbin.org/anything`
  2. Click `Send`
  - Expect: `200` · 0 B · body empty — correct, no body should render (this is bug 3 above)
  - ```sh
    curl -sS -i --head -X HEAD https://httpbin.org/anything
    ```

- [ ] **OPTIONS**
  1. Method: `OPTIONS`, URL: `https://httpbin.org/anything`
  2. Click `Send`
  - Expect: `200` · 0 B · body empty — correct, no body should render
  - ```sh
    curl -sS -i -X OPTIONS https://httpbin.org/anything
    ```

## B — Large and tricky response bodies

- [ ] **Large HTML page with an embedded blank line**
  1. Method: `GET`, URL: `https://www.google.com`
  2. Click `Send`
  - Expect: `200` · full HTML body renders in the Body tab, starting with `<!doctype html>` — not cut off partway through (bug 2 above)
  - ```sh
    curl -sS -i -L -X GET https://www.google.com
    ```

- [ ] **JSON body ending in its own trailing newline**
  1. Method: `GET`, URL: `https://httpbin.org/get?probe=1`
  2. Click `Send`
  - Expect: `200` · Body tab shows the full JSON, pretty-printed and syntax highlighted — not empty (bug 1 above)
  - ```sh
    curl -sS -i -X GET 'https://httpbin.org/get?probe=1'
    ```

- [ ] **Non-JSON content type (XML)**
  1. Method: `GET`, URL: `https://httpbin.org/xml`
  2. Click `Send`
  - Expect: `200` · XML text renders in the Body tab (plain text, no JSON highlighting)
  - ```sh
    curl -sS -i -X GET https://httpbin.org/xml
    ```

## C — Redirects and error status colors

- [ ] **Two-hop redirect chain**
  1. Method: `GET`, URL: `https://httpbin.org/redirect/2`
  2. Confirm `Follow redirects` is checked
  3. Click `Send`
  - Expect: `200` · final response only — body's `"url"` ends in `/get`, not an intermediate redirect hop
  - ```sh
    curl -sS -i -L -X GET https://httpbin.org/redirect/2
    ```

- [ ] **Client error status (404)**
  1. Method: `GET`, URL: `https://httpbin.org/status/404`
  2. Click `Send`
  - Expect: `404` · status line renders in red, body correctly empty
  - ```sh
    curl -sS -i -X GET https://httpbin.org/status/404
    ```

- [ ] **Server error status (500)**
  1. Method: `GET`, URL: `https://httpbin.org/status/500`
  2. Click `Send`
  - Expect: `500` · status line renders in red, body correctly empty
  - ```sh
    curl -sS -i -X GET https://httpbin.org/status/500
    ```

## D — Query params, headers, and a JSON body

httpbin echoes back exactly what it received, so anything added on the
Params/Headers/Body tabs should reappear verbatim in the response.

- [ ] **Query parameter round-trip**
  1. Method: `GET`, URL: `https://httpbin.org/get`
  2. On the `Params` tab, click `+ Add Param`
  3. Key: `search`, Value: `curl module`
  4. Click `Send`
  - Expect: `200` · body's `"args"` shows `{"search": "curl module"}`
  - ```sh
    curl -sS -i -G -X GET --data-urlencode 'search=curl module' https://httpbin.org/get
    ```

- [ ] **Custom header round-trip**
  1. Method: `GET`, URL: `https://httpbin.org/headers`
  2. On the `Headers` tab, click `+ Add Header`
  3. Key: `X-Wish-Test`, Value: `hello`
  4. Click `Send`
  - Expect: `200` · body's `"headers"` includes `"X-Wish-Test": "hello"`
  - ```sh
    curl -sS -i -X GET -H 'X-Wish-Test: hello' https://httpbin.org/headers
    ```

- [ ] **JSON request body on a POST**
  1. Method: `POST`, URL: `https://httpbin.org/post`
  2. On the `Body` tab, set mode to `JSON`
  3. Body text: `{"name": "Ada", "year": 1815}`
  4. Click `Send`
  - Expect: `200` · body's `"json"` field echoes `{"name": "Ada", "year": 1815}`
  - ```sh
    curl -sS -i -X POST --data-raw '{"name": "Ada", "year": 1815}' -H 'Content-Type: application/json' https://httpbin.org/post
    ```

## E — Basic and Bearer auth

- [ ] **Basic auth with matching credentials**
  1. Method: `GET`, URL: `https://httpbin.org/basic-auth/wishuser/wishpass`
  2. On the `Auth` tab, set mode to `Basic Auth`
  3. Username: `wishuser`, Password: `wishpass`
  4. Click `Send`
  - Expect: `200` · body shows `{"authenticated": true, "user": "wishuser"}`
  - ```sh
    curl -sS -i -u wishuser:wishpass https://httpbin.org/basic-auth/wishuser/wishpass
    ```

- [ ] **Bearer token**
  1. Method: `GET`, URL: `https://httpbin.org/bearer`
  2. On the `Auth` tab, set mode to `Bearer Token`
  3. Token: `wish-demo-token`
  4. Click `Send`
  - Expect: `200` · body shows `{"authenticated": true, "token": "wish-demo-token"}`
  - ```sh
    curl -sS -i -H 'Authorization: Bearer wish-demo-token' https://httpbin.org/bearer
    ```

## F — Bonus: Collections, History, Environments

These are covered by the module's own automated test suite
(`tests/test_curl.cpp`) and are known to work the same way through the UI,
but weren't re-driven against a live endpoint in this validation pass —
worth a manual look if you have a minute.

- [ ] **Save a request, then reload it from Collections**
  1. Build any request (e.g. the JSON POST above)
  2. Under `Save as`, name it and give it a collection, click `Save`
  3. Switch to the `Collections` tab, find the row, open `...` → `Load`
  - Expect: every field — method, URL, params, headers, body, auth — reappears exactly as saved

- [ ] **Reload a previous request from History**
  1. Send a couple of different requests
  2. Switch to the `History` tab
  3. Open `...` → `Load` on an older row
  - Expect: the builder resets to that request's exact state

- [ ] **Environment variable substitution**
  1. On the `Environments` tab, create one named `Local`
  2. Add a variable: `base_url` → `https://httpbin.org`
  3. Back in Request, pick `Local` from the environment dropdown
  4. URL: `{{base_url}}/get`, click `Send`
  - Expect: request succeeds against the resolved URL — the Console trace shows the real address, not the literal `{{base_url}}`

- [ ] **Delete a saved request**
  1. On `Collections`, open `...` → `Delete` on any saved request
  - Expect: a confirm dialog appears; confirming removes the row
