import { api } from '@/api/client'
import Hls from 'hls.js'

export type PlayerState =
  | 'idle'
  | 'connecting'
  | 'connected'
  | 'disconnected'
  | 'reconnecting'
  | 'fallback'

export type Transport = 'webrtc' | 'hls' | null

export interface PlayerStatus {
  state: PlayerState
  transport: Transport
  /** Short human-readable reason, shown in the UI so a failure is diagnosable
   *  without opening devtools on a phone. */
  detail: string
  /** True when the browser refused to autoplay and needs a tap. */
  needsTap: boolean
}

// WebRTC is preferred (sub-second), but it is also the fragile one on phones:
// it needs UDP/ICE to reach the server and some mobile browsers refuse it on an
// insecure (http) origin. If it has not produced a picture within this window,
// switch to HLS, which is plain HTTP over TCP and always works over the same
// port that already served the page.
const WEBRTC_TIMEOUT_MS = 7000
const MAX_WEBRTC_ATTEMPTS = 2

export class StreamPlayer {
  private video: HTMLVideoElement
  private cameraId: string
  private pc: RTCPeerConnection | null = null
  private hls: Hls | null = null
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

  /** Switch to HLS explicitly (used by the UI toggle and by the fallback). */
  useHLS(): void {
    this.clearTimers()
    this.teardownPeer()
    this.transport = 'hls'
    this.startHLS()
  }

  /** Go back to WebRTC (UI toggle). */
  useWebRTC(): void {
    this.clearTimers()
    this.teardownHLS()
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
    this.teardownHLS()
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
    if (this.transport === 'hls') {
      return this.hls ? 'fallback' : 'connecting'
    }
    if (this.retry > 0) return 'reconnecting'
    return 'connecting'
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

  // ------------------------------------------------------------------- webrtc

  private async connectWebRTC(): Promise<void> {
    if (this.closed) return
    this.transport = 'webrtc'
    this.detail = this.retry === 0 ? 'negotiating WebRTC' : 'retrying WebRTC #' + this.retry
    this.emit({ state: this.retry === 0 ? 'connecting' : 'reconnecting' })

    // Give up on WebRTC if no frame arrives in time, rather than spinning
    // forever behind a black rectangle.
    this.watchdog = window.setTimeout(() => {
      this.watchdog = null
      if (this.transport === 'webrtc') {
        this.detail = 'WebRTC timed out - no media in ' + WEBRTC_TIMEOUT_MS / 1000 + 's'
        this.teardownPeer()
        this.transport = 'hls'
        this.startHLS()
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

  private onWebRTCFailure(): void {
    if (this.closed) return
    this.teardownPeer()
    this.retry++
    if (this.retry > MAX_WEBRTC_ATTEMPTS) {
      this.clearTimers()
      this.detail += ' - falling back to HLS'
      this.transport = 'hls'
      this.startHLS()
      return
    }
    this.scheduleReconnect()
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

  private clearWatchdog(): void {
    if (this.watchdog !== null) {
      window.clearTimeout(this.watchdog)
      this.watchdog = null
    }
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
  }

  // ---------------------------------------------------------------------- hls

  private async startHLS(): Promise<void> {
    if (this.closed) return
    this.transport = 'hls'
    this.detail = 'loading HLS (HTTP fallback)'
    this.emit({ state: 'fallback' })

    let url = '/hls/' + this.cameraId + '/index.m3u8'
    try {
      // The camera id and the MediaMTX stream path can differ, so ask the API
      // for the canonical same-origin URL.
      const info = await api.cameraStream(this.cameraId)
      if (info.hls_url) url = info.hls_url
    } catch {
      /* fall back to the id-derived URL */
    }

    if (Hls.isSupported()) {
      const hls = new Hls({ enableWorker: true, lowLatencyMode: true })
      this.hls = hls
      hls.on(Hls.Events.MANIFEST_PARSED, () => {
        this.detail = 'HLS playing (HTTP fallback)'
        this.play()
        this.emit({ state: 'fallback' })
      })
      hls.on(Hls.Events.ERROR, (_e, data) => {
        if (!data.fatal) return
        this.detail = 'HLS error: ' + data.type + ' / ' + data.details
        this.emit({ state: 'disconnected' })
        this.scheduleHLSRetry(url)
      })
      hls.loadSource(url)
      hls.attachMedia(this.video)
    } else if (this.video.canPlayType('application/vnd.apple.mpegurl')) {
      // iOS Safari plays HLS natively - and this is exactly the browser where
      // WebRTC over plain http is refused, so this path matters most there.
      this.video.src = url
      this.detail = 'HLS playing via native player'
      this.play()
      this.emit({ state: 'fallback' })
    } else {
      this.detail = 'HLS unsupported by this browser'
      this.emit({ state: 'disconnected' })
    }
  }

  private scheduleHLSRetry(url: string): void {
    if (this.closed || this.timer !== null) return
    this.timer = window.setTimeout(() => {
      this.timer = null
      if (this.transport !== 'hls') return
      this.teardownHLS()
      this.startHLS()
    }, 3000)
  }

  private teardownHLS(): void {
    if (this.hls) {
      this.hls.destroy()
      this.hls = null
    }
    if (this.video.src) {
      this.video.removeAttribute('src')
      this.video.load()
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
