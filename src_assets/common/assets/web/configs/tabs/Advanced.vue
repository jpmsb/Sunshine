<script setup>
import { computed, ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config',
  'global_prep_cmd'
])

const config = ref(props.config)

/**
 * Native color inputs only accept #RRGGBB; strip alpha when present.
 */
const placeholderColorForPicker = computed(() => {
  const value = (config.value.screencast_placeholder_color || '#000000').trim()
  if (/^#[0-9A-Fa-f]{8}$/.test(value)) {
    return value.slice(0, 7)
  }
  if (/^#[0-9A-Fa-f]{6}$/.test(value)) {
    return value
  }
  return '#000000'
})

/**
 * @param {Event} event Color input change event.
 */
function onPlaceholderColorPicker(event) {
  const target = event.target
  if (target && typeof target.value === 'string') {
    config.value.screencast_placeholder_color = target.value
  }
}
</script>

<template>
  <div class="config-page">
    <!-- FEC Percentage -->
    <div class="mb-3">
      <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
      <input type="text" class="form-control" id="fec_percentage" placeholder="20" v-model="config.fec_percentage" />
      <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
    </div>

    <!-- Quantization Parameter -->
    <div class="mb-3">
      <label for="qp" class="form-label">{{ $t('config.qp') }}</label>
      <input type="number" class="form-control" id="qp" placeholder="28" v-model="config.qp" />
      <div class="form-text">{{ $t('config.qp_desc') }}</div>
    </div>

    <!-- Min Threads -->
    <div class="mb-3">
      <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
      <input type="number" class="form-control" id="min_threads" placeholder="2" min="1" v-model="config.min_threads" />
      <div class="form-text">{{ $t('config.min_threads_desc') }}</div>
    </div>

    <!-- HEVC Support -->
    <div class="mb-3">
      <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
      <select id="hevc_mode" class="form-select" v-model="config.hevc_mode">
        <option value="0">{{ $t('config.hevc_mode_0') }}</option>
        <option value="1">{{ $t('config.hevc_mode_1') }}</option>
        <option value="2">{{ $t('config.hevc_mode_2') }}</option>
        <option value="3">{{ $t('config.hevc_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.hevc_mode_desc') }}</div>
    </div>

    <!-- AV1 Support -->
    <div class="mb-3">
      <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
      <select id="av1_mode" class="form-select" v-model="config.av1_mode">
        <option value="0">{{ $t('config.av1_mode_0') }}</option>
        <option value="1">{{ $t('config.av1_mode_1') }}</option>
        <option value="2">{{ $t('config.av1_mode_2') }}</option>
        <option value="3">{{ $t('config.av1_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
    </div>

    <!-- Capture -->
    <div class="mb-3" v-if="platform !== 'macos'">
      <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
      <select id="capture" class="form-select" v-model="config.capture">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #freebsd>
            <option value="wlr">wlroots</option>
            <option value="x11">X11</option>
            <option value="portal">XDG Portal</option>
            <option value="screencast">ScreenCast (Pipewire)</option>
          </template>
          <template #linux>
            <option value="nvfbc">NvFBC</option>
            <option value="wlr">wlroots</option>
            <option value="kms">KMS</option>
            <option value="x11">X11</option>
            <option value="kwin">KWin Screencast</option>
            <option value="portal">XDG Portal</option>
            <option value="screencast">ScreenCast (Pipewire)</option>
          </template>
          <template #windows>
            <option value="ddx">Desktop Duplication API</option>
            <option value="wgc">Windows.Graphics.Capture {{ $t('_common.beta') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.capture_desc') }}</div>
    </div>

    <!-- Screencast persist -->
    <div class="mb-3" v-if="(platform === 'linux' || platform === 'freebsd') && config.capture === 'screencast'">
      <label for="screencast_persist" class="form-label">{{ $t('config.screencast_persist') }}</label>
      <select id="screencast_persist" class="form-select" v-model="config.screencast_persist">
        <option value="disabled">{{ $t('_common.disabled_def') }}</option>
        <option value="enabled">{{ $t('_common.enabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.screencast_persist_desc') }}</div>
    </div>

    <!-- Screencast placeholder -->
    <div class="mb-3" v-if="(platform === 'linux' || platform === 'freebsd') && config.capture === 'screencast'">
      <label for="screencast_placeholder_color" class="form-label">{{ $t('config.screencast_placeholder_color') }}</label>
      <div class="d-flex gap-2 align-items-center">
        <input
          id="screencast_placeholder_color_picker"
          type="color"
          class="form-control form-control-color"
          :value="placeholderColorForPicker"
          @input="onPlaceholderColorPicker"
        >
        <input
          id="screencast_placeholder_color"
          type="text"
          class="form-control"
          v-model="config.screencast_placeholder_color"
          placeholder="#000000"
        >
      </div>
      <div class="form-text">{{ $t('config.screencast_placeholder_color_desc') }}</div>
    </div>
    <div class="mb-3" v-if="(platform === 'linux' || platform === 'freebsd') && config.capture === 'screencast'">
      <label for="screencast_placeholder_text" class="form-label">{{ $t('config.screencast_placeholder_text') }}</label>
      <input
        id="screencast_placeholder_text"
        type="text"
        class="form-control"
        v-model="config.screencast_placeholder_text"
        :placeholder="$t('config.screencast_placeholder_text_placeholder')"
      >
      <div class="form-text">{{ $t('config.screencast_placeholder_text_desc') }}</div>
    </div>

    <!-- Encoder -->
    <div class="mb-3">
      <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
      <select id="encoder" class="form-select" v-model="config.encoder">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #windows>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="quicksync">Intel QuickSync</option>
            <option value="amdvce">AMD AMF/VCE</option>
          </template>
          <template #freebsd>
            <option value="vulkan">Vulkan</option>
            <option value="vaapi">VA-API</option>
          </template>
          <template #linux>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="vaapi">VA-API</option>
            <option value="vulkan">Vulkan</option>
          </template>
          <template #macos>
            <option value="videotoolbox">VideoToolbox</option>
          </template>
        </PlatformLayout>
        <option value="software">{{ $t('config.encoder_software') }}</option>
      </select>
      <div class="form-text">{{ $t('config.encoder_desc') }}</div>
    </div>

  </div>
</template>

<style scoped>

</style>
