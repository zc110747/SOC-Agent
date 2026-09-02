// Package database opens the SQLite store and applies the schema.
// modernc.org/sqlite is a pure-Go driver (no cgo), which keeps the build
// cross-platform (Windows/Linux) and embeddable.
package database

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"

	_ "modernc.org/sqlite"

	"video-server/internal/metadata"
)

// Open opens (creating if needed) the SQLite database at path and runs the
// schema migration. Parent directories are created automatically.
func Open(path string) (*sql.DB, error) {
	if dir := filepath.Dir(path); dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return nil, fmt.Errorf("create db dir: %w", err)
		}
	}
	// busy_timeout avoids "database is locked" under concurrent access.
	dsn := path + "?_pragma=busy_timeout(5000)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open db: %w", err)
	}
	if err := db.Ping(); err != nil {
		return nil, fmt.Errorf("ping db: %w", err)
	}
	if err := migrate(db); err != nil {
		return nil, fmt.Errorf("migrate db: %w", err)
	}
	return db, nil
}

func migrate(db *sql.DB) error {
	_, err := db.Exec(`
CREATE TABLE IF NOT EXISTS cameras (
	id           TEXT PRIMARY KEY,
	name         TEXT NOT NULL,
	stream_path  TEXT NOT NULL UNIQUE,
	device_ip    TEXT,
	status       TEXT NOT NULL DEFAULT 'offline',
	resolution   TEXT,
	fps          INTEGER NOT NULL DEFAULT 0,
	bitrate      INTEGER NOT NULL DEFAULT 0,
	created_at   TEXT NOT NULL,
	updated_at   TEXT NOT NULL,
	last_seen    TEXT
);
`)
	if err != nil {
		return err
	}
	// AI metadata tables (camera-agent "Metadata" protocol). Kept in the same
	// database so a camera and its AI results can never drift apart across
	// backup/restore, but migrated by the metadata package itself.
	return metadata.Migrate(db)
}
