-- Sample database for manually testing the wish `sq` module.
-- Regenerate: rm -f sample.db && sqlite3 sample.db < sample.sql
--   (or: python3 -c "import sqlite3;sqlite3.connect('sample.db').executescript(open('sample.sql').read())")
-- Register:   sq add ./sample.db --handle @sample

PRAGMA foreign_keys = ON;

-- ── A small relational schema: foreign keys, primary keys, a view ─────────
CREATE TABLE artist (
  id      INTEGER PRIMARY KEY,
  name    TEXT NOT NULL,
  country TEXT
);
CREATE TABLE album (
  id        INTEGER PRIMARY KEY,
  title     TEXT NOT NULL,
  artist_id INTEGER NOT NULL REFERENCES artist(id),
  released  DATE,
  price     REAL
);
CREATE TABLE track (
  id       INTEGER PRIMARY KEY,
  album_id INTEGER NOT NULL REFERENCES album(id),
  no       INTEGER NOT NULL,
  title    TEXT NOT NULL,
  seconds  INTEGER,
  UNIQUE (album_id, no)
);
-- Composite primary key + two foreign keys.
CREATE TABLE playlist_track (
  playlist TEXT NOT NULL,
  track_id INTEGER NOT NULL REFERENCES track(id),
  position INTEGER NOT NULL,
  PRIMARY KEY (playlist, position)
);

INSERT INTO artist(name, country) VALUES
  ('Miles Davis', 'US'), ('John Coltrane', 'US'), ('Björk', 'IS'),
  ('坂本龍一', 'JP'), ('Unknown Artist', NULL);

INSERT INTO album(title, artist_id, released, price) VALUES
  ('Kind of Blue',            1, '1959-08-17', 9.50),
  ('Bitches Brew',            1, '1970-03-30', 12.99),
  ('A Love Supreme',          2, '1965-01-01', 11.00),
  ('Homogenic',               3, '1997-09-22', 10.25),
  ('async',                   4, '2017-03-29', NULL),
  ('Untitled (no release)',   5, NULL,         0);

INSERT INTO track(album_id, no, title, seconds) VALUES
  (1, 1, 'So What', 562), (1, 2, 'Freddie Freeloader', 589), (1, 3, 'Blue in Green', 337),
  (2, 1, 'Pharaoh''s Dance', 1225), (2, 2, 'Bitches Brew', 1653),
  (3, 1, 'Acknowledgement', 462), (3, 2, 'Resolution', 442),
  (4, 1, 'Hunter', 255), (4, 2, 'Jóga', 305),
  (5, 1, 'andata', 366), (5, 2, 'disko', NULL);

INSERT INTO playlist_track(playlist, track_id, position) VALUES
  ('Late night', 1, 1), ('Late night', 8, 2), ('Focus', 6, 1), ('Focus', 10, 2);

CREATE VIEW album_overview AS
  SELECT al.id, al.title, ar.name AS artist, al.released,
         COUNT(t.id) AS tracks, SUM(t.seconds) AS total_seconds
  FROM album al
  JOIN artist ar ON ar.id = al.artist_id
  LEFT JOIN track t ON t.album_id = al.id
  GROUP BY al.id;

-- ── Edge cases for the results grid ───────────────────────────────────────
-- NULL vs empty string, multi-line and very long text, unicode, quotes.
CREATE TABLE edge_cases (
  id      INTEGER PRIMARY KEY,
  label   TEXT,
  value   TEXT,
  number  REAL,
  flag    BOOLEAN
);
INSERT INTO edge_cases(label, value, number, flag) VALUES
  ('null value',        NULL,                       NULL, NULL),
  ('empty string',      '',                         0,    0),
  ('multi-line',        'line 1' || char(10) || 'line 2' || char(10) || 'line 3', 1.5, 1),
  ('quotes',            'He said "hi" and ''bye''', -2.25, 1),
  ('unicode',           'héllo wörld ✓ 日本語 😀',   3.14159265358979, 0),
  ('looks like NULL',   'NULL',                     1e21, 1),
  ('leading/trailing',  '  padded  ',               1e-9, 0),
  ('very long text',    (SELECT group_concat('word' || n, ' ')
                         FROM (WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n < 200) SELECT n FROM c)),
                                                     42,   1),
  ('csv-hostile',       'a,b;"c"' || char(10) || 'd', 7, 0);

-- ── Identifier quoting: names with spaces, quotes and a reserved word ─────
CREATE TABLE "order details" (
  "order" INTEGER PRIMARY KEY,
  "select" TEXT,
  "unit price" REAL
);
INSERT INTO "order details" VALUES (1, 'widget', 2.5), (2, 'gadget', 10);

CREATE TABLE "say ""hi""" (x INTEGER);
INSERT INTO "say ""hi""" VALUES (1);

-- ── An empty table (a result with no rows) ────────────────────────────────
CREATE TABLE empty_table (id INTEGER PRIMARY KEY, note TEXT);

-- ── A wide table (30 columns) to exercise horizontal scrolling ────────────
CREATE TABLE wide (
  id INTEGER PRIMARY KEY,
  c01 TEXT, c02 TEXT, c03 TEXT, c04 TEXT, c05 TEXT, c06 TEXT, c07 TEXT, c08 TEXT, c09 TEXT, c10 TEXT,
  c11 TEXT, c12 TEXT, c13 TEXT, c14 TEXT, c15 TEXT, c16 TEXT, c17 TEXT, c18 TEXT, c19 TEXT, c20 TEXT,
  c21 TEXT, c22 TEXT, c23 TEXT, c24 TEXT, c25 TEXT, c26 TEXT, c27 TEXT, c28 TEXT, c29 TEXT, c30 TEXT
);
INSERT INTO wide
  WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 5)
  SELECT i, 'r'||i||'c01','r'||i||'c02','r'||i||'c03','r'||i||'c04','r'||i||'c05','r'||i||'c06','r'||i||'c07','r'||i||'c08','r'||i||'c09','r'||i||'c10',
            'r'||i||'c11','r'||i||'c12','r'||i||'c13','r'||i||'c14','r'||i||'c15','r'||i||'c16','r'||i||'c17','r'||i||'c18','r'||i||'c19','r'||i||'c20',
            'r'||i||'c21','r'||i||'c22','r'||i||'c23','r'||i||'c24','r'||i||'c25','r'||i||'c26','r'||i||'c27','r'||i||'c28','r'||i||'c29','r'||i||'c30'
  FROM n;

-- ── A big table (12,000 rows) to exercise the row limit / truncation and ──
-- ── the full-result CSV export (more than the 5000 maximum display limit) ─
CREATE TABLE measurement (
  id        INTEGER PRIMARY KEY,
  sensor    TEXT NOT NULL,
  taken_at  TEXT NOT NULL,
  value     REAL,
  ok        BOOLEAN NOT NULL
);
INSERT INTO measurement(sensor, taken_at, value, ok)
  WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i < 12000)
  SELECT 'sensor-' || (i % 8),
         datetime('2026-01-01 00:00:00', '+' || (i * 5) || ' minutes'),
         CASE WHEN i % 97 = 0 THEN NULL ELSE round(20 + 8 * sin(i / 50.0) + (i % 7) * 0.1, 3) END,
         i % 13 <> 0
  FROM n;
CREATE INDEX measurement_sensor ON measurement(sensor, taken_at);
