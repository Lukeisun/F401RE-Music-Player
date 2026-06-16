#pragma once
#include "gpio.hpp"
#include "zephyr/fs/fs_interface.h"
#include <ff.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/disk_access.h>

#define DISK_DRIVE_NAME "SD"
#define MAX_PATH 128
#define FS_RET_OK FR_OK
#define DISK_MOUNT_PT "/" DISK_DRIVE_NAME ":"
#define MAX_DIRS 64
#define MAX_FILES 64

class SDCard {
private:
  struct directory_entries {
    char name[MAX_FILE_NAME];
  };
  struct file_entries {
    char name[MAX_FILE_NAME];
    uint32_t size;
  };
  GPIO card_detect;
  // Debounce work item
  static constexpr char *disk_pdrv = DISK_DRIVE_NAME;
  static constexpr char *disk_mount_pt = DISK_MOUNT_PT;
  // End Directory File
  inline static FATFS fat_fs;
  /* mounting info */
  struct fs_mount_t mp = {
      .type = FS_FATFS,
      .fs_data = &fat_fs,
  };
  struct k_work_q *workq;
  struct k_work_q *music_workq;
  struct k_work music_work;
  inline static struct fs_file_t file;
  int get_dirs(const char *path);
  int mount();
  int unmount();
  static void music_cb(struct k_work *work);
  static void card_detect_cb(void *ctx, bool active);

public:
  inline static bool mounted = false;
  SDCard(struct k_work_q *workq, struct k_work_q *music_workq);
  int lsdir(int idx);
  int lsdir(const char *path);
  int read_file(int dir_idx, int file_idx);

  inline static directory_entries dir_list[MAX_DIRS];
  inline static uint8_t dir_count = 0;
  inline static file_entries file_list[MAX_DIRS];
  inline static uint8_t file_count = 0;
};
