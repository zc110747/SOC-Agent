// Command video-server is the single binary entrypoint for the central video
// server: it starts the database, MediaMTX, the camera monitor, the REST API and
// the embedded Web UI.
package main

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"

	"video-server/internal/camera"
	"video-server/internal/config"
	"video-server/internal/database"
	"video-server/internal/logger"
	"video-server/internal/mediamtx"
	"video-server/internal/monitor"
	"video-server/internal/server"
)

func main() {
	cfgPath := "config/config.yaml"
	if len(os.Args) > 1 {
		cfgPath = os.Args[1]
	}

	cfg, err := config.Load(cfgPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "config error: %v\n", err)
		os.Exit(1)
	}

	logger.SetLevel(cfg.Log.Level)
	if cfg.Log.File != "" {
		f, ferr := os.OpenFile(cfg.Log.File, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
		if ferr == nil {
			logger.SetOutput(io.MultiWriter(os.Stdout, f))
			defer f.Close()
		} else {
			logger.Warn("cannot open log file %s: %v", cfg.Log.File, ferr)
		}
	}

	logger.Info("video-server starting (config=%s)", cfgPath)

	db, err := database.Open(cfg.Database.Path)
	if err != nil {
		logger.Error("database open failed: %v", err)
		os.Exit(1)
	}
	repo := camera.NewRepository(db)
	mtx := mediamtx.New(cfg)
	mon := monitor.New(repo, mtx, cfg)
	srv := server.New(cfg, db, repo, mtx, mon)

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	go func() {
		if err := srv.Start(ctx); err != nil {
			logger.Error("server error: %v", err)
			cancel()
		}
	}()

	logger.Info("video-server running; press Ctrl+C to stop")
	<-ctx.Done()

	logger.Info("shutting down...")
	srv.Shutdown(context.Background())
	logger.Info("video-server stopped")
}
