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
  /** Same-origin HLS playlist, proxied by the server. Reserved for a future
   *  cached-recording playback feature - NOT used by the live player (live
   *  viewing is WebRTC only; HLS adds seconds of latency). */
  hls_url?: string
  /** Direct MediaMTX HLS URL, for debugging / future playback with an external player. */
  hls_direct_url?: string
}

// ---- AI metadata (boxes drawn on the video overlay) -------------------------

export interface MetadataObject {
  class: string
  confidence: number
  track_id: number
  /** [x1, y1, x2, y2] in ORIGINAL video pixels. */
  bbox: [number, number, number, number]
  /** Pose landmarks [x, y, conf] in ORIGINAL video pixels. Present only for
   *  pose models; detection-only output omits it. */
  keypoints?: [number, number, number][]
}

export interface MetadataFrame {
  frame_id: number
  timestamp: number
  video_width: number
  video_height: number
  object_count: number
  received_at: string
  objects: MetadataObject[]
}

export interface MetadataStatus {
  enable: boolean
  running: boolean
  fps: number
  model: string
  tracker: string
  last_frame_id: number
  last_timestamp: number
  processed: number
  wall_clock: number
  received_at: string
}

/** Everything the overlay needs for one camera in a single call. */
export interface MetadataSnapshot {
  camera_id: string
  frame?: MetadataFrame
  status?: MetadataStatus
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
    sendJSON<{ type: string; sdp: string }>('/cameras/' + id + '/webrtc', 'POST', { sdp }),
  cameraMetadata: (id: string) => getJSON<MetadataSnapshot>('/cameras/' + id + '/metadata')
}
