import { api } from '@/api/client'

export type PlayerState =
  | 'idle'
  | 'connecting'
  | 'connected'
  | 'disconnected'
  | 'reconnecting'

export type Transport = 'webrtc' | null

export interface PlayerStatus {
  state: PlayerState
  transport: Transport
  /** Short human-readable reason, shown in the UI so a failure is diagnosable
   *  without opening devtools on a phone. */
  detail: string
  /** True when the browser refused to autoplay and needs a tap. */
  needsTap: boolean
}

// WebRTC is the ONLY live transport. It is sub-second; HLS is intentionally NOT
// used for live viewing (seconds of latency, no value for real-time monitoring)
// and is kept server-side only for a future cached-recording playback feature.
// If WebRTC has not produced a picture within this window, report the failure
// instead of silently degrading to a high-latency stream.
const WEBRTC_TIMEOUT_MS = 7000
// Limited reconnect for transient blips (network flap / ICE restart). On
// exhaustion the player stays in 'disconnected' with the error visible - no
// hidden fallback.
const MAX_WEBRTC_RETRIES = 2

export class StreamPlayer {
  private video: HTMLVideoElement
  private cameraId: string
  private pc: RTCPeerConnection | null = null
  private closed = false
  private retry = 0
  private timer: number | null = null
  private watchdog: number | null = null
  private transport: Transport = null
  private needsTap = false
  private detail = ''
  onStatus: ((s: PlayerStatus) => void) | null = null

  constructor(video: HTMLVideoElement, cameraId: string) {
    this.video = video
    this.cameraId = cameraId
  }

  start(): void {
    this.closed = false
    this.connectWebRTC()
  }

  /** Manual reconnect (UI button). Also used to recover from a shown error. */
  useWebRTC(): void {
    this.clearTimers()
    this.teardownPeer()
    this.retry = 0
    this.connectWebRTC()
  }

  /** Called on a user tap when autoplay was blocked. */
  resume(): void {
    this.video.muted = true
    void this.video.play().then(
      () => {
        this.needsTap = false
        this.emit()
      },
      () => {}
    )
  }

  stop(): void {
    this.closed = true
    this.clearTimers()
    this.teardownPeer()
    this.video.srcObject = null
  }

  // ---------------------------------------------------------------- internals

  private emit(extra?: Partial<PlayerStatus>): void {
    this.onStatus?.({
      state: this.currentState(),
      transport: this.transport,
      detail: this.detail,
      needsTap: this.needsTap,
      ...extra
    })
  }

  private currentState(): PlayerState {
    if (this.retry > 0 && this.transport === 'webrtc') return 'reconnecting'
    if (this.transport === 'webrtc' && this.pc) return 'connected'
    if (this.transport === 'webrtc') return 'connecting'
    return 'disconnected'
  }

  private clearTimers(): void {
    if (this.timer !== null) {
      window.clearTimeout(this.timer)
      this.timer = null
    }
    if (this.watchdog !== null) {
      window.clearTimeout(this.watchdog)
      this.watchdog = null
    }
  }

  /** Cancel only the "no media in WEBRTC_TIMEOUT_MS" watchdog. Used when the
   *  connection actually established (connectionState 'connected') so we don't
   *  spuriously fail a working stream. Does NOT touch the reconnect timer. */
  private clearWatchdog(): void {
    if (this.watchdog !== null) {
      window.clearTimeout(this.watchdog)
      this.watchdog = null
    }
  }

  // ------------------------------------------------------------------- webrtc

