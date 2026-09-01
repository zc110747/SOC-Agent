// Package mediamtx manages the MediaMTX subprocess: it launches MediaMTX with a
// generated config, watches its health, polls the control API for path discovery,
// and proxies WebRTC signaling offers to MediaMTX's WebRTC endpoint.
package mediamtx

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"time"

	"video-server/internal/config"
	"video-server/internal/logger"
)

// Path mirrors the relevant fields of a MediaMTX v3 API path object.
type Path struct {
	Name      string `json:"name"`
	Online    bool   `json:"online"`
	Source    *struct {
		Type string `json:"type"`
		ID   string `json:"id"`
	} `json:"source"`
	ReadyTime string `json:"readyTime"`
	// Tracks2 carries per-track codec properties. This is the only place the
	// resolution actually negotiated by the publisher is exposed: an RTSP
	// publisher (the Camera Agent) never reports it through the control API,
	// so we read it back from the media track instead.
	Tracks2 []struct {
		Codec      string         `json:"codec"`
		CodecProps map[string]any `json:"codecProps"`
	} `json:"tracks2"`
	// BytesReceived is the cumulative inbound byte count for the path. The
	// monitor samples it across scans to derive a live bitrate, which the
	// publisher never reports on its own.
	BytesReceived int64 `json:"bytesReceived"`
}

// PathsListResponse is the response of GET /v3/paths/list.
type PathsListResponse struct {
	Items []Path `json:"items"`
}

// Manager controls the MediaMTX lifecycle.
type Manager struct {
	cfg            *config.Config
	cmd            *exec.Cmd
	generatedPath  string
	apiBase        string
	webrtcBase     string
	httpClient     *http.Client
}

func New(cfg *config.Config) *Manager {
	return &Manager{
		cfg:        cfg,
		apiBase:    "http://" + cfg.APIAddr(),
		webrtcBase: "http://" + cfg.WebRTCAPIAddr(),
		httpClient: &http.Client{Timeout: 5 * time.Second},
	}
}

// resolveBinary returns an existing binary path, appending .exe on Windows when
// needed, so a single cross-platform config value works on both platforms.
func resolveBinary(p string) string {
	if _, err := os.Stat(p); err == nil {
		return p
	}
	if runtime.GOOS == "windows" {
		if _, err := os.Stat(p + ".exe"); err == nil {
			return p + ".exe"
		}
	}
	return p
}

// GenerateConfig writes the MediaMTX YAML derived from our Config so that ports
// stay the single source of truth. It is regenerated on every start. The file is
// written to a unique temp name inside the configured config directory so that
// (a) it never clobbers a checked-in config file and (b) repeated starts never
// try to overwrite a pre-existing file (which some sandboxes forbid).
func (m *Manager) GenerateConfig() error {
	body := fmt.Sprintf(`logLevel: info
api: true
apiAddress: :%d
rtsp: true
rtspAddress: :%d
rtpAddress: :8000
webrtc: true
webrtcAddress: :%d
hls: true
hlsAddress: :%d
playback: false
record: false
paths:
  all_others:
    source: publisher
`, m.cfg.MediaMTX.APIPort, m.cfg.RTSP.Port, m.cfg.WebRTC.Port, m.cfg.MediaMTX.HLSPort)

	dir := dirOf(m.cfg.MediaMTX.Config)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Errorf("create config dir: %w", err)
	}
	f, err := os.CreateTemp(dir, "mediamtx-*.yml")
	if err != nil {
		return fmt.Errorf("write mediamtx config: %w", err)
	}
	if _, err := f.WriteString(body); err != nil {
		f.Close()
		return fmt.Errorf("write mediamtx config: %w", err)
	}
	if err := f.Close(); err != nil {
		return fmt.Errorf("write mediamtx config: %w", err)
	}
	m.generatedPath = f.Name()
	logger.Debug("mediamtx config written to %s", m.generatedPath)
	return nil
}

