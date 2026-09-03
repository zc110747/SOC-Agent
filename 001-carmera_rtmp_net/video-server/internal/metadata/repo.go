package metadata

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"sync"
	"time"

	"video-server/internal/logger"
)

// Repository persists AI metadata pushed by camera-agents.
//
// Storage is deliberately split by update pattern:
//
//	ai_status  one row per camera, overwritten on every heartbeat  (latest only)
//	ai_frame   one row per camera, overwritten on every result     (latest only)
//	ai_object  append-only detail rows, pruned to a bounded window
//
// Keeping the "latest" rows separate is what makes GET cheap: the UI polls it
// every second or so, and it must never scan a growing history table.
type Repository struct {
	db *sql.DB

	mu        sync.Mutex
	saveCount int
	// pruneEvery is how many frame saves happen between two prune passes.
	// Pruning on every write would turn a 5 msg/s stream into 5 DELETEs/s;
	// doing it periodically keeps the write path to one INSERT per object.
	pruneEvery int
	// retention is the number of object rows kept per camera. <=0 disables
	// pruning entirely (the table then grows without bound - opt in knowingly).
	retention int
}

// NewRepository builds a metadata repository.
//   - retention: object rows kept per camera (<=0 = unlimited)
//   - pruneEvery: frames between two prune passes (<=0 falls back to 64)
func NewRepository(db *sql.DB, retention, pruneEvery int) *Repository {
	if pruneEvery <= 0 {
		pruneEvery = 64
	}
	return &Repository{db: db, retention: retention, pruneEvery: pruneEvery}
}

// Migrate creates the metadata tables. It is called from the shared schema
// migration so a fresh database is ready in one step.
func Migrate(db *sql.DB) error {
	stmts := []string{
		`CREATE TABLE IF NOT EXISTS ai_status (
			camera_id      TEXT PRIMARY KEY,
			version        INTEGER NOT NULL DEFAULT 0,
			ai_enable      INTEGER NOT NULL DEFAULT 0,
			ai_running     INTEGER NOT NULL DEFAULT 0,
			ai_fps         REAL    NOT NULL DEFAULT 0,
			model          TEXT,
			tracker        TEXT,
			last_frame_id  INTEGER NOT NULL DEFAULT 0,
			last_timestamp INTEGER NOT NULL DEFAULT 0,
			processed      INTEGER NOT NULL DEFAULT 0,
			wall_clock     INTEGER NOT NULL DEFAULT 0,
			received_at    TEXT NOT NULL,
			updated_at     TEXT NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS ai_frame (
			camera_id     TEXT PRIMARY KEY,
			frame_id      INTEGER NOT NULL,
			timestamp     INTEGER NOT NULL,
			video_width   INTEGER NOT NULL DEFAULT 0,
			video_height  INTEGER NOT NULL DEFAULT 0,
			object_count  INTEGER NOT NULL DEFAULT 0,
			received_at   TEXT NOT NULL,
			updated_at    TEXT NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS ai_object (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			camera_id   TEXT    NOT NULL,
			frame_id    INTEGER NOT NULL,
			class       TEXT    NOT NULL,
			confidence  REAL    NOT NULL,
			track_id    INTEGER NOT NULL DEFAULT 0,
			x1          INTEGER NOT NULL,
			y1          INTEGER NOT NULL,
			x2          INTEGER NOT NULL,
			y2          INTEGER NOT NULL,
			keypoints   TEXT,
			received_at TEXT    NOT NULL
		)`,
		// Serves both the latest-frame lookup and the retention prune.
		`CREATE INDEX IF NOT EXISTS idx_ai_object_camera_id ON ai_object(camera_id, id)`,
	}
	for _, s := range stmts {
		if _, err := db.Exec(s); err != nil {
			return fmt.Errorf("migrate metadata: %w", err)
		}
	}
	// Existing databases were created before the keypoints column existed.
	// ADD COLUMN is a no-op on a fresh DB (the CREATE above already has it);
	// on an old DB it backfills the nullable column without touching rows.
	if err := addColumnIfMissing(db, "ai_object", "keypoints", "TEXT"); err != nil {
		return fmt.Errorf("migrate ai_object.keypoints: %w", err)
	}
	return nil
}

