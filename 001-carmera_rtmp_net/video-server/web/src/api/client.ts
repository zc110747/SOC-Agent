export type CameraStatus = 'online' | 'offline' | 'connecting' | 'error'

export interface Camera {
  id: string
  name: string
  stream_path: string
  device_ip?: string
  status: CameraStatus
  resolution?: string
  fps: number
  bitrate: number
  created_at: string
  updated_at: string
  last_seen?: string
  rtsp_url?: string
}

export interface Health {
  status: string
  database: string
  media_server: string
}

export interface StreamInfo {
  id: string
  name: string
  stream_path: string
  status: CameraStatus
  rtsp_url: string
  resolution?: string
  fps: number
  bitrate: number
  webrtc: {
    signaling: string
    path: string
  }
  /** Same-origin HLS playlist, proxied by the server (fallback for phones). */
  hls_url?: string
  /** Direct MediaMTX HLS URL, for debugging with an external player. */
  hls_direct_url?: string
}

const BASE = '/api'

async function getJSON<T>(path: string): Promise<T> {
  const res = await fetch(BASE + path)
  if (!res.ok) throw new Error(await res.text())
  return res.json() as Promise<T>
}

async function sendJSON<T>(path: string, method: string, body?: unknown): Promise<T> {
  const res = await fetch(BASE + path, {
    method,
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined
  })
  if (!res.ok) throw new Error(await res.text())
  return res.json() as Promise<T>
}

export const api = {
  health: () => getJSON<Health>('/health'),
  listCameras: () => getJSON<Camera[]>('/cameras'),
  getCamera: (id: string) => getJSON<Camera>('/cameras/' + id),
  createCamera: (c: Partial<Camera>) => sendJSON<Camera>('/cameras', 'POST', c),
  updateCamera: (id: string, c: Partial<Camera>) => sendJSON<Camera>('/cameras/' + id, 'PUT', c),
  deleteCamera: (id: string) => sendJSON<void>('/cameras/' + id, 'DELETE'),
  cameraStatus: (id: string) => getJSON<{ id: string; status: CameraStatus }>('/cameras/' + id + '/status'),
  cameraStream: (id: string) => getJSON<StreamInfo>('/cameras/' + id + '/stream'),
  webrtcOffer: (id: string, sdp: string) =>
    sendJSON<{ type: string; sdp: string }>('/cameras/' + id + '/webrtc', 'POST', { sdp })
}
