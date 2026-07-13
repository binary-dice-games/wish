# calculator

Four-function calculator; demonstrates self-contained server-side form
logic — all arithmetic runs server-side, the client only instantiates the
form and listens for the `"closed"` event.

- **server/**: `Calculator` form (`register_calculator()`), a
  `bdg::wish::form` subclass owning the window/button-grid/display and all
  arithmetic.
- **client/**: `run_calculator(wish_app_host&)`, self-registered as the
  `"calculator"` embedded app — instantiates the form, keeps it alive until
  the window closes.
- **resources/**: none.