// Start launches MediaMTX and waits (briefly) for its API to become ready.
func (m *Manager) Start(ctx context.Context) error {
	if err := m.GenerateConfig(); err != nil {
		return err
	}
	binary := resolveBinary(m.cfg.MediaMTX.Binary)
	if _, err := os.Stat(binary); err != nil {
		return fmt.Errorf("mediamtx binary not found at %q (set mediamtx.binary in config)", binary)
	}
	cmd := exec.Command(binary, m.generatedPath)
	cmd.Stdout = &logWriter{}
	cmd.Stderr = &logWriter{}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("start mediamtx: %w", err)
	}
	m.cmd = cmd
	logger.Info("mediamtx started (binary=%s config=%s)", binary, m.cfg.MediaMTX.Config)

	if !m.waitReady(ctx, 12*time.Second) {
		logger.Warn("mediamtx api not reachable within timeout; continuing")
	}
	return nil
}

// Stop terminates the MediaMTX subprocess.
func (m *Manager) Stop() {
	if m.cmd == nil || m.cmd.Process == nil {
		return
	}
	_ = m.cmd.Process.Kill()
	_, _ = m.cmd.Process.Wait()
	m.cmd = nil
	if m.generatedPath != "" {
		_ = os.Remove(m.generatedPath)
		m.generatedPath = ""
	}
	logger.Info("mediamtx stopped")
}

func (m *Manager) waitReady(ctx context.Context, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if m.Health() {
			return true
		}
		select {
		case <-ctx.Done():
			return false
		case <-time.After(300 * time.Millisecond):
		}
	}
	return false
}

// Health reports whether the MediaMTX control API responds.
func (m *Manager) Health() bool {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, m.apiBase+"/v3/paths/list", nil)
	if err != nil {
		return false
	}
	resp, err := m.httpClient.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	return resp.StatusCode == http.StatusOK
}

// PathsList returns the current MediaMTX paths (publishers/streams).
func (m *Manager) PathsList(ctx context.Context) (*PathsListResponse, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, m.apiBase+"/v3/paths/list?itemsPerPage=1000", nil)
	if err != nil {
		return nil, err
	}
	resp, err := m.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("paths/list returned status %d", resp.StatusCode)
	}
	var out PathsListResponse
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, fmt.Errorf("decode paths/list: %w", err)
	}
	return &out, nil
}

// WebRTCOffer forwards a browser SDP offer to MediaMTX's WebRTC read endpoint and
// returns the SDP answer. This keeps browser<->mediamtx signaling server-side so
// the WebRTC port need not be CORS-exposed.
func (m *Manager) WebRTCOffer(ctx context.Context, streamPath, offer string) (string, error) {
	// MediaMTX exposes WebRTC read (play) via the WHEP endpoint
	// (POST /<path>/whep) on its WebRTC port. It answers with 201 Created
	// and the SDP answer in the body.
	url := fmt.Sprintf("%s/%s/whep", m.webrtcBase, streamPath)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewBufferString(offer))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/sdp")
	resp, err := m.httpClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusCreated {
		b, _ := io.ReadAll(resp.Body)
		return "", fmt.Errorf("webrtc signaling status %d: %s", resp.StatusCode, string(b))
	}
	buf := new(bytes.Buffer)
	if _, err := buf.ReadFrom(resp.Body); err != nil {
		return "", err
	}
	return buf.String(), nil
}

// logWriter adapts MediaMTX's stdout/stderr to our leveled logger, line by line.
type logWriter struct{ buf []byte }

func (w *logWriter) Write(p []byte) (int, error) {
	w.buf = append(w.buf, p...)
	for {
		idx := bytes.IndexByte(w.buf, '\n')
		if idx < 0 {
			break
		}
		line := strings.TrimRight(string(w.buf[:idx]), "\r")
		w.buf = w.buf[idx+1:]
		if line != "" {
			logger.Info("[mediamtx] %s", line)
		}
	}
	return len(p), nil
}

func dirOf(p string) string {
	if i := strings.LastIndexAny(p, `/\`); i >= 0 {
		return p[:i]
	}
	return "."
}

// LocalIP returns the first non-loopback IPv4, used to build LAN-facing URLs.
func LocalIP() string {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return "127.0.0.1"
	}
	for _, a := range addrs {
		if ipnet, ok := a.(*net.IPNet); ok && !ipnet.IP.IsLoopback() {
			if ip4 := ipnet.IP.To4(); ip4 != nil {
				return ip4.String()
			}
		}
	}
	return "127.0.0.1"
}
