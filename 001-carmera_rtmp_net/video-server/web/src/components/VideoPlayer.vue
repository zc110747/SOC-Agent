<template>
  <div class="player-wrap">
    <video ref="videoEl" autoplay playsinline muted></video>

    <!-- AI tracking-box overlay. Pointer-events disabled so the WebRTC /
         fullscreen controls above stay clickable. The canvas is drawn from the
         latest GET /api/cameras/{id}/metadata frame, scaled to the displayed
         video size. -->
    <canvas ref="canvasEl" class="box-overlay"></canvas>

    <!-- Autoplay can be refused on phones (low power / data saver). Without a
         visible affordance the symptom is just "a black rectangle". -->
    <button v-if="status.needsTap" class="tap-overlay" @click="player?.resume()">
      <span>TAP TO PLAY</span>
    </button>

    <div class="player-status">
      <span class="transport">{{ transportLabel }}</span>
      <span class="state">{{ stateText }}</span>
      <span v-if="aiOn" class="ai-badge">AI {{ objectCount }}</span>
    </div>

    <div class="player-tools">
      <button class="tool-btn" :class="{ active: status.transport === 'webrtc' }" @click="player?.useWebRTC()" title="Reconnect via WebRTC">
        WebRTC
      </button>
      <button class="tool-btn" @click="fullscreen" title="Fullscreen">
        <el-icon><FullScreen /></el-icon>
      </button>
    </div>

    <div v-if="status.detail" class="player-detail">{{ status.detail }}</div>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive, ref } from 'vue'
import { FullScreen } from '@element-plus/icons-vue'
import { StreamPlayer, type PlayerStatus } from '@/webrtc/player'
import { api, type MetadataSnapshot } from '@/api/client'

const props = defineProps<{ cameraId: string }>()
const videoEl = ref<HTMLVideoElement | null>(null)
const canvasEl = ref<HTMLCanvasElement | null>(null)
let player: StreamPlayer | null = null
let metaTimer: number | null = null

const status = reactive<PlayerStatus>({
  state: 'idle',
  transport: null,
  detail: '',
  needsTap: false
})

const aiOn = ref(false)
const objectCount = ref(0)

const stateText = computed(() => {
  switch (status.state) {
    case 'connected':
      return 'CONNECTED'
    case 'connecting':
      return 'CONNECTING'
    case 'reconnecting':
      return 'RECONNECTING'
    case 'disconnected':
      return 'DISCONNECTED'
    default:
      return 'IDLE'
  }
})

// Naming the transport in the UI makes a failure debuggable at a glance. Live
// viewing is WebRTC only - there is no HLS fallback, so a problem shows up as a
// WEBRTC error rather than silently dropping to a high-latency stream.
const transportLabel = computed(() => {
  if (status.transport === 'webrtc') return 'WEBRTC'
  return '—'
})

function fullscreen() {
  videoEl.value?.requestFullscreen?.()
}

// ---- AI metadata overlay ------------------------------------------------

// Poll the metadata endpoint and repaint the box overlay. The metadata frame
// carries bbox in ORIGINAL video pixels; we scale by the video element's
// actual rendered size so the boxes track the displayed stream regardless of
// layout. Missing frame / 0 detected objects simply clears the canvas.
const META_INTERVAL_MS = 200

async function pollMetadata() {
  const canvas = canvasEl.value
  const video = videoEl.value
  if (!canvas || !video) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return

  let snap: MetadataSnapshot | undefined
  try {
    snap = await api.cameraMetadata(props.cameraId)
  } catch {
    // Metadata disabled / not ready yet - keep the overlay empty.
    clearOverlay(canvas, ctx)
    aiOn.value = false
    objectCount.value = 0
    return
  }

  const vw = video.videoWidth
  const vh = video.videoHeight
  if (!snap.frame || vw === 0 || vh === 0 || snap.frame.objects.length === 0) {
    clearOverlay(canvas, ctx)
    aiOn.value = !!snap.status?.running
    objectCount.value = snap.frame?.object_count ?? 0
    return
  }

  const cw = video.clientWidth
  const ch = video.clientHeight
  const dpr = window.devicePixelRatio || 1
  const bw = Math.max(1, Math.round(cw * dpr))
  const bh = Math.max(1, Math.round(ch * dpr))
  if (canvas.width !== bw || canvas.height !== bh) {
    canvas.width = bw
    canvas.height = bh
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, cw, ch)

  const sx = cw / vw
  const sy = ch / vh
  ctx.lineWidth = 2
  ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace'
  ctx.textBaseline = 'alphabetic'

  for (const o of snap.frame.objects) {
    const [x1, y1, x2, y2] = o.bbox
    const rx = x1 * sx
    const ry = y1 * sy
    const rw = (x2 - x1) * sx
    const rh = (y2 - y1) * sy

    ctx.strokeStyle = 'rgba(0, 220, 200, 0.95)'
    ctx.lineWidth = 2
    ctx.strokeRect(rx, ry, rw, rh)

    const label = `${o.class} ${(o.confidence * 100).toFixed(0)}%`
    const padX = 4
    const lh = 16
    const tw = ctx.measureText(label).width
    const ly = ry - lh
    ctx.fillStyle = 'rgba(0, 0, 0, 0.55)'
    ctx.fillRect(rx, ly, tw + padX * 2, lh)
    ctx.fillStyle = '#e6f7f4'
    ctx.fillText(label, rx + padX, ly + lh - 4)

    if (o.track_id > 0) {
      const tid = '#' + o.track_id
      ctx.fillStyle = 'rgba(0, 0, 0, 0.45)'
      ctx.fillText(tid, rx + rw - ctx.measureText(tid).width - padX, ry + lh - 4)
    }

    // Pose skeleton (drawn over the box). Keypoints are [x, y, conf] in the
    // same original video pixels as the bbox, so the same scale applies.
    if (o.keypoints && o.keypoints.length >= 2) {
      drawSkeleton(ctx, o.keypoints, sx, sy)
    }
  }

  aiOn.value = !!snap.status?.running
  objectCount.value = snap.frame.objects.length
}

