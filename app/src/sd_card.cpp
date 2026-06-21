#include "sd_card.hpp"
#include "zephyr/drivers/i2s.h"
#include "zephyr/dt-bindings/gpio/gpio.h"
#include "zephyr/dt-bindings/gpio/st-morpho-header.h"
#include "zephyr/fs/fs.h"
#include "zephyr/kernel.h"
#include "zephyr/sys/printk.h"
#include "zephyr/sys/util.h"
#include <stdlib.h>
#define BLOCK_SIZE 2048
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
// 1kHz sine wave lookup table (one full period at 44100/1000 = ~44 samples)
// Amplitude ~50% of int16_t max to avoid clipping
static const int16_t sine_table[] = {
    0,      4560,   8981,   13126,  16870,  20101,  22725,  24665,
    25864,  26290,  25935,  24814,  22961,  20432,  17300,  13651,
    9586,   5215,   659,    -3964,  -8527,  -12905, -16980, -20642,
    -23791, -26342, -28228, -29399, -29825, -29496, -28422, -26631,
    -24166, -21090, -17478, -13419, -9013,  -4366,  402,    5168,
    9807,   14197,  18222,  21776,  24763,  27102,
};
#define SINE_TABLE_LEN (sizeof(sine_table) / sizeof(sine_table[0]))

// Generate a test tone directly through I2S (no SD card involved).
// Plays a 1kHz sine wave for ~3 seconds.
static void play_test_tone(void) {
  const struct device *dev_i2s = DEVICE_DT_GET(DT_NODELABEL(i2s3));
  struct i2s_config i2s_cfg;
  void *tx_block[NUM_BLOCKS];
  int ret;

  if (!device_is_ready(dev_i2s)) {
    printk("I2S device not ready\n");
    return;
  }

  i2s_cfg.word_size = 16U;
  i2s_cfg.channels = 2U;
  i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
  i2s_cfg.frame_clk_freq = 44100;
  i2s_cfg.block_size = BLOCK_SIZE;
  i2s_cfg.timeout = 2000;
  i2s_cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
  i2s_cfg.mem_slab = &tx_0_mem_slab;

  ret = i2s_configure(dev_i2s, I2S_DIR_TX, &i2s_cfg);
  if (ret < 0) {
    printk("Failed to configure I2S for test tone: %d\n", ret);
    return;
  }

  // Fill blocks with sine wave data
  uint32_t sample_idx = 0;
  for (int b = 0; b < NUM_BLOCKS; b++) {
    ret = k_mem_slab_alloc(&tx_0_mem_slab, &tx_block[b], K_FOREVER);
    if (ret < 0) {
      printk("Failed to alloc test tone block\n");
      return;
    }
    auto buf = static_cast<int16_t *>(tx_block[b]);
    // BLOCK_SIZE / 4 = number of stereo frames per block (2 bytes * 2 channels)
    for (int i = 0; i < BLOCK_SIZE / 4; i++) {
      int16_t sample = sine_table[sample_idx % SINE_TABLE_LEN];
      buf[i * 2] = sample;     // left
      buf[i * 2 + 1] = sample; // right
      sample_idx++;
    }
    ret = i2s_write(dev_i2s, tx_block[b], BLOCK_SIZE);
    if (ret < 0) {
      printk("Failed to write test tone block\n");
      return;
    }
  }

  ret = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_START);
  if (ret < 0) {
    printk("Failed to start test tone\n");
    return;
  }

  // Stream for ~3 seconds (44100 * 4 bytes/frame = 176400 bytes/sec, ~3s =
  // 529200 bytes)
  uint32_t bytes_remaining = 529200;
  while (bytes_remaining > 0) {
    void *block;
    k_mem_slab_alloc(&tx_0_mem_slab, &block, K_FOREVER);
    auto buf = static_cast<int16_t *>(block);
    for (int i = 0; i < BLOCK_SIZE / 4; i++) {
      int16_t sample = sine_table[sample_idx % SINE_TABLE_LEN];
      buf[i * 2] = sample;
      buf[i * 2 + 1] = sample;
      sample_idx++;
    }
    ret = i2s_write(dev_i2s, block, BLOCK_SIZE);
    if (ret < 0) {
      printk("Test tone write failed, recovering\n");
      i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
      i2s_write(dev_i2s, block, BLOCK_SIZE);
      i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_START);
    }
    bytes_remaining -=
        (bytes_remaining >= BLOCK_SIZE) ? BLOCK_SIZE : bytes_remaining;
  }

  i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
  printk("Test tone DONE\n");
}

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
  while (to_read > 0) {
    ret = i2s_write(dev_i2s, ready_block,
                    read); // instantly queue the pre-read block
    if (ret < 0) {
      printk("UNDERRUN\n");
    }

    k_mem_slab_alloc(&tx_0_mem_slab, &ready_block,
                     K_FOREVER); // wait for DMA to free one
    read = fs_read(&file, ready_block,
                   BLOCK_SIZE); // read next (slow, but queue has ~10 blocks)
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
      printk("\t%s\n",entry.name);
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
