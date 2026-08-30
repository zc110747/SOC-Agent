// Package server wires the MediaMTX manager, the monitor, the SQLite store and the
// HTTP server (REST API + embedded Web UI) into a single runnable unit.
package server

import (
	"context"
	"database/sql"
	"fmt"
	"io/fs"
	"net/http"
	"strings"

	"video-server/internal/api"
	"video-server/internal/camera"
	"video-server/internal/config"
	"video-server/internal/logger"
	"video-server/internal/mediamtx"
	"video-server/internal/monitor"
	"video-server/web"
)

// Server is the top-level runtime container.
type Server struct {
	cfg        *config.Config
	db         *sql.DB
	repo       *camera.Repository
	mtx        *mediamtx.Manager
	mon        *monitor.Monitor
	httpServer *http.Server
}

func New(cfg *config.Config, db *sql.DB, repo *camera.Repository, mtx *mediamtx.Manager, mon *monitor.Monitor) *Server {
	return &Server{cfg: cfg, db: db, repo: repo, mtx: mtx, mon: mon}
}

// Start launches MediaMTX, the monitor and the HTTP server. It blocks until the
// server stops (error or shutdown).
func (s *Server) Start(ctx context.Context) error {
	if err := s.mtx.Start(ctx); err != nil {
		return fmt.Errorf("start mediamtx: %w", err)
	}
	go s.mon.Run(ctx)

	h := api.New(s.cfg, s.repo, s.mtx, s.db)
	mux := http.NewServeMux()
	h.Register(mux)

	sub, err := fs.Sub(webstatic.Dist, "dist")
	if err != nil {
		logger.Warn("web UI not embedded (run 'npm run build' in web/ first): %v", err)
	} else {
		mux.Handle("/", spaHandler(sub))
	}

	s.httpServer = &http.Server{
		Addr:    fmt.Sprintf(":%d", s.cfg.Server.HTTPPort),
		Handler: mux,
	}
	logger.Info("http server listening on http://%s:%d", s.cfg.Server.Host, s.cfg.Server.HTTPPort)
	if err := s.httpServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		return err
	}
	return nil
}

// Shutdown gracefully stops the HTTP server, MediaMTX and closes the database.
func (s *Server) Shutdown(ctx context.Context) {
	if s.httpServer != nil {
		_ = s.httpServer.Shutdown(ctx)
	}
	s.mtx.Stop()
	if s.db != nil {
		_ = s.db.Close()
	}
}

// spaHandler serves embedded static files and falls back to index.html for
// client-side routes (e.g. /camera/camera01) so the Vue router can handle them.
func spaHandler(fsys fs.FS) http.Handler {
	fileServer := http.FileServer(http.FS(fsys))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := strings.TrimPrefix(r.URL.Path, "/")
		if p == "" {
			p = "index.html"
		}
		if _, err := fs.Stat(fsys, p); err != nil {
			data, e := fs.ReadFile(fsys, "index.html")
			if e != nil {
				http.NotFound(w, r)
				return
			}
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			_, _ = w.Write(data)
			return
		}
		fileServer.ServeHTTP(w, r)
	})
}