  private async connectWebRTC(): Promise<void> {
    if (this.closed) return
    this.transport = 'webrtc'
    // Start from a clean <video>: drop any stale src / srcObject so the WebRTC
    // stream we assign on ontrack is the only thing driving it.
    if (this.video.src) {
      this.video.removeAttribute('src')
      this.video.load()
    }
    this.video.srcObject = null
    this.detail = this.retry === 0 ? 'negotiating WebRTC' : 'retrying WebRTC #' + this.retry
    this.emit({ state: this.retry === 0 ? 'connecting' : 'reconnecting' })

    // Give up on WebRTC if no frame arrives in time, rather than spinning
    // forever behind a black rectangle. Report the failure (no HLS fallback).
    this.watchdog = window.setTimeout(() => {
      this.watchdog = null
      if (this.transport === 'webrtc') {
        this.detail = 'WebRTC timed out - no media in ' + WEBRTC_TIMEOUT_MS / 1000 + 's'
        this.onWebRTCFailure()
      }
    }, WEBRTC_TIMEOUT_MS)

    try {
      const pc = new RTCPeerConnection({ iceServers: [] })
      this.pc = pc
      pc.addTransceiver('video', { direction: 'recvonly' })
      pc.addTransceiver('audio', { direction: 'recvonly' })

      pc.ontrack = (ev: RTCTrackEvent) => {
        if (ev.track.kind === 'video') {
          this.video.srcObject = ev.streams[0]
          this.play()
        }
      }

      pc.onconnectionstatechange = () => {
        const st = pc.connectionState
        if (st === 'connected') {
          this.retry = 0
          this.clearWatchdog()
          this.detail = 'WebRTC connected'
          this.emit({ state: 'connected' })
        } else if (st === 'failed') {
          this.detail = 'WebRTC ICE failed (UDP to server unreachable?)'
          this.onWebRTCFailure()
        } else if (st === 'disconnected') {
          this.detail = 'WebRTC disconnected'
          this.onWebRTCFailure()
        }
      }

      pc.oniceconnectionstatechange = () => {
        if (pc.iceConnectionState === 'failed') {
          this.detail = 'ICE failed - no candidate pair (UDP blocked)'
          this.onWebRTCFailure()
        }
      }

      const offer = await pc.createOffer()
      await pc.setLocalDescription(offer)
      const { sdp } = await api.webrtcOffer(this.cameraId, offer.sdp || '')
      await pc.setRemoteDescription({ type: 'answer', sdp })
    } catch (err) {
      this.detail = 'WebRTC error: ' + describe(err)
      this.onWebRTCFailure()
    }
  }

  // WebRTC failure is reported directly - there is no HLS fallback. A few
  // reconnects cover transient blips; once exhausted the error stays visible
  // (state 'disconnected') and the user can retry via the WebRTC button.
  private onWebRTCFailure(): void {
    if (this.closed) return
    this.teardownPeer()
    this.retry++
    if (this.retry <= MAX_WEBRTC_RETRIES) {
      this.scheduleReconnect()
      return
    }
    this.transport = null
    this.detail += ' - WebRTC unavailable (no fallback transport)'
    this.emit({ state: 'disconnected' })
  }

  private scheduleReconnect(): void {
    if (this.closed || this.timer !== null) return
    const delay = Math.min(1000 * this.retry, 4000)
    this.emit({ state: 'reconnecting' })
    this.timer = window.setTimeout(() => {
      this.timer = null
      if (this.transport === 'webrtc') this.connectWebRTC()
    }, delay)
  }

  private teardownPeer(): void {
    if (this.pc) {
      try {
        this.pc.ontrack = null
        this.pc.onconnectionstatechange = null
        this.pc.oniceconnectionstatechange = null
        this.pc.close()
      } catch {
        /* ignore */
      }
      this.pc = null
    }
    if (this.video.srcObject) {
      this.video.srcObject = null
    }
  }

  // -------------------------------------------------------------------- media

  private play(): void {
    // Mobile browsers can refuse play() even on a muted inline video (low power
    // mode, data saver). Surface it as "tap to play" instead of a black box.
    const p = this.video.play()
    if (p && typeof p.catch === 'function') {
      p.then(
        () => {
          this.needsTap = false
        },
        (err: unknown) => {
          this.needsTap = true
          this.detail = 'autoplay blocked - tap to play (' + describe(err) + ')'
          this.emit()
        }
      )
    }
  }
}

function describe(err: unknown): string {
  if (err instanceof Error) return err.message
  return String(err)
}
