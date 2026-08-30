package config

import (
	"fmt"
	"os"

	"gopkg.in/yaml.v3"
)

// Config is the top-level Video Server configuration loaded from config/config.yaml.
type Config struct {
	Server   ServerConfig   `yaml:"server"`
	RTSP     RTSPConfig     `yaml:"rtsp"`
	WebRTC   WebRTCConfig   `yaml:"webrtc"`
	Database DatabaseConfig `yaml:"database"`
	MediaMTX MediaMTXConfig `yaml:"mediamtx"`
	Log      LogConfig      `yaml:"log"`
}

type ServerConfig struct {
	Host     string `yaml:"host"`
	HTTPPort int    `yaml:"http_port"`
}

type RTSPConfig struct {
	Port int `yaml:"port"`
}

type WebRTCConfig struct {
	Port int `yaml:"port"`
}

type DatabaseConfig struct {
	Path string `yaml:"path"`
}

// MediaMTXConfig points to the MediaMTX binary and the generated config file.
// APIPort is the MediaMTX control API port (used for camera discovery).
// HLSPort is only used to build optional HLS URLs.
type MediaMTXConfig struct {
	Binary  string `yaml:"binary"`
	Config  string `yaml:"config"`
	APIPort int    `yaml:"api_port"`
	HLSPort int    `yaml:"hls_port"`
}

type LogConfig struct {
	Level string `yaml:"level"`
	File  string `yaml:"file"`
}

// Default returns a Config populated with sensible defaults. Any field present
// in the YAML file overrides the corresponding default.
func Default() *Config {
	return &Config{
		Server:   ServerConfig{Host: "localhost", HTTPPort: 8080},
		RTSP:     RTSPConfig{Port: 8554},
		WebRTC:   WebRTCConfig{Port: 8889},
		Database: DatabaseConfig{Path: "data/video.db"},
		MediaMTX: MediaMTXConfig{Binary: "./mediamtx/mediamtx", Config: "./config/mediamtx.yml", APIPort: 9997, HLSPort: 8888},
		Log:      LogConfig{Level: "info"},
	}
}

// Load reads the YAML file at path. If the file does not exist the defaults are
// returned (so the server can still boot). Missing keys keep their default value
// because defaults are applied before unmarshalling.
func Load(path string) (*Config, error) {
	cfg := Default()
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return cfg, nil
		}
		return nil, fmt.Errorf("read config %q: %w", path, err)
	}
	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config %q: %w", path, err)
	}
	applyDefaults(cfg)
	return cfg, nil
}

func applyDefaults(cfg *Config) {
	if cfg.Server.HTTPPort == 0 {
		cfg.Server.HTTPPort = 8080
	}
	if cfg.Server.Host == "" {
		cfg.Server.Host = "localhost"
	}
	if cfg.RTSP.Port == 0 {
		cfg.RTSP.Port = 8554
	}
	if cfg.WebRTC.Port == 0 {
		cfg.WebRTC.Port = 8889
	}
	if cfg.Database.Path == "" {
		cfg.Database.Path = "data/video.db"
	}
	if cfg.MediaMTX.Binary == "" {
		cfg.MediaMTX.Binary = "./mediamtx/mediamtx"
	}
	if cfg.MediaMTX.Config == "" {
		cfg.MediaMTX.Config = "./config/mediamtx.yml"
	}
	if cfg.MediaMTX.APIPort == 0 {
		cfg.MediaMTX.APIPort = 9997
	}
	if cfg.MediaMTX.HLSPort == 0 {
		cfg.MediaMTX.HLSPort = 8888
	}
	if cfg.Log.Level == "" {
		cfg.Log.Level = "info"
	}
}

// RTSPURL builds the public RTSP URL for a stream path.
func (c *Config) RTSPURL(path string) string {
	return fmt.Sprintf("rtsp://%s:%d/%s", c.Server.Host, c.RTSP.Port, path)
}

// APIAddr returns the MediaMTX control API address the server talks to (loopback).
func (c *Config) APIAddr() string {
	return fmt.Sprintf("127.0.0.1:%d", c.MediaMTX.APIPort)
}

// WebRTCAPIAddr returns the MediaMTX WebRTC signaling address (loopback).
func (c *Config) WebRTCAPIAddr() string {
	return fmt.Sprintf("127.0.0.1:%d", c.WebRTC.Port)
}
