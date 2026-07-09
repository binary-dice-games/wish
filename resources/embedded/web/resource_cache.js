// MIT License © 2025 Binary Dice Games
//
// Persistent, browser-side cache of server-pushed texture resources, keyed
// by (path, crc32) -- see src/web/DESIGN.md's TEX_CHECK / CACHE_RESPONSE
// handshake. Backed by IndexedDB so a resource already seen in an earlier
// browser session survives a page reload without being re-uploaded.
//
// Loaded before client.js (see index.html) and exposes window.WishResourceCache.
// Entirely best-effort: any IndexedDB failure (unsupported browser, private
// browsing restrictions, quota errors) degrades to "always a cache miss"
// rather than breaking rendering -- callers never need to handle a rejection.

(function () {
  "use strict";

  const DB_NAME = "wish-resource-cache";
  const DB_VERSION = 1;
  const STORE_NAME = "textures";

  // How long a cached entry is trusted before sweepExpired() evicts it.
  // Tune by changing this constant only.
  const TTL_MS = 30 * 24 * 60 * 60 * 1000; // 30 days

  function keyFor(path, crc32) {
    return path + "@" + crc32;
  }

  let dbPromise = null;

  // Real open, which can reject; open() below wraps this and never rejects.
  function openDb() {
    return new Promise((resolve, reject) => {
      if (!window.indexedDB) {
        reject(new Error("indexedDB is not available"));
        return;
      }
      const req = indexedDB.open(DB_NAME, DB_VERSION);
      req.onupgradeneeded = () => {
        const db = req.result;
        if (!db.objectStoreNames.contains(STORE_NAME)) {
          const store = db.createObjectStore(STORE_NAME, { keyPath: "key" });
          store.createIndex("storedAt", "storedAt");
        }
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }

  // Public open(): caches the (possibly failed) attempt so repeated calls
  // don't reopen the database, and never rejects -- a failure just means
  // every lookup() is a miss and every store()/sweepExpired() is a no-op.
  async function open() {
    if (!dbPromise) {
      dbPromise = openDb().catch((err) => {
        console.warn("[wish] resource cache unavailable:", err);
        return null;
      });
    }
    return dbPromise;
  }

  async function lookup(path, crc32) {
    const db = await open();
    if (!db)
      return null;
    return new Promise((resolve) => {
      const tx = db.transaction(STORE_NAME, "readonly");
      const req = tx.objectStore(STORE_NAME).get(keyFor(path, crc32));
      req.onsuccess = () => resolve(req.result || null);
      req.onerror = () => resolve(null);
    });
  }

  async function store(path, crc32, width, height, isAlpha, pixels) {
    const db = await open();
    if (!db)
      return;
    return new Promise((resolve) => {
      const tx = db.transaction(STORE_NAME, "readwrite");
      tx.objectStore(STORE_NAME).put({
        key: keyFor(path, crc32),
        path,
        crc32,
        width,
        height,
        isAlpha,
        pixels: pixels.slice(), // detach from the WS message's ArrayBuffer view
        storedAt: Date.now(),
      });
      tx.oncomplete = () => resolve();
      tx.onerror = () => resolve();
    });
  }

  async function sweepExpired(ttlMs) {
    const db = await open();
    if (!db)
      return;
    return new Promise((resolve) => {
      const tx = db.transaction(STORE_NAME, "readwrite");
      const range = IDBKeyRange.upperBound(Date.now() - ttlMs);
      const req = tx.objectStore(STORE_NAME).index("storedAt").openCursor(range);
      req.onsuccess = () => {
        const cursor = req.result;
        if (cursor) {
          cursor.delete();
          cursor.continue();
        }
      };
      tx.oncomplete = () => resolve();
      tx.onerror = () => resolve();
    });
  }

  window.WishResourceCache = { open, lookup, store, sweepExpired, TTL_MS };
})();