// addColumnIfMissing issues ALTER TABLE ... ADD COLUMN only when the column is
// absent, so Migrate is safe to re-run on databases of any age.
func addColumnIfMissing(db *sql.DB, table, column, typ string) error {
	rows, err := db.Query("SELECT 1 FROM pragma_table_info(?) WHERE name = ?", table, column)
	if err != nil {
		return err
	}
	exists := rows.Next()
	rows.Close()
	if exists {
		return nil
	}
	_, err = db.Exec("ALTER TABLE " + table + " ADD COLUMN " + column + " " + typ)
	return err
}

const timeFmt = time.RFC3339Nano

// SaveFrame stores one inference result: it upserts the per-camera "latest
// frame" row and appends the detected objects to the bounded history.
func (r *Repository) SaveFrame(f *FrameMessage, received time.Time) error {
	now := received.UTC().Format(timeFmt)
	recv := now

	tx, err := r.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	if _, err := tx.Exec(`
		INSERT INTO ai_frame (camera_id,frame_id,timestamp,video_width,video_height,object_count,received_at,updated_at)
		VALUES (?,?,?,?,?,?,?,?)
		ON CONFLICT(camera_id) DO UPDATE SET
			frame_id=excluded.frame_id, timestamp=excluded.timestamp,
			video_width=excluded.video_width, video_height=excluded.video_height,
			object_count=excluded.object_count,
			received_at=excluded.received_at, updated_at=excluded.updated_at`,
		f.CameraID, f.FrameID, f.Timestamp, f.VideoWidth, f.VideoHeight,
		len(f.Objects), recv, now); err != nil {
		return fmt.Errorf("upsert ai_frame: %w", err)
	}

	ins, err := tx.Prepare(`
		INSERT INTO ai_object (camera_id,frame_id,class,confidence,track_id,x1,y1,x2,y2,keypoints,received_at)
		VALUES (?,?,?,?,?,?,?,?,?,?,?)`)
	if err != nil {
		return fmt.Errorf("prepare ai_object: %w", err)
	}
	defer ins.Close()
	for _, o := range f.Objects {
		if _, err := ins.Exec(f.CameraID, f.FrameID, o.Class, o.Confidence, o.TrackID,
			o.BBox[0], o.BBox[1], o.BBox[2], o.BBox[3], kpJSON(o), recv); err != nil {
			return fmt.Errorf("insert ai_object: %w", err)
		}
	}

	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit frame: %w", err)
	}
	r.maybePrune(f.CameraID)
	return nil
}

// SaveStatus stores one AI heartbeat (latest per camera).
func (r *Repository) SaveStatus(s *StatusMessage, received time.Time) error {
	now := received.UTC().Format(timeFmt)
	_, err := r.db.Exec(`
		INSERT INTO ai_status
			(camera_id,version,ai_enable,ai_running,ai_fps,model,tracker,
			 last_frame_id,last_timestamp,processed,wall_clock,received_at,updated_at)
		VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)
		ON CONFLICT(camera_id) DO UPDATE SET
			version=excluded.version, ai_enable=excluded.ai_enable,
			ai_running=excluded.ai_running, ai_fps=excluded.ai_fps,
			model=excluded.model, tracker=excluded.tracker,
			last_frame_id=excluded.last_frame_id,
			last_timestamp=excluded.last_timestamp,
			processed=excluded.processed, wall_clock=excluded.wall_clock,
			received_at=excluded.received_at, updated_at=excluded.updated_at`,
		s.CameraID, s.Version, boolInt(s.AI.Enable), boolInt(s.AI.Running), s.AI.FPS,
		nullStr(s.AI.Model), nullStr(s.AI.Tracker),
		s.AI.LastFrameID, s.AI.LastTimestamp, s.AI.Processed, s.WallClock, now, now)
	if err != nil {
		return fmt.Errorf("upsert ai_status: %w", err)
	}
	return nil
}