function clearOverlay(canvas: HTMLCanvasElement, ctx: CanvasRenderingContext2D) {
  if (canvas.width === 0 || canvas.height === 0) return
  ctx.clearRect(0, 0, canvas.width, canvas.height)
}

// COCO 17-keypoint skeleton (0-based indices). yolo11n-pose emits COCO
// landmarks, so this is the topology used to connect them on the overlay.
const SKELETON: [number, number][] = [
  [15, 13], [13, 11], [16, 14], [14, 12], [11, 12],
  [5, 11], [6, 12], [5, 6], [5, 7], [6, 8],
  [7, 9], [8, 10], [1, 2], [0, 1], [0, 2],
  [1, 3], [2, 4], [3, 5], [4, 6]
]
const KP_MIN_CONF = 0.3

function drawSkeleton(
  ctx: CanvasRenderingContext2D,
  kps: [number, number, number][],
  sx: number,
  sy: number
) {
  ctx.lineWidth = 2
  ctx.strokeStyle = 'rgba(0, 220, 200, 0.85)'
  for (const [a, b] of SKELETON) {
    if (a >= kps.length || b >= kps.length) continue
    const ka = kps[a]
    const kb = kps[b]
    if (ka[2] < KP_MIN_CONF || kb[2] < KP_MIN_CONF) continue
    ctx.beginPath()
    ctx.moveTo(ka[0] * sx, ka[1] * sy)
    ctx.lineTo(kb[0] * sx, kb[1] * sy)
    ctx.stroke()
  }
  ctx.fillStyle = 'rgba(0, 220, 200, 0.95)'
  for (const k of kps) {
    if (k[2] < KP_MIN_CONF) continue
    ctx.beginPath()
    ctx.arc(k[0] * sx, k[1] * sy, 2.5, 0, Math.PI * 2)
    ctx.fill()
  }
}

onMounted(() => {
  if (!videoEl.value) return
  player = new StreamPlayer(videoEl.value, props.cameraId)
  player.onStatus = (s) => {
    Object.assign(status, s)
  }
  player.start()
  metaTimer = window.setInterval(pollMetadata, META_INTERVAL_MS)
})

onUnmounted(() => {
  player?.stop()
  player = null
  if (metaTimer !== null) {
    window.clearInterval(metaTimer)
    metaTimer = null
  }
})
</script>

<style scoped>
.player-wrap {
  position: relative;
  width: 100%;
  background: #000;
  border-radius: 6px;
  overflow: hidden;
}

video {
  width: 100%;
  display: block;
  background: #000;
}

.box-overlay {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
}

.player-status {
  position: absolute;
  top: 8px;
  left: 8px;
  display: flex;
  gap: 8px;
  padding: 3px 8px;
  border-radius: 4px;
  background: rgba(0, 0, 0, 0.55);
  color: #e6e6e6;
  font-size: 11px;
  letter-spacing: 0.06em;
}

.transport {
  opacity: 0.75;
}

.state {
  opacity: 0.95;
}

.ai-badge {
  opacity: 0.95;
  color: #00dcc8;
}

.player-tools {
  position: absolute;
  top: 8px;
  right: 8px;
  display: flex;
  gap: 4px;
}

.tool-btn {
  padding: 3px 8px;
  font-size: 11px;
  color: #d8d8d8;
  background: rgba(0, 0, 0, 0.55);
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 4px;
  cursor: pointer;
}

.tool-btn.active {
  border-color: rgba(255, 255, 255, 0.45);
  color: #fff;
}

.player-detail {
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  padding: 6px 10px;
  background: rgba(0, 0, 0, 0.55);
  color: #bdbdbd;
  font-size: 11px;
  line-height: 1.4;
}

.tap-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.45);
  border: none;
  cursor: pointer;
}

.tap-overlay span {
  padding: 8px 16px;
  border: 1px solid rgba(255, 255, 255, 0.5);
  border-radius: 4px;
  color: #fff;
  font-size: 12px;
  letter-spacing: 0.08em;
}
</style>
