<template>
  <div class="player-wrap">
    <video ref="videoEl" autoplay playsinline muted></video>

    <!-- Autoplay can be refused on phones (low power / data saver). Without a
         visible affordance the symptom is just "a black rectangle". -->
    <button v-if="status.needsTap" class="tap-overlay" @click="player?.resume()">
      <span>TAP TO PLAY</span>
    </button>

    <div class="player-status">
      <span class="transport">{{ transportLabel }}</span>
      <span class="state">{{ stateText }}</span>
    </div>

    <div class="player-tools">
      <button class="tool-btn" :class="{ active: status.transport === 'webrtc' }" @click="player?.useWebRTC()">
        WebRTC
      </button>
      <button class="tool-btn" :class="{ active: status.transport === 'hls' }" @click="player?.useHLS()">
        HLS
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

const props = defineProps<{ cameraId: string }>()
const videoEl = ref<HTMLVideoElement | null>(null)
let player: StreamPlayer | null = null

const status = reactive<PlayerStatus>({
  state: 'idle',
  transport: null,
  detail: '',
  needsTap: false
})

const stateText = computed(() => {
  switch (status.state) {
    case 'connected':
      return 'CONNECTED'
    case 'fallback':
      return 'PLAYING'
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

// Naming the transport in the UI is what makes a phone-only failure debuggable:
// "HLS (fallback)" tells you immediately that WebRTC did not make it.
const transportLabel = computed(() => {
  if (status.transport === 'webrtc') return 'WEBRTC'
  if (status.transport === 'hls') return 'HLS (fallback)'
  return '—'
})

function fullscreen() {
  videoEl.value?.requestFullscreen?.()
}

onMounted(() => {
  if (!videoEl.value) return
  player = new StreamPlayer(videoEl.value, props.cameraId)
  player.onStatus = (s) => {
    Object.assign(status, s)
  }
  player.start()
})

onUnmounted(() => {
  player?.stop()
  player = null
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