// Latest returns the newest frame + status for one camera. Both may be nil when
// nothing has arrived yet; that is not an error so the endpoint can answer 200
// with an empty snapshot instead of making the UI handle 404s.
func (r *Repository) Latest(cameraID string) (Snapshot, error) {
	snap := Snapshot{CameraID: cameraID}

	var (
		frameID, ts    int64
		w, h, objCount int
		receivedAt     string
	)
	err := r.db.QueryRow(`
		SELECT frame_id,timestamp,video_width,video_height,object_count,received_at
		FROM ai_frame WHERE camera_id = ?`, cameraID).
		Scan(&frameID, &ts, &w, &h, &objCount, &receivedAt)
	switch {
	case err == nil:
		objs, oerr := r.objects(cameraID, frameID)
		if oerr != nil {
			return snap, oerr
		}
		snap.Frame = &FrameView{
			FrameID:     frameID,
			Timestamp:   ts,
			VideoWidth:  w,
			VideoHeight: h,
			ObjectCount: objCount,
			ReceivedAt:  parseTime(receivedAt),
			Objects:     objs,
		}
	case err == sql.ErrNoRows:
		// no frames yet - fall through, status may still exist
	default:
		return snap, fmt.Errorf("read ai_frame: %w", err)
	}

	st, err := r.status(cameraID)
	switch {
	case err == nil:
		snap.Status = st
	case err == sql.ErrNoRows:
		// neither kind has arrived yet
	default:
		return snap, fmt.Errorf("read ai_status: %w", err)
	}
	return snap, nil
}

// List returns a snapshot for every camera that has ever sent metadata, so the
// UI can render one card per device without knowing their ids up front.
func (r *Repository) List() ([]Snapshot, error) {
	ids, err := r.CameraIDs()
	if err != nil {
		return nil, err
	}
	out := make([]Snapshot, 0, len(ids))
	for _, id := range ids {
		s, err := r.Latest(id)
		if err != nil {
			return nil, err
		}
		out = append(out, s)
	}
	return out, nil
}

