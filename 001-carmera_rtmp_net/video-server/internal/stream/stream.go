// Package stream is the Stream Manager: it presents the live MediaMTX paths in a
// server-friendly shape and is consumed by the monitor (auto-discovery/status) and
// the API (stream metadata).
package stream

import (
	"context"

	"video-server/internal/mediamtx"
)

// Stream is a normalized view of a MediaMTX path.
type Stream struct {
	Path       string
	State      string
	SourceType string
	HasSource  bool
	Ready      bool
}

// Manager lists live streams from MediaMTX.
type Manager struct {
	mtx *mediamtx.Manager
}

func New(m *mediamtx.Manager) *Manager {
	return &Manager{mtx: m}
}

// List returns the currently active/known streams.
func (m *Manager) List(ctx context.Context) ([]Stream, error) {
	list, err := m.mtx.PathsList(ctx)
	if err != nil {
		return nil, err
	}
	out := make([]Stream, 0, len(list.Items))
	for _, p := range list.Items {
		has := p.Source != nil
		state := "offline"
		if p.Online {
			state = "online"
		} else if has {
			state = "connecting"
		}
		out = append(out, Stream{
			Path:       p.Name,
			State:      state,
			SourceType: sourceType(p),
			HasSource:  has,
			Ready:      p.Online,
		})
	}
	return out, nil
}

func sourceType(p mediamtx.Path) string {
	if p.Source == nil {
		return ""
	}
	return p.Source.Type
}
