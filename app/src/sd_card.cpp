#include "sd_card.hpp"
#include "zephyr/drivers/i2s.h"
#include "zephyr/dt-bindings/gpio/gpio.h"
#include "zephyr/dt-bindings/gpio/st-morpho-header.h"
#include "zephyr/fs/fs.h"
#include "zephyr/kernel.h"
#include "zephyr/sys/atomic.h"
#include "zephyr/sys/printk.h"
#include "zephyr/sys/util.h"
#define BLOCK_SIZE 4096
#define NUM_BLOCKS 4
// #define CD_NODE DT_NODELABEL(cd_pin)
// constexpr static const struct gpio_dt_spec cd_gpio =
//     GPIO_DT_SPEC_GET(CD_NODE, gpios);
constexpr static const struct gpio_dt_spec cd_gpio = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpiob)),
    .pin = 14,
    .dt_flags = GPIO_ACTIVE_HIGH};
K_MEM_SLAB_DEFINE(tx_0_mem_slab, WB_UP(BLOCK_SIZE), NUM_BLOCKS, WB_UP(32));
LOG_MODULE_REGISTER(sd_card);
// Called when card inserted/removed
void SDCard::card_detect_cb(void *ctx, bool active) {
  static int last_state = -1;
  SDCard *self = static_cast<SDCard *>(ctx);
  if (active == last_state)
    return;
  last_state = active;
  if (active) {
    self->mount();
  } else {
    self->unmount();
  }
}
void SDCard::music_cb(struct k_work *work) {
  printk("MUSIC_CB\n");

  // Uncomment the next line to play a test tone instead of WAV file.
  // This tests the I2S hardware path without involving the SD card.
  // play_test_tone(); return;

  auto self = CONTAINER_OF(work, SDCard, music_work);
  atomic_set(&total_time, 0);
  atomic_set(&elapsed_time, 0);
  auto &file = self->file;
  char bytes[64];
  void *tx_block[NUM_BLOCKS];
  struct i2s_config i2s_cfg;
  int ret;
  uint32_t tx_idx;
  const struct device *dev_i2s = DEVICE_DT_GET(DT_NODELABEL(i2s3));

  if (!device_is_ready(dev_i2s)) {
    printk("I2S device not ready\n");
    return;
  }

  // --- Parse WAV header ---
  memset(bytes, 0, sizeof(bytes));

  // RIFF header
  fs_read(&file, bytes, 4);
  if (strncmp(bytes, "RIFF", 4) != 0) {
    printk("Not a RIFF file\n");
    return;
  }
  fs_read(&file, bytes, 4); // file size - 8
  fs_read(&file, bytes, 4); // WAVE
  if (strncmp(bytes, "WAVE", 4) != 0) {
    printk("Not a WAVE file\n");
    return;
  }

  // fmt subchunk
  fs_read(&file, bytes, 4); // "fmt "
  if (strncmp(bytes, "fmt ", 4) != 0) {
    printk("Missing fmt chunk\n");
    return;
  }

  // fmt chunk size
  fs_read(&file, bytes, 4);
  uint32_t chunk_size = (uint8_t)bytes[0] | ((uint8_t)bytes[1] << 8) |
                        ((uint8_t)bytes[2] << 16) | ((uint8_t)bytes[3] << 24);
  printk("test\n");
  printk("fmt chunk_size: %u\n", chunk_size);

  // audio format (1 = PCM)
  fs_read(&file, bytes, 2);
  uint16_t audio_format = (uint8_t)bytes[0] | ((uint8_t)bytes[1] << 8);
  printk("audio_format: %u\n", audio_format);
  if (audio_format != 1) {
    printk("Unsupported audio format (not PCM): %u\n", audio_format);
    return;
  }

  // number of channels
  fs_read(&file, bytes, 2);
  uint16_t num_channels = (uint8_t)bytes[0] | ((uint8_t)bytes[1] << 8);
  printk("channels: %u\n", num_channels);

  // sample rate
  fs_read(&file, bytes, 4);
  uint32_t sample_rate = (uint8_t)bytes[0] | ((uint8_t)bytes[1] << 8) |
                         ((uint8_t)bytes[2] << 16) | ((uint8_t)bytes[3] << 24);
  printk("sample_rate: %u\n", sample_rate);

  // byte rate (skip)
  fs_read(&file, bytes, 4);

  // block align (skip)
  fs_read(&file, bytes, 2);

  // bits per sample
  fs_read(&file, bytes, 2);
  uint16_t bits_per_sample = (uint8_t)bytes[0] | ((uint8_t)bytes[1] << 8);
  printk("bits_per_sample: %u\n", bits_per_sample);

  // Skip any extra bytes in fmt chunk (for chunk_size > 16)
  if (chunk_size > 16) {
    uint32_t extra = chunk_size - 16;
    while (extra > 0) {
      uint32_t skip = extra > sizeof(bytes) ? sizeof(bytes) : extra;
      fs_read(&file, bytes, skip);
      extra -= skip;
    }
  }

  // Find "data" chunk (skip any non-data chunks like "fact", "LIST", etc.)
  memset(bytes, 0, sizeof(bytes));
  fs_read(&file, bytes, 4);
  while (strncmp(bytes, "data", 4) != 0) {
    // Read chunk size and skip it
    fs_read(&file, bytes, 4);
    uint32_t skip_size = (uint8_t)bytes[0] | ((uint8_t)bytes[1] << 8) |
                         ((uint8_t)bytes[2] << 16) | ((uint8_t)bytes[3] << 24);
    while (skip_size > 0) {
      uint32_t skip = skip_size > sizeof(bytes) ? sizeof(bytes) : skip_size;
      fs_read(&file, bytes, skip);
      skip_size -= skip;
    }
    // Read next chunk ID
    fs_read(&file, bytes, 4);
  }

  // data chunk size
  fs_read(&file, bytes, 4);
  uint32_t to_read = (uint8_t)bytes[0] | ((uint8_t)bytes[1] << 8) |
                     ((uint8_t)bytes[2] << 16) | ((uint8_t)bytes[3] << 24);
  printk("data size: %u\n", to_read);
  auto duration =
      to_read / (sample_rate * num_channels * (bits_per_sample / 8));
  atomic_set(&total_time, duration);
  k_sem_give(&self->header_ready);
  {
    uint32_t bench_start = k_uptime_get_32();
    uint32_t bench_bytes = 0;
    void *bench_buf;
    k_mem_slab_alloc(&tx_0_mem_slab, &bench_buf, K_FOREVER);
    for (int i = 0; i < 64; i++) { // 64 x 4096 = 256 KB
      int r = fs_read(&file, bench_buf, BLOCK_SIZE);
      if (r <= 0)
        break;
      bench_bytes += r;
    }
    uint32_t bench_elapsed = k_uptime_get_32() - bench_start;
    printk("Benchmark: %u bytes in %u ms = %u KB/s\n", bench_bytes,
           bench_elapsed, bench_bytes / bench_elapsed);
    k_mem_slab_free(&tx_0_mem_slab, bench_buf);
    to_read -= bench_bytes; // skip the bytes we already read (don't seek back)
  }
  // --- End benchmark ---

  // --- Configure I2S from WAV header ---
  i2s_cfg.word_size = bits_per_sample;
  i2s_cfg.channels = num_channels;
  i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
  i2s_cfg.frame_clk_freq = sample_rate;
  i2s_cfg.block_size = BLOCK_SIZE;
  i2s_cfg.timeout = 2000;
  i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
  i2s_cfg.mem_slab = &tx_0_mem_slab;

  ret = i2s_configure(dev_i2s, I2S_DIR_TX, &i2s_cfg);
  if (ret < 0) {
    printk("Failed to configure I2S: %d\n", ret);
    return;
  }
  printk("I2S configured: %u-bit, %u ch, %u Hz\n", bits_per_sample,
         num_channels, sample_rate);

  /* Prepare all TX blocks */
  for (tx_idx = 0; tx_idx < NUM_BLOCKS; tx_idx++) {
    ret = k_mem_slab_alloc(&tx_0_mem_slab, &tx_block[tx_idx], K_FOREVER);
    if (ret < 0) {
      printk("Failed to allocate TX block\n");
      return;
    }
  }
  static int read = 0;
  printk("to_read: %u\n", to_read);
  // Fill and queue all pre-allocated blocks before starting
  for (tx_idx = 0; tx_idx < NUM_BLOCKS - 1 && to_read > 0; tx_idx++) {
    read = fs_read(&file, tx_block[tx_idx], BLOCK_SIZE);
    to_read -= read;
    ret = i2s_write(dev_i2s, tx_block[tx_idx], BLOCK_SIZE);
    if (ret < 0) {
      printk("Failed to write prealloc\n");
      return;
    }
  }
  // Pre-read into the 12th block (read-ahead)
  void *ready_block = tx_block[NUM_BLOCKS - 1];
  read = fs_read(&file, ready_block, BLOCK_SIZE);
  to_read -= read;

  // Start I2S (11 blocks in queue, 1 ready to submit)
  ret = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_START);
  if (ret < 0) {
    printk("Failed to start trigger\n");
    return;
  }
  // i2s_write
  printk("to_read before loop %d\n", to_read);
  uint32_t start = k_cycle_get_32();
  uint32_t elapsed_us = k_cycle_get_32();
  while (to_read > 0) {
    ret = i2s_write(dev_i2s, ready_block,
                    read); // instantly queue the pre-read block
    duration = to_read / (sample_rate * num_channels * (bits_per_sample / 8));
    atomic_set(&self->elapsed_time, atomic_get(&self->total_time) - duration);
    if (ret < 0) {
      printk("UNDERRUN - recovering\n");
      i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
      // Re-fill queue with current ready_block + read a few more
      i2s_write(dev_i2s, ready_block, read);
      // Alloc and read next blocks to pre-fill...
      i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_START);
    }
    start = k_cycle_get_32();
    k_mem_slab_alloc(&tx_0_mem_slab, &ready_block, K_FOREVER);
    elapsed_us = k_cyc_to_us_floor32(k_cycle_get_32() - start);
    if (elapsed_us > 50000) {
      printk("SLOW ALLOC: %u us\n", elapsed_us);
    }
    start = k_cycle_get_32();
    read = fs_read(&file, ready_block, BLOCK_SIZE);
    elapsed_us = k_cyc_to_us_floor32(k_cycle_get_32() - start);
    if (elapsed_us > 50000) { // only print if > 50ms
      printk("SLOW READ: %u us\n", elapsed_us);
    }
    if (read <= 0)
      break;
    to_read -= read;
  };
  ret = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
  if (ret < 0) {
    printk("Failed to trigger drain\n");
    return;
  }
  printk("DONE\n");
}
// ISR - minimal, just reschedule the debounce timer
SDCard::SDCard(struct k_work_q *workq, struct k_work_q *music_workq)
    : card_detect(cd_gpio, workq, K_MSEC(50), card_detect_cb, this),
      workq(workq), music_workq(music_workq) {
  k_work_init(&this->music_work, &SDCard::music_cb);
  k_sem_init(&header_ready, 0, 1);
  this->mount();
}
int SDCard::mount() {
  // do {
  //   uint64_t memory_size_mb;
  //   uint32_t block_count;
  //   uint32_t block_size;
  // if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_CTRL_INIT, NULL) != 0) {
  //   LOG_ERR("Storage init ERROR!");
  // }
  //
  //   if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_COUNT,
  //                         &block_count)) {
  //     LOG_ERR("Unable to get sector count");
  //     break;
  //   }
  //   LOG_INF("Block count %u", block_count);
  //
  //   if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_GET_SECTOR_SIZE,
  //   &block_size)) {
  //     LOG_ERR("Unable to get sector size");
  //     break;
  //   }
  //   printk("Sector size %u\n", block_size);
  //
  //   memory_size_mb = (uint64_t)block_count * block_size;
  //   printk("Memory Size(MB) %u\n", (uint32_t)(memory_size_mb >> 20));
  //
  //   if (disk_access_ioctl(disk_pdrv, DISK_IOCTL_CTRL_DEINIT, NULL) != 0) {
  //     LOG_ERR("Storage deinit ERROR!");
  //     break;
  //   }
  // } while (0);
  if (this->mounted)
    return FS_RET_OK;
  mp.mnt_point = disk_mount_pt;
  auto res = fs_mount(&mp);
  if (res == FS_RET_OK) {
    this->mounted = true;
    get_dirs(this->disk_mount_pt);
  }
  return res;
}
int SDCard::unmount() {
  if (!this->mounted)
    return FS_RET_OK;
  auto res = fs_unmount(&mp);
  if (res == FS_RET_OK) {
    this->mounted = false;
    dir_count = 0;
    file_count = 0;
  }
  return res;
  // LOG_ERR("Unmount FSRESULT: %d", res);
}
int SDCard::lsdir(int idx) {
  if (idx < 0 || idx > dir_count) {
    return -1;
  }
  static char path[128];
  snprintf(path, 128, "%s/%s", DISK_MOUNT_PT, this->dir_list[idx].name);
  return this->lsdir(path);
}
int SDCard::read_file(int dir_idx, int file_idx) {
  if (dir_idx < 0 || dir_idx > dir_count) {
    return -1;
  }
  if (file_idx < 0 || file_idx > file_count) {
    return -1;
  }
  static char path[256];
  snprintf(path, 256, "%s/%s/%s", DISK_MOUNT_PT, this->dir_list[dir_idx].name,
           this->file_list[file_idx].name);
  fs_file_t_init(&this->file);
  printk("PATH %s\n", path);
  auto res = fs_open(&this->file, path, FS_O_READ);
  if (res != 0) {
    LOG_ERR("ERR OPENING FILE\n");
    return -1;
  }
  k_work_submit_to_queue(this->music_workq, &this->music_work);
  return 0;
}
int SDCard::lsdir(const char *path) {
  int res;
  struct fs_dir_t dirp;
  static struct fs_dirent entry;
  file_count = 0;

  fs_dir_t_init(&dirp);

  /* Verify fs_opendir() */
  res = fs_opendir(&dirp, path);
  if (res) {
    printk("Error opening dir %s [%d]\n", path, res);
    return res;
  }
  for (;;) {
    /* Verify fs_readdir() */
    res = fs_readdir(&dirp, &entry);

    /* entry.name[0] == 0 means end-of-dir */
    if (res || entry.name[0] == 0) {
      break;
    }

    if (entry.type == FS_DIR_ENTRY_FILE) {
      auto len = strlen(entry.name);
      if (strncmp(entry.name + (len - 4), ".wav", 4) != 0) {
        continue;
      }
      // if (entry.name
      file_entries file = {.name = "", .size = entry.size};
      memcpy(file.name, entry.name, MAX_FILE_NAME);
      file_list[file_count++] = file;
    }
  }
  return 0;
}
int SDCard::get_dirs(const char *path) {
  int res;
  struct fs_dir_t dirp;
  static struct fs_dirent entry;
  int count = 0;

  fs_dir_t_init(&dirp);

  /* Verify fs_opendir() */
  res = fs_opendir(&dirp, path);
  if (res) {
    LOG_ERR("Error opening dir %s [%d]\n", path, res);
    return res;
  }
  for (;;) {
    /* Verify fs_readdir() */
    res = fs_readdir(&dirp, &entry);

    /* entry.name[0] == 0 means end-of-dir */
    if (res || entry.name[0] == 0) {
      break;
    }

    if (entry.type == FS_DIR_ENTRY_DIR) {
      struct directory_entries dir;
      memcpy(dir.name, entry.name, MAX_FILE_NAME);
      printk("\t%s\n", entry.name);
      SDCard::dir_list[dir_count++] = dir;
    }
    count++;
  }

  /* Verify fs_closedir() */
  fs_closedir(&dirp);
  if (res == 0) {
    res = count;
  }

  return res;
}
void SDCard::time_str(char buf[9], uint32_t time) {
  memset(buf, 0, 9);
  auto hours = time / 3600;
  auto minutes = (time % 3600) / 60;
  auto seconds = time % 60;
  sprintf(buf, "%02d:%02d:%02d", hours, minutes, seconds);
}
