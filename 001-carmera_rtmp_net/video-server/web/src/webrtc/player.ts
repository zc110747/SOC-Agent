import { api } from '@/api/client'

export type PlayerState = 'idle' | 'connecting' | 'connected' | 'disconnected' | 'reconnecting'

// WebRTCPlayer plays a camera's stream via MediaMTX's WebRTC endpoint. The SDP
// offer is exchanged through our backend (POST /api/cameras/{id}/webrtc) which
// proxies to MediaMTX, so the browser never talks to MediaMTX directly. If the
// connection drops, it automatically reconnects with backoff.
export class WebRTCPlayer {
  private pc: RTCPeerConnection | null = null
  private video: HTMLVideoElement
  private cameraId: string
  private closed = false
  private retry = 0
  private timer: number | null = null
  onState: ((s: PlayerState) => void) | null = null

  constructor(video: HTMLVideoElement, cameraId: string) {
    this.video = video
    this.cameraId = cameraId
  }

  start(): void {
    this.closed = false
    this.connect()
  }

  private setState(s: PlayerState): void {
    this.onState?.(s)
  }

  private async connect(): Promise<void> {
    if (this.closed) return
    this.setState(this.retry === 0 ? 'connecting' : 'reconnecting')
    try {
      const pc = new RTCPeerConnection({ iceServers: [] })
      this.pc = pc
      pc.addTransceiver('video', { direction: 'recvonly' })
      pc.addTransceiver('audio', { direction: 'recvonly' })

      pc.ontrack = (ev: RTCTrackEvent) => {
        if (ev.track.kind === 'video') {
          this.video.srcObject = ev.streams[0]
        }
      }

      pc.onconnectionstatechange = () => {
        const st = pc.connectionState
        if (st === 'connected') {
          this.retry = 0
          this.setState('connected')
        } else if (st === 'disconnected' || st === 'failed') {
          this.setState('disconnected')
          this.scheduleReconnect()
        }
      }

      const offer = await pc.createOffer()
      await pc.setLocalDescription(offer)
      const { sdp } = await api.webrtcOffer(this.cameraId, offer.sdp || '')
      await pc.setRemoteDescription({ type: 'answer', sdp })
    } catch (err) {
      console.error('webrtc connect failed:', err)
      this.setState('disconnected')
      this.scheduleReconnect()
    }
  }

  private scheduleReconnect(): void {
    if (this.closed || this.timer !== null) return
    this.retry++
    const delay = Math.min(1000 * this.retry, 5000)
    this.timer = window.setTimeout(() => {
      this.timer = null
      this.teardownPeer()
      this.connect()
    }, delay)
  }

  private teardownPeer(): void {
    if (this.pc) {
      try {
        this.pc.close()
      } catch {
        /* ignore */
      }
      this.pc = null
    }
  }

  stop(): void {
    this.closed = true
    if (this.timer !== null) {
      window.clearTimeout(this.timer)
      this.timer = null
    }
    this.teardownPeer()
    this.video.srcObject = null
  }
}
