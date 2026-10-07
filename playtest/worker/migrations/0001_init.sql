CREATE TABLE testers (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  token_hash TEXT NOT NULL UNIQUE,
  created_at TEXT NOT NULL,
  revoked_at TEXT
);

CREATE TABLE games (
  product TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  repo TEXT NOT NULL
);

CREATE TABLE builds (
  product TEXT NOT NULL REFERENCES games(product),
  build TEXT NOT NULL,
  created_at TEXT NOT NULL,
  release_url TEXT,
  PRIMARY KEY (product, build)
);

CREATE TABLE sessions (
  id TEXT PRIMARY KEY,
  tester_id TEXT NOT NULL REFERENCES testers(id),
  product TEXT NOT NULL,
  build TEXT NOT NULL,
  started_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  ended_at TEXT,
  exit TEXT,
  vblanks INTEGER NOT NULL DEFAULT 0,
  uploads INTEGER NOT NULL DEFAULT 0,
  bytes INTEGER NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'playing' CHECK (status IN ('playing', 'ended', 'reviewed')),
  summary TEXT,
  new_functions INTEGER,
  new_bytes INTEGER,
  missed TEXT,
  reviewed_at TEXT
);

CREATE INDEX sessions_by_tester ON sessions (tester_id, started_at);
CREATE INDEX sessions_by_status ON sessions (status, updated_at);