// CameraIDs returns every camera id present in either metadata table, most
// recently updated first (bounded, so a stuck camera cannot starve the rest).
func (r *Repository) CameraIDs() ([]string, error) {
	rows, err := r.db.Query(`
		SELECT camera_id, MAX(updated_at) AS u FROM (
			SELECT camera_id, updated_at FROM ai_frame
			UNION ALL
			SELECT camera_id, updated_at FROM ai_status
		) GROUP BY camera_id ORDER BY u DESC`)
	if err != nil {
		return nil, fmt.Errorf("list metadata cameras: %w", err)
	}
	defer rows.Close()
	var ids []string
	for rows.Next() {
		var (
			id string
			// MAX(updated_at) is only used for ordering; its value is not needed.
			lastUpdate sql.NullString
		)
		if err := rows.Scan(&id, &lastUpdate); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

func (r *Repository) status(cameraID string) (*StatusView, error) {
	var (
		s               StatusView
		enable, running int
		model, tracker  sql.NullString
		receivedAt      string
	)
	err := r.db.QueryRow(`
		SELECT version,ai_enable,ai_running,ai_fps,model,tracker,
		       last_frame_id,last_timestamp,processed,wall_clock,received_at
		FROM ai_status WHERE camera_id = ?`, cameraID).
		Scan(&s.Version, &enable, &running, &s.FPS, &model, &tracker,
			&s.LastFrameID, &s.LastTimestamp, &s.Processed, &s.WallClock, &receivedAt)
	if err != nil {
		return nil, err
	}
	s.Enable = enable != 0
	s.Running = running != 0
	s.Model = model.String
	s.Tracker = tracker.String
	s.ReceivedAt = parseTime(receivedAt)
	return &s, nil
}

// kpJSON marshals an object's pose keypoints to JSON for storage, or nil when
// the object has none (detection model) so the column stays NULL and the wire
// format for non-pose frames is unchanged.
func kpJSON(o Object) interface{} {
	if len(o.Keypoints) == 0 {
		return nil
	}
	b, err := json.Marshal(o.Keypoints)
	if err != nil {
		return nil
	}
	return b
}

func (r *Repository) objects(cameraID string, frameID int64) ([]ObjectView, error) {
	rows, err := r.db.Query(`
		SELECT class,confidence,track_id,x1,y1,x2,y2,keypoints
		FROM ai_object WHERE camera_id = ? AND frame_id = ? ORDER BY id ASC`,
		cameraID, frameID)
	if err != nil {
		return nil, fmt.Errorf("read ai_object: %w", err)
	}
	defer rows.Close()
	out := []ObjectView{}
	for rows.Next() {
		var o ObjectView
		var kp []byte
		if err := rows.Scan(&o.Class, &o.Confidence, &o.TrackID,
			&o.BBox[0], &o.BBox[1], &o.BBox[2], &o.BBox[3], &kp); err != nil {
			return nil, err
		}
		if len(kp) > 0 {
			var ks []Keypoint
			if err := json.Unmarshal(kp, &ks); err == nil {
				o.Keypoints = ks
			}
		}
		out = append(out, o)
	}
	return out, rows.Err()
}

// maybePrune trims the object history for one camera every pruneEvery saves.
// It runs on the caller's goroutine: at 5 msg/s that is one DELETE every ~13s,
// which is far cheaper than a background ticker and cannot leak a goroutine.
func (r *Repository) maybePrune(cameraID string) {
	if r.retention <= 0 {
		return
	}
	r.mu.Lock()
	r.saveCount++
	due := r.saveCount%r.pruneEvery == 0
	r.mu.Unlock()
	if !due {
		return
	}
	if err := r.Prune(cameraID, r.retention); err != nil {
		logger.Warn("metadata prune %s failed: %v", cameraID, err)
	}
}

// Prune keeps only the newest `keep` object rows for one camera.
func (r *Repository) Prune(cameraID string, keep int) error {
	if keep <= 0 {
		return nil
	}
	res, err := r.db.Exec(`
		DELETE FROM ai_object
		WHERE camera_id = ?
		  AND id NOT IN (
			SELECT id FROM ai_object WHERE camera_id = ? ORDER BY id DESC LIMIT ?
		  )`, cameraID, cameraID, keep)
	if err != nil {
		return fmt.Errorf("prune ai_object: %w", err)
	}
	if n, _ := res.RowsAffected(); n > 0 {
		logger.Debug("metadata pruned %d old object rows for %s", n, cameraID)
	}
	return nil
}

// DeleteCamera removes every metadata row for one camera. Used when the
// camera itself is deleted, so the DB does not keep orphaned AI rows around.
func (r *Repository) DeleteCamera(cameraID string) error {
	if _, err := r.db.Exec(`DELETE FROM ai_status WHERE camera_id = ?`, cameraID); err != nil {
		return err
	}
	if _, err := r.db.Exec(`DELETE FROM ai_frame  WHERE camera_id = ?`, cameraID); err != nil {
		return err
	}
	if _, err := r.db.Exec(`DELETE FROM ai_object WHERE camera_id = ?`, cameraID); err != nil {
		return err
	}
	return nil
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func nullStr(s string) any {
	if s == "" {
		return nil
	}
	return s
}

// parseTime is lenient on purpose: a timestamp written by an older build must
// not turn a metadata read into a 500.
func parseTime(s string) time.Time {
	if t, err := time.Parse(timeFmt, s); err == nil {
		return t
	}
	if t, err := time.Parse(time.RFC3339, s); err == nil {
		return t
	}
	return time.Time{}
}
