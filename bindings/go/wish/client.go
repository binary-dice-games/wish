// MIT License © 2025 Binary Dice Games

// Package wish: this file is the safe, idiomatic Go wrapper around
// wish_client_c.h -- the Go analogue of
// bindings/cpp/include/wish_cpp/client.hpp, covering the full
// wish_client_c.h surface: client lifecycle for all 5 transports, style
// presets, template register/instantiate/proxy_get/release, direct object
// instantiation, embedded-app list/run, file transfer, logging, and native
// automation.
package wish

/*
#include <stdlib.h>
#include "wish_client_c.h"
#include "shim.h"

// Forward declaration for the //export'd trampoline defined in native.go
// (see shim.c/shim.h for wish_client_run_shim/wish_client_run_with_params_shim
// themselves).
extern void goSessionTrampoline(wish_client_handle client, void* userdata);
extern void goResourceSyncTrampoline(wish_client_handle client, char* name, int state,
    uint64_t transferred, uint64_t total, char* errMsg, void* userdata);
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"runtime"
	"runtime/cgo"
	"time"
	"unsafe"
)

// ─── Errors ─────────────────────────────────────────────────────────────────

// wish_error codes (see wish_client_c.h), exported as typed constants so
// callers can compare a *WishError's Code field directly.
const (
	WishErrNull      int32 = -1
	WishErrNotFound  int32 = -2
	WishErrTransport int32 = -3
	WishErrException int32 = -4
	WishErrAmbiguous int32 = -5
	// WishErrUnavailable: the feature isn't available for this session
	// (e.g. the user store of an anonymous session).
	WishErrUnavailable int32 = -6
)

var wishErrorMessages = map[int32]string{
	WishErrNull:        "null handle or pointer",
	WishErrNotFound:    "named proxy or resource not found",
	WishErrTransport:   "transport connection failed",
	WishErrException:   "internal C++ exception",
	WishErrAmbiguous:   "app name matches more than one registered app; use the fully-qualified name",
	WishErrUnavailable: "not available for this session (e.g. the user store of an anonymous session)",
}

func wishErrorMessage(code int32) string {
	if msg, ok := wishErrorMessages[code]; ok {
		return msg
	}
	return fmt.Sprintf("unknown error %d", code)
}

// WishError is returned when a wish_* C API call reports a non-zero error
// code. Code is the raw wish_error value (see wish_client_c.h); Detail (if
// non-empty) carries the client's last_error() message, which is often more
// specific than the generic per-code message.
type WishError struct {
	Code    int32
	Detail  string
	message string
}

func (e *WishError) Error() string { return e.message }

func newWishError(code int32, context string, client C.wish_client_handle) *WishError {
	msg := context + ": " + wishErrorMessage(code)
	detail := ""
	if client != nil {
		detail = lastErrorString(client)
		if detail != "" {
			msg += " (" + detail + ")"
		}
	}
	return &WishError{Code: code, Detail: detail, message: msg}
}

func checkWish(rc C.wish_error, context string, client C.wish_client_handle) error {
	if rc == C.WISH_OK {
		return nil
	}
	return newWishError(int32(rc), context, client)
}

func lastErrorString(client C.wish_client_handle) string {
	p := C.wish_last_error(client)
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

// ListAppsJSON lists every embedded app registered by an enabled optional
// module (see modules/README.md), as a raw JSON array string --
// [{"name","organization","collection","description","params":[...]}, ...].
//
// Mirrors `wish client --list`. Does not require a connection -- app
// registration happens at library-load time.
func ListAppsJSON() (string, error) {
	var out *C.char
	rc := C.wish_list_apps(&out)
	if err := checkWish(rc, "list_apps_json", nil); err != nil {
		return "", err
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// ─── Client ─────────────────────────────────────────────────────────────────

// Client wraps a wish_client_handle. Construct via NewTCPClient,
// NewTLSClient, NewStreamClient, NewPipeClient, or NewTermClient, then call
// Run to connect, drive the session, and disconnect.
type Client struct {
	handle C.wish_client_handle
}

func newClient(h C.wish_client_handle) *Client {
	c := &Client{handle: h}
	runtime.SetFinalizer(c, (*Client).finalize)
	return c
}

func (c *Client) finalize() { c.Destroy() }

func cCharOrNil(s string) *C.char {
	if s == "" {
		return nil
	}
	return C.CString(s)
}

// NewTCPClient creates a TCP socket client. Not connected until Run is
// called.
func NewTCPClient(host string, port uint16) (*Client, error) {
	cHost := C.CString(host)
	defer C.free(unsafe.Pointer(cHost))
	h := C.wish_client_tcp_create(cHost, C.uint16_t(port))
	if h == nil {
		return nil, fmt.Errorf("wish: wish_client_tcp_create failed")
	}
	return newClient(h), nil
}

// NewTLSClient creates a TLS-secured TCP client (not yet connected). TLS
// trust/identity material is supplied via Run/RunWithParams's connectParams
// (ca_file/ca_pem, insecure_skip_verify, cert_file/cert_pem, key_file/
// key_pem, key_password, server_name).
func NewTLSClient(host string, port uint16) (*Client, error) {
	cHost := C.CString(host)
	defer C.free(unsafe.Pointer(cHost))
	h := C.wish_client_tls_create(cHost, C.uint16_t(port))
	if h == nil {
		return nil, fmt.Errorf("wish: wish_client_tls_create failed")
	}
	return newClient(h), nil
}

// NewStreamClient creates a std::iostream-backed stream (FIFO / named pipe)
// client. Linux only.
func NewStreamClient(path string) (*Client, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	h := C.wish_client_stream_create(cPath)
	if h == nil {
		return nil, fmt.Errorf("wish: wish_client_stream_create failed")
	}
	return newClient(h), nil
}

// NewPipeClient creates a named-pipe / Unix-socket client.
func NewPipeClient(path string) (*Client, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	h := C.wish_client_pipe_create(cPath)
	if h == nil {
		return nil, fmt.Errorf("wish: wish_client_pipe_create failed")
	}
	return newClient(h), nil
}

// NewDesktopClient creates a client for the local wish desktop's fixed
// endpoint (`\\.\pipe\wish-desktop` on Windows, the abstract socket
// `@wish-desktop` on Linux, or WISH_DESKTOP_ENDPOINT when set). Use with
// Client.RunOnDesktop.
func NewDesktopClient() (*Client, error) {
	h := C.wish_client_desktop_create()
	if h == nil {
		return nil, fmt.Errorf("wish: wish_client_desktop_create failed")
	}
	return newClient(h), nil
}

// NewTermClient creates a terminal (OSC-99 framed) client wrapping the
// calling process's own inherited stdio.
func NewTermClient() (*Client, error) {
	h := C.wish_client_term_create()
	if h == nil {
		return nil, fmt.Errorf("wish: wish_client_term_create failed")
	}
	return newClient(h), nil
}

// Destroy releases the client handle. Must not be called while a Run/
// RunWithParams session is active. Safe to call multiple times.
func (c *Client) Destroy() {
	if c.handle != nil {
		C.wish_client_destroy(c.handle)
		c.handle = nil
	}
}

// LastError returns the last error message recorded for this client (empty
// if none).
func (c *Client) LastError() string {
	if c.handle == nil {
		return ""
	}
	return lastErrorString(c.handle)
}

// ── Session lifecycle ───────────────────────────────────────────────────

// sessionCtx carries the Go callback (and any panic it raised) across the
// cgo boundary to native.go's goSessionTrampoline; see that function's doc
// comment for the panic-recovery convention.
type sessionCtx struct {
	fn         func(*Client)
	panicValue interface{}
	// Set by RunOnDesktop: a panic also ends the reconnect loop.
	stopDesktopOnPanic bool
}

// Run connects, invokes fn(c), then disconnects. Blocks until fn returns;
// it runs on the RMI worker thread, so call Client.Wait inside it to keep
// the session alive while event handlers update the UI, ending it with
// Client.Quit.
func (c *Client) Run(fn func(*Client)) error {
	return c.RunWithParams(fn, nil)
}

// RunWithParams is identical to Run, except connectParams is forwarded to
// both the transport's connection setup and the server's connect handshake
// payload (e.g. fields a server-side auth module inspects).
func (c *Client) RunWithParams(fn func(*Client), connectParams *Value) error {
	ctx := &sessionCtx{fn: fn}
	h := cgo.NewHandle(ctx)
	defer h.Delete()

	rc := C.wish_client_run_with_params_shim(
		c.handle,
		C.wish_session_fn(C.goSessionTrampoline),
		C.uintptr_t(h),
		paramsHandle(connectParams),
	)
	if ctx.panicValue != nil {
		panic(ctx.panicValue)
	}
	return checkWish(rc, "client.run", c.handle)
}

// RunOnDesktop runs fn(c) on the desktop whenever one is running, for
// long-running processes (services). While no desktop runs, the connection
// is retried every retry; when the desktop closes, it waits for the next
// one, so the UI comes back when the desktop is started again. Returns
// after StopDesktop, or when fn returns while still connected (e.g. after
// Quit). A panic in fn ends the loop and is re-raised here.
func (c *Client) RunOnDesktop(fn func(*Client), connectParams *Value, retry time.Duration) error {
	ctx := &sessionCtx{fn: fn, stopDesktopOnPanic: true}
	h := cgo.NewHandle(ctx)
	defer h.Delete()

	retryMs := retry.Milliseconds()
	if retryMs < 1 {
		retryMs = 1
	}
	rc := C.wish_client_run_on_desktop_shim(
		c.handle,
		C.wish_session_fn(C.goSessionTrampoline),
		C.uintptr_t(h),
		paramsHandle(connectParams),
		C.uint32_t(retryMs),
	)
	if ctx.panicValue != nil {
		panic(ctx.panicValue)
	}
	return checkWish(rc, "client.run_on_desktop", c.handle)
}

// StopDesktop makes a running (or the next) RunOnDesktop return. Safe to
// call from any goroutine.
func (c *Client) StopDesktop() {
	C.wish_client_desktop_stop(c.handle)
}

// Wait blocks until Quit is called (from any thread).
func (c *Client) Wait() {
	C.wish_client_wait(c.handle)
}

// Quit signals the session to end; unblocks a concurrent Wait. Safe to call
// from any thread, including an event handler.
func (c *Client) Quit() {
	C.wish_client_quit(c.handle)
}

// ── Style ───────────────────────────────────────────────────────────────

// SetStylePreset applies a built-in style preset: "wish", "dark", "light",
// or "classic".
func (c *Client) SetStylePreset(preset string) error {
	cPreset := C.CString(preset)
	defer C.free(unsafe.Pointer(cPreset))
	rc := C.wish_set_style_preset(c.handle, cPreset)
	return checkWish(rc, "client.set_style_preset", c.handle)
}

// SetStylePresetAsync is the asynchronous counterpart to SetStylePreset.
func (c *Client) SetStylePresetAsync(preset string) (*Future, error) {
	cPreset := C.CString(preset)
	defer C.free(unsafe.Pointer(cPreset))
	var f C.rmi_future_handle
	rc := C.wish_set_style_preset_async(c.handle, cPreset, &f)
	if err := checkWish(rc, "client.set_style_preset_async", c.handle); err != nil {
		return nil, err
	}
	return newFuture(f), nil
}

// ── Internationalization ─────────────────────────────────────────────────

// SetTranslations sets the translations applied by RegisterTemplate:
// template string values "$$KEY" become KEY's translation. Both texts use the
// translation file format ("KEY = value" per line); pass an empty fallback
// for none. A key found in neither shows as the bare KEY.
func (c *Client) SetTranslations(text, fallback string) error {
	cText := C.CString(text)
	defer C.free(unsafe.Pointer(cText))
	var cFallback *C.char
	if fallback != "" {
		cFallback = C.CString(fallback)
		defer C.free(unsafe.Pointer(cFallback))
	}
	rc := C.wish_set_translations(c.handle, cText, cFallback)
	return checkWish(rc, "client.set_translations", c.handle)
}

// Translate translates "$$KEY"; any other string is returned unchanged.
func (c *Client) Translate(text string) (string, error) {
	cText := C.CString(text)
	defer C.free(unsafe.Pointer(cText))
	var out *C.char
	rc := C.wish_translate(c.handle, cText, &out)
	if err := checkWish(rc, "client.translate", c.handle); err != nil {
		return "", err
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// SetLanguage sets this session's UI language on the server (e.g. "es"),
// used by server-side tools.
func (c *Client) SetLanguage(lang string) error {
	cLang := C.CString(lang)
	defer C.free(unsafe.Pointer(cLang))
	rc := C.wish_set_language(c.handle, cLang)
	return checkWish(rc, "client.set_language", c.handle)
}

// ── Template management ──────────────────────────────────────────────────

// RegisterTemplate registers a named UI template (JSON or YAML descriptor
// text).
func (c *Client) RegisterTemplate(name, descriptor string) error {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	cDesc := C.CString(descriptor)
	defer C.free(unsafe.Pointer(cDesc))
	rc := C.wish_register_template(c.handle, cName, cDesc)
	return checkWish(rc, fmt.Sprintf("client.register_template(%q)", name), c.handle)
}

// RegisterTemplateAsync is the asynchronous counterpart to
// RegisterTemplate.
func (c *Client) RegisterTemplateAsync(name, descriptor string) (*Future, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	cDesc := C.CString(descriptor)
	defer C.free(unsafe.Pointer(cDesc))
	var f C.rmi_future_handle
	rc := C.wish_register_template_async(c.handle, cName, cDesc, &f)
	if err := checkWish(rc, fmt.Sprintf("client.register_template_async(%q)", name), c.handle); err != nil {
		return nil, err
	}
	return newFuture(f), nil
}

// InstantiateTemplate instantiates a registered template under dot-path
// prefix and returns a proxy to its root; descendants are reachable via
// ProxyGet("prefix.child.path").
func (c *Client) InstantiateTemplate(name, prefix string) (*Proxy, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	cPrefix := C.CString(prefix)
	defer C.free(unsafe.Pointer(cPrefix))
	h := C.wish_instantiate_template(c.handle, cName, cPrefix)
	if h == nil {
		return nil, newWishError(WishErrException, fmt.Sprintf("client.instantiate_template(%q, %q)", name, prefix), c.handle)
	}
	return newProxy(h), nil
}

// InstantiateTemplateAsync is the asynchronous counterpart to
// InstantiateTemplate; consume the returned Future with Future.GetProxy.
func (c *Client) InstantiateTemplateAsync(name, prefix string) (*Future, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	cPrefix := C.CString(prefix)
	defer C.free(unsafe.Pointer(cPrefix))
	var f C.rmi_future_handle
	rc := C.wish_instantiate_template_async(c.handle, cName, cPrefix, &f)
	if err := checkWish(rc, fmt.Sprintf("client.instantiate_template_async(%q, %q)", name, prefix), c.handle); err != nil {
		return nil, err
	}
	return newFuture(f), nil
}

// ProxyGet resolves a dot-joined element path (see InstantiateTemplate)
// from the client's local proxy map -- no round trip to the server.
func (c *Client) ProxyGet(dotPath string) (*Proxy, error) {
	cPath := C.CString(dotPath)
	defer C.free(unsafe.Pointer(cPath))
	h := C.wish_proxy_get(c.handle, cPath)
	if h == nil {
		return nil, newWishError(WishErrNotFound, fmt.Sprintf("client.proxy_get(%q)", dotPath), c.handle)
	}
	return newProxy(h), nil
}

// Release releases every proxy cached under prefix and its descendants.
func (c *Client) Release(prefix string) error {
	cPrefix := C.CString(prefix)
	defer C.free(unsafe.Pointer(cPrefix))
	rc := C.wish_release(c.handle, cPrefix)
	return checkWish(rc, fmt.Sprintf("client.release(%q)", prefix), c.handle)
}

// ── Object instantiation ─────────────────────────────────────────────────

// Instantiate instantiates a remote object directly (no UI template
// involved). Unlike InstantiateTemplate, the result is not merged into the
// dot-path proxy map; the caller keeps and closes the returned proxy
// directly. ns is "" for the global namespace.
func (c *Client) Instantiate(klass, ns string, params *Value) (*Proxy, error) {
	h := C.wish_instantiate(c.handle, cKeyOrZero(ns), cKey(klass), paramsHandle(params))
	if h == nil {
		return nil, newWishError(WishErrException, fmt.Sprintf("client.instantiate(%q)", klass), c.handle)
	}
	return newProxy(h), nil
}

// ── Embedded apps ────────────────────────────────────────────────────────

// RunApp connects, runs the named embedded app (see ListAppsJSON), blocks
// until it signals completion, then disconnects. name may be a short name
// (e.g. "bc") or its fully-qualified "organization/collection/name" form.
func (c *Client) RunApp(name string, args []string) error {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	cArgs := make([]*C.char, len(args))
	for i, a := range args {
		cArgs[i] = C.CString(a)
	}
	defer func() {
		for _, p := range cArgs {
			C.free(unsafe.Pointer(p))
		}
	}()

	var argvPtr **C.char
	if len(cArgs) > 0 {
		argvPtr = &cArgs[0]
	}
	rc := C.wish_run_app(c.handle, cName, argvPtr, C.size_t(len(cArgs)))
	return checkWish(rc, fmt.Sprintf("client.run_app(%q)", name), c.handle)
}

// ── File transfer ────────────────────────────────────────────────────────

// UploadFile uploads a file to the server's sandboxed session resource
// directory.
func (c *Client) UploadFile(name string, data []byte) error {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	var dataPtr *C.char
	if len(data) > 0 {
		dataPtr = (*C.char)(unsafe.Pointer(&data[0]))
	}
	rc := C.wish_upload_file(c.handle, cName, dataPtr, C.size_t(len(data)))
	return checkWish(rc, fmt.Sprintf("client.upload_file(%q)", name), c.handle)
}

// DownloadFile downloads a previously uploaded file from the server.
func (c *Client) DownloadFile(name string) ([]byte, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	var out *C.char
	var outLen C.size_t
	rc := C.wish_download_file(c.handle, cName, &out, &outLen)
	if err := checkWish(rc, fmt.Sprintf("client.download_file(%q)", name), c.handle); err != nil {
		return nil, err
	}
	defer C.bison_free_string(out)
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), nil
}

// UploadFileFromPath uploads a file, streaming it in chunks from a local
// file on disk.
func (c *Client) UploadFileFromPath(name, localPath string) error {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	cPath := C.CString(localPath)
	defer C.free(unsafe.Pointer(cPath))
	rc := C.wish_upload_file_from_path(c.handle, cName, cPath)
	return checkWish(rc, fmt.Sprintf("client.upload_file_from_path(%q)", name), c.handle)
}

// DownloadFileToPath downloads a file, streaming it directly to a local
// file on disk.
func (c *Client) DownloadFileToPath(name, localPath string) error {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	cPath := C.CString(localPath)
	defer C.free(unsafe.Pointer(cPath))
	rc := C.wish_download_file_to_path(c.handle, cName, cPath)
	return checkWish(rc, fmt.Sprintf("client.download_file_to_path(%q)", name), c.handle)
}

// UploadPackage uploads a local zip archive and has the server unpack it
// into a sandboxed destination directory.
func (c *Client) UploadPackage(destPath, localZipPath string) error {
	cDest := C.CString(destPath)
	defer C.free(unsafe.Pointer(cDest))
	cZip := C.CString(localZipPath)
	defer C.free(unsafe.Pointer(cZip))
	rc := C.wish_upload_package_from_path(c.handle, cDest, cZip)
	return checkWish(rc, fmt.Sprintf("client.upload_package(%q)", destPath), c.handle)
}

// ── Resource packages ────────────────────────────────────────────────────

// ResourceSyncState is the kind of a ResourceSyncStatus report
// (wish_sync_state).
type ResourceSyncState int32

const (
	// ResourceSyncProgress: a chunk was uploaded; more reports follow.
	ResourceSyncProgress ResourceSyncState = 0
	// ResourceSyncUploaded: final; the zip was uploaded and unpacked.
	ResourceSyncUploaded ResourceSyncState = 1
	// ResourceSyncCached: final; the server already had this version.
	ResourceSyncCached ResourceSyncState = 2
	// ResourceSyncFailed: final; the sync failed, see Err.
	ResourceSyncFailed ResourceSyncState = 3
)

// ResourceSyncStatus is one progress or completion report of
// Client.SyncResourcePackage.
type ResourceSyncStatus struct {
	Name        string            // Package name.
	State       ResourceSyncState // Kind of report.
	Transferred uint64            // Bytes uploaded so far.
	Total       uint64            // Size of the local zip.
	Err         string            // Failure message when State is ResourceSyncFailed.
}

// Finished reports whether this is the last report (any state but
// ResourceSyncProgress).
func (s ResourceSyncStatus) Finished() bool {
	return s.State != ResourceSyncProgress
}

// SyncResourcePackage keeps a local zip of resources in sync with the
// session sandbox, in the background.
//
// The server remembers which version (modified time and size) of package
// name it last unpacked; the zip is uploaded and unpacked into destPath
// ("": the sandbox root) only if it changed. Unpacking adds and overwrites
// files, never deletes any. name must be unique, e.g. "acme.game.level1"
// (1-128 characters from [A-Za-z0-9_.-]).
//
// Returns at once. onUpdate (may be nil) runs on a background thread for
// each progress report and once more for the final one (Finished()).
// Returns an error if the sync cannot start (e.g. a missing zip).
func (c *Client) SyncResourcePackage(name, localZipPath, destPath string, onUpdate func(ResourceSyncStatus)) error {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	cZip := C.CString(localZipPath)
	defer C.free(unsafe.Pointer(cZip))
	cDest := C.CString(destPath)
	defer C.free(unsafe.Pointer(cDest))

	h := cgo.NewHandle(onUpdate)
	rc := C.wish_sync_resource_package_shim(
		c.handle,
		cName,
		cZip,
		cDest,
		(C.wish_resource_sync_fn)(unsafe.Pointer(C.goResourceSyncTrampoline)),
		C.uintptr_t(h),
	)
	if rc != C.WISH_OK {
		// The callback never runs when the sync did not start.
		h.Delete()
	}
	return checkWish(rc, fmt.Sprintf("client.sync_resource_package(%q)", name), c.handle)
}

// WaitResourceSyncs blocks until every SyncResourcePackage on this client
// has finished. Must not be called from a sync callback.
func (c *Client) WaitResourceSyncs() error {
	return checkWish(C.wish_wait_resource_syncs(c.handle), "client.wait_resource_syncs", c.handle)
}

// ── User store ───────────────────────────────────────────────────────────
//
// A persistent store of named objects private to this session's identity
// (connect with a "username" param); see docs/persistent-store.md. Every
// method below except HasUserStore fails with WishErrUnavailable for an
// anonymous session.

// HasUserStore reports whether this session has a user store (it is not
// anonymous).
func (c *Client) HasUserStore() (bool, error) {
	var avail C.int
	rc := C.wish_user_store_available(c.handle, &avail)
	if err := checkWish(rc, "client.has_user_store", c.handle); err != nil {
		return false, err
	}
	return avail != 0, nil
}

// UserStoreGet returns a copy of the entry named name, or nil (and no
// error) if it doesn't exist.
func (c *Client) UserStoreGet(name string) (*Value, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	var out C.bison_handle
	rc := C.wish_user_store_get(c.handle, cName, &out)
	if int32(rc) == WishErrNotFound {
		return nil, nil
	}
	if err := checkWish(rc, fmt.Sprintf("client.user_store_get(%q)", name), c.handle); err != nil {
		return nil, err
	}
	return newValueOwned(out), nil
}

// UserStoreSet creates or replaces the entry named name; persisted before
// returning.
func (c *Client) UserStoreSet(name string, value *Value) error {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	var h C.bison_handle
	if value != nil {
		h = value.handle
	}
	rc := C.wish_user_store_set(c.handle, cName, h)
	runtime.KeepAlive(value)
	return checkWish(rc, fmt.Sprintf("client.user_store_set(%q)", name), c.handle)
}

// UserStoreErase removes the entry named name; returns true if it existed.
func (c *Client) UserStoreErase(name string) (bool, error) {
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))
	var erased C.int
	rc := C.wish_user_store_erase(c.handle, cName, &erased)
	if err := checkWish(rc, fmt.Sprintf("client.user_store_erase(%q)", name), c.handle); err != nil {
		return false, err
	}
	return erased != 0, nil
}

// UserStoreKeys returns the names of every entry in the user store, sorted.
func (c *Client) UserStoreKeys() ([]string, error) {
	var out *C.char
	rc := C.wish_user_store_keys(c.handle, &out)
	if err := checkWish(rc, "client.user_store_keys", c.handle); err != nil {
		return nil, err
	}
	defer C.bison_free_string(out)
	var names []string
	if err := json.Unmarshal([]byte(C.GoString(out)), &names); err != nil {
		return nil, fmt.Errorf("wish: user_store_keys: %w", err)
	}
	return names, nil
}

// ── Settings metadata ────────────────────────────────────────────────────
//
// Describes settings kept in the user store so settings tools (the
// bdg/desktop/conf tool) can show and edit them; see docs/settings.md.

// SettingOption is one choice of an enum setting.
type SettingOption struct {
	Value string `json:"value"`
	Label string `json:"label"`
}

// SettingInfo is the metadata of one setting: field "color" of the user
// store entry "bdg.desktop.example" is named "bdg.desktop.example.color".
type SettingInfo struct {
	Name        string          `json:"name"`
	Type        string          `json:"type"` // bool, int, float, string, enum, color or path
	Label       string          `json:"label"`
	Description string          `json:"description"`
	Category    string          `json:"category"` // sidebar category id
	Section     string          `json:"section"`  // group title inside the category
	Default     any             `json:"default"`  // nil, bool, int, float64 or string
	Min         *float64        `json:"min,omitempty"`
	Max         *float64        `json:"max,omitempty"`
	Step        *float64        `json:"step,omitempty"`
	Options     []SettingOption `json:"options,omitempty"` // enum choices
	Icon        string          `json:"icon"`
	Order       int32           `json:"order"`
	Restart     bool            `json:"restart"` // takes effect only after a restart
}

// SettingsCategory is the metadata of one sidebar category.
type SettingsCategory struct {
	ID          string `json:"id"`
	Label       string `json:"label"`
	Description string `json:"description"`
	Icon        string `json:"icon"`
	Color       string `json:"color"` // badge color, "#RRGGBB"
	Order       int32  `json:"order"`
}

// DescribeSetting describes a setting (replacing an earlier description).
// It fails with WishErrUnavailable for an anonymous session.
func (c *Client) DescribeSetting(info SettingInfo) error {
	if info.Type == "" {
		info.Type = "string"
	}
	data, err := json.Marshal(info)
	if err != nil {
		return fmt.Errorf("wish: describe_setting: %w", err)
	}
	cJSON := C.CString(string(data))
	defer C.free(unsafe.Pointer(cJSON))
	rc := C.wish_settings_describe(c.handle, cJSON)
	return checkWish(rc, "client.describe_setting("+info.Name+")", c.handle)
}

// ReadSetting returns the current value of info in the user store: the
// stored value when it suits the setting, else info.Default (also for an
// anonymous session). The value is a bool, a float64 (int and float
// settings), a string, or nil when there is no default.
func (c *Client) ReadSetting(info SettingInfo) (any, error) {
	if info.Type == "" {
		info.Type = "string"
	}
	data, err := json.Marshal(info)
	if err != nil {
		return nil, fmt.Errorf("wish: read_setting: %w", err)
	}
	cJSON := C.CString(string(data))
	defer C.free(unsafe.Pointer(cJSON))
	var out *C.char
	rc := C.wish_settings_read(c.handle, cJSON, &out)
	if err := checkWish(rc, "client.read_setting("+info.Name+")", c.handle); err != nil {
		return nil, err
	}
	defer C.bison_free_string(out)
	var v any
	if err := json.Unmarshal([]byte(C.GoString(out)), &v); err != nil {
		return nil, fmt.Errorf("wish: read_setting: %w", err)
	}
	return v, nil
}

// DescribeSettingsCategory describes a sidebar category settings refer to by id.
func (c *Client) DescribeSettingsCategory(category SettingsCategory) error {
	data, err := json.Marshal(category)
	if err != nil {
		return fmt.Errorf("wish: describe_settings_category: %w", err)
	}
	cJSON := C.CString(string(data))
	defer C.free(unsafe.Pointer(cJSON))
	rc := C.wish_settings_describe_category(c.handle, cJSON)
	return checkWish(rc, "client.describe_settings_category("+category.ID+")", c.handle)
}

// ── Notifications ────────────────────────────────────────────────────────

// NotifyKind is a notification's severity: its accent color and icon
// (WISH_NOTIFY_*).
type NotifyKind int32

const (
	NotifyInfo    NotifyKind = 0
	NotifySuccess NotifyKind = 1
	NotifyWarning NotifyKind = 2
	NotifyError   NotifyKind = 3
)

// Notification is a toast for Client.Notify. Title is required; a zero
// Duration keeps it until dismissed (at most 5 minutes).
type Notification struct {
	Title    string
	Body     string
	Kind     NotifyKind
	Keys     string // key combination drawn as key caps, e.g. "Ctrl+B"
	Duration time.Duration
	Tag      string // replaces this session's notification with the same tag
}

// Notify shows a toast notification on the server's screen and returns its
// id. The server shows this session's identity on it and keeps at most 4
// per session. Fails when the server shows no notifications or the title is
// empty.
func (c *Client) Notify(n Notification) (uint64, error) {
	cTitle := C.CString(n.Title)
	defer C.free(unsafe.Pointer(cTitle))
	cBody := C.CString(n.Body)
	defer C.free(unsafe.Pointer(cBody))
	cKeys := C.CString(n.Keys)
	defer C.free(unsafe.Pointer(cKeys))
	cTag := C.CString(n.Tag)
	defer C.free(unsafe.Pointer(cTag))
	var id C.uint64_t
	rc := C.wish_notify(c.handle, cTitle, cBody, C.int32_t(n.Kind), cKeys,
		C.int32_t(n.Duration.Milliseconds()), cTag, &id)
	if err := checkWish(rc, "client.notify", c.handle); err != nil {
		return 0, err
	}
	return uint64(id), nil
}

// DismissNotification removes one of this session's notifications early.
func (c *Client) DismissNotification(id uint64) error {
	rc := C.wish_notify_dismiss(c.handle, C.uint64_t(id))
	return checkWish(rc, "client.dismiss_notification", c.handle)
}

// ── Logging ──────────────────────────────────────────────────────────────

func (c *Client) Log(level, msg string) error {
	cLevel := C.CString(level)
	defer C.free(unsafe.Pointer(cLevel))
	cMsg := C.CString(msg)
	defer C.free(unsafe.Pointer(cMsg))
	rc := C.wish_log(c.handle, cLevel, cMsg)
	return checkWish(rc, "client.log", c.handle)
}

func (c *Client) LogDebug(msg string) error {
	cMsg := C.CString(msg)
	defer C.free(unsafe.Pointer(cMsg))
	rc := C.wish_log_debug(c.handle, cMsg)
	return checkWish(rc, "client.log_debug", c.handle)
}

func (c *Client) LogInfo(msg string) error {
	cMsg := C.CString(msg)
	defer C.free(unsafe.Pointer(cMsg))
	rc := C.wish_log_info(c.handle, cMsg)
	return checkWish(rc, "client.log_info", c.handle)
}

func (c *Client) LogWarn(msg string) error {
	cMsg := C.CString(msg)
	defer C.free(unsafe.Pointer(cMsg))
	rc := C.wish_log_warn(c.handle, cMsg)
	return checkWish(rc, "client.log_warn", c.handle)
}

func (c *Client) LogError(msg string) error {
	cMsg := C.CString(msg)
	defer C.free(unsafe.Pointer(cMsg))
	rc := C.wish_log_error(c.handle, cMsg)
	return checkWish(rc, "client.log_error", c.handle)
}

// ── Automation ────────────────────────────────────────────────────────────
//
// Native (ABI-driven) automation: query the widget tree, take screenshots,
// and inject synthetic input -- only available when the connected server's
// active renderer implements the automation backend (currently only the
// SDL3 renderer); see src/automation/DESIGN.md.

// AutomationGetTree queries the current widget tree/hit-test snapshot as a
// raw JSON string, optionally filtered to root and its descendants (pass
// "" for the whole tree).
func (c *Client) AutomationGetTree(root string) (string, error) {
	cRoot := cCharOrNil(root)
	if cRoot != nil {
		defer C.free(unsafe.Pointer(cRoot))
	}
	var out *C.char
	rc := C.wish_automation_get_tree(c.handle, cRoot, &out)
	if err := checkWish(rc, "client.automation_get_tree", c.handle); err != nil {
		return "", err
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// AutomationGetLogs retrieves the session's buffered automation log entries
// as a raw JSON string.
func (c *Client) AutomationGetLogs() (string, error) {
	var out *C.char
	rc := C.wish_automation_get_logs(c.handle, &out)
	if err := checkWish(rc, "client.automation_get_logs", c.handle); err != nil {
		return "", err
	}
	defer C.bison_free_string(out)
	return C.GoString(out), nil
}

// AutomationScreenshot captures a screenshot of the next frame the server
// renders, as PNG-encoded bytes.
func (c *Client) AutomationScreenshot() ([]byte, error) {
	var out *C.char
	var outLen C.size_t
	rc := C.wish_automation_screenshot(c.handle, &out, &outLen)
	if err := checkWish(rc, "client.automation_screenshot", c.handle); err != nil {
		return nil, err
	}
	defer C.bison_free_string(out)
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen)), nil
}

// AutomationMouseMove injects a synthetic mouse-move event (window-relative
// coordinates).
func (c *Client) AutomationMouseMove(x, y float32) error {
	rc := C.wish_automation_mouse_move(c.handle, C.float(x), C.float(y))
	return checkWish(rc, "client.automation_mouse_move", c.handle)
}

// AutomationMouseButton injects a synthetic mouse-button press/release.
// button: 0 = left, 1 = right, 2 = middle.
func (c *Client) AutomationMouseButton(button int, down bool) error {
	var d C.int
	if down {
		d = 1
	}
	rc := C.wish_automation_mouse_button(c.handle, C.int(button), d)
	return checkWish(rc, "client.automation_mouse_button", c.handle)
}

// AutomationKeyEvent injects a synthetic key press/release (keycode is the
// platform keycode, SDL_Keycode for the SDL3 renderer).
func (c *Client) AutomationKeyEvent(keycode int, down bool) error {
	var d C.int
	if down {
		d = 1
	}
	rc := C.wish_automation_key_event(c.handle, C.int(keycode), d)
	return checkWish(rc, "client.automation_key_event", c.handle)
}

// AutomationTextInput injects synthetic UTF-8 text input (e.g. for typing
// into an InputText).
func (c *Client) AutomationTextInput(text string) error {
	cText := C.CString(text)
	defer C.free(unsafe.Pointer(cText))
	rc := C.wish_automation_text_input(c.handle, cText)
	return checkWish(rc, "client.automation_text_input", c.handle)
}
