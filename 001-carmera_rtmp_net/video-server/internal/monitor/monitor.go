// Package monitor periodically polls MediaMTX and keeps the camera table in
// sync: it auto-registers streams that have an active publisher and flips
// cameras to offline when no data has been seen for a timeout.
package monitor

import (
	"context"
	"time"

	"video-server/internal/camera"
	"video-server/internal/config"
	"video-server/internal/logger"
	"video-server/internal/mediamtx"
	"video-server/internal/stream"
)

type Monitor struct {
	repo           *camera.Repository
	streams        *stream.Manager
	cfg            *config.Config
	interval       time.Duration
	offlineTimeout time.Duration
}

func New(repo *camera.Repository, mtx *mediamtx.Manager, cfg *config.Config) *Monitor {
	return &Monitor{
		repo:           repo,
		streams:        stream.New(mtx),
		cfg:            cfg,
		interval:       3 * time.Second,
		offlineTimeout: 10 * time.Second,
	}
}

// Run scans immediately and then on a ticker until ctx is cancelled.
func (m *Monitor) Run(ctx context.Context) {
	ticker := time.NewTicker(m.interval)
	defer ticker.Stop()
	m.scan(ctx)
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			m.scan(ctx)
		}
	}
}

func (m *Monitor) scan(ctx context.Context) {
	streams, err := m.streams.List(ctx)
	if err != nil {
		logger.Warn("monitor: stream list failed: %v", err)
		return
	}
	now := time.Now()
	for _, p := range streams {
		// Only consider paths that actually have a publisher (a Camera Agent
		// pushing). Idle paths (no source) are skipped to avoid phantom cameras.
		if !p.HasSource {
			continue
		}
		// online (publisher present and serving) -> ONLINE, otherwise OFFLINE.
		// A path whose publisher dropped stays listed by MediaMTX with
		// source set but online=false, so we surface it as OFFLINE here.
		status := camera.StatusOffline
		if p.Ready {
			status = camera.StatusOnline
		}
		if _, err := m.repo.UpsertByStreamPath(p.Path, status, now); err != nil {
			logger.Error("monitor: upsert %s failed: %v", p.Path, err)
			continue
		}
		logger.Debug("monitor: %s -> %s", p.Path, status)
	}
	// Any camera not seen within the offline timeout is marked offline.
	if err := m.repo.MarkOfflineIfNotSeen(now.Add(-m.offlineTimeout)); err != nil {
		logger.Error("monitor: mark offline failed: %v", err)
	}
}
