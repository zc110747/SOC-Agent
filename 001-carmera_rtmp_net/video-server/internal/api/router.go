package api

import "net/http"

// Register mounts all /api/* routes onto the provided mux. The server package
// registers the SPA static handler separately; more specific /api patterns take
// precedence over the "/" fallback.
func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/health", h.health)
	mux.HandleFunc("GET /api/cameras", h.listCameras)
	mux.HandleFunc("POST /api/cameras", h.createCamera)
	mux.HandleFunc("GET /api/cameras/{id}", h.getCamera)
	mux.HandleFunc("PUT /api/cameras/{id}", h.updateCamera)
	mux.HandleFunc("DELETE /api/cameras/{id}", h.deleteCamera)
	mux.HandleFunc("GET /api/cameras/{id}/status", h.cameraStatus)
	mux.HandleFunc("GET /api/cameras/{id}/stream", h.cameraStream)
	mux.HandleFunc("POST /api/cameras/{id}/webrtc", h.cameraWebRTC)
}
