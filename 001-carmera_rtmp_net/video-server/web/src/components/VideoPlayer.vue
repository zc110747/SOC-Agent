<template>
  <div class="player-wrap">
    <video ref="videoEl" autoplay playsinline muted></video>
    <div class="player-status">{{ stateText }}</div>
    <button class="fs-btn" @click="fullscreen" title="Fullscreen">
      <el-icon><FullScreen /></el-icon>
    </button>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { FullScreen } from '@element-plus/icons-vue'
import { WebRTCPlayer, type PlayerState } from '@/webrtc/player'

const props = defineProps<{ cameraId: string }>()
const videoEl = ref<HTMLVideoElement | null>(null)
const state = ref<PlayerState>('idle')
let player: WebRTCPlayer | null = null

const stateText = computed(() => {
  switch (state.value) {
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

function fullscreen() {
  videoEl.value?.requestFullscreen?.()
}

onMounted(() => {
  if (!videoEl.value) return
  player = new WebRTCPlayer(videoEl.value, props.cameraId)
  player.onState = (s) => {
    state.value = s
  }
  player.start()
})

onUnmounted(() => {
  player?.stop()
  player = null
})
</script>
