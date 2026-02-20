#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <stdint.h>

#include "colors.h"

#define SYS_BLOCK "/sys/class/block"
#define POLL_INTERVAL 1 // seconds
#define HASH_SIZE_MB 1  // first MB for tiny hash


typedef struct {
    char devname[32];
    char last_label[128];
    uint32_t last_hash;
    int last_audio;
} drive_info_t;

// Simple FNV-1a 32-bit hash
uint32_t fnv1a_hash(const unsigned char *data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619;
    }
    return hash;
}

volatile sig_atomic_t stop = 0;

bool use_color(void) {
    const char *no_color = getenv("NO_COLOR");
    return isatty(STDOUT_FILENO) && no_color == NULL;
}

void handle_sigterm(int sig) {
    (void) sig; // explicitly mark unused
    stop = 1; // NEVER do printf here
}

// Check if an audio CD is present (all tracks must be audio)
int is_audio_cd(const char *devpath) {
    int fd = open(devpath, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;

    struct cdrom_tochdr tochdr;
    if (ioctl(fd, CDROMREADTOCHDR, &tochdr) != 0) {
        close(fd);
        return 0;
    }

    for (int t = tochdr.cdth_trk0; t <= tochdr.cdth_trk1; t++) {
        struct cdrom_tocentry te = {0};
        te.cdte_track = t;
        te.cdte_format = CDROM_LBA;
        if (ioctl(fd, CDROMREADTOCENTRY, &te) == 0) {
            // Data track → not audio
            if (te.cdte_ctrl & CDROM_DATA_TRACK) {
                close(fd);
                return 0;
            }
        }
    }

    close(fd);
    return 1; // All tracks are audio
}

// Compute hash from TOC + first MB
uint32_t hash_disc_toc(const char *devpath) {
    int fd = open(devpath, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;

    uint32_t hash = 2166136261u; // FNV-1a seed

    // 1) Hash TOC
    struct cdrom_tochdr tochdr;
    if (ioctl(fd, CDROMREADTOCHDR, &tochdr) == 0) {
        hash ^= tochdr.cdth_trk0;
        hash *= 16777619;
        hash ^= tochdr.cdth_trk1;
        hash *= 16777619;
        for (int t = tochdr.cdth_trk0; t <= tochdr.cdth_trk1; t++) {
            struct cdrom_tocentry te = {0};
            te.cdte_track = t;
            te.cdte_format = CDROM_LBA;
            if (ioctl(fd, CDROMREADTOCENTRY, &te) == 0) {
                hash ^= te.cdte_addr.lba & 0xFFFFFFFF;
                hash *= 16777619;
            }
        }
    }

    // 2) Hash first MB of data
    size_t bytes = HASH_SIZE_MB * 1024 * 1024;
    unsigned char *buf = malloc(bytes);
    if (buf) {
        ssize_t r = read(fd, buf, bytes);
        if (r > 0) {
            for (ssize_t i = 0; i < r; i++) {
                hash ^= buf[i];
                hash *= 16777619;
            }
        }
        free(buf);
    }

    close(fd);
    return hash;
}

// Get drive status, label, hash, audio flag
int check_drive(drive_info_t *drive, char *label, size_t label_size, uint32_t *hash, int *audio) {
    char devpath[64];
    snprintf(devpath, sizeof(devpath), "/dev/%s", drive->devname);

    int fd = open(devpath, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    int status = ioctl(fd, CDROM_DRIVE_STATUS, 0);
    close(fd);

    if (status == CDS_DISC_OK) {
        // Volume label via blkid (may be empty for data CDs)
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "blkid -s LABEL -o value %s", devpath);
        FILE *fp = popen(cmd, "r");
        if (fp && fgets(label, label_size, fp)) {
            label[strcspn(label, "\n")] = 0;
            pclose(fp);
        } else {
            label[0] = '\0';
            if (fp) pclose(fp);
        }

        // Hash and audio flag
        *hash = hash_disc_toc(devpath);
        *audio = is_audio_cd(devpath);
    } else {
        label[0] = '\0';
        *hash = 0;
        *audio = 0;
    }

    return status;
}

void clear_screen() {
    printf(CLEAR_SCREEN);
    fflush(stdout);
}

int main(void) {
    signal(SIGTERM, handle_sigterm);
    signal(SIGINT, handle_sigterm);

    drive_info_t drives[16];
    int num_drives = 0;

    // Enumerate drives
    DIR *d = opendir(SYS_BLOCK);
    if (!d) {
        perror("opendir");
        return 1;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "sr", 2) == 0) {
            strncpy(drives[num_drives].devname, entry->d_name, sizeof(drives[num_drives].devname));
            drives[num_drives].last_label[0] = '\0';
            drives[num_drives].last_hash = 0;
            drives[num_drives].last_audio = 0;
            num_drives++;
        }
    }
    closedir(d);

    if (num_drives == 0) {
        printf("No optical drives found.\n");
        return 1;
    } else if (num_drives == 1) {
        printf("Monitoring %s1%s optical drive...\n", ITALIC, COLOR_RESET);
    } else {
        printf("Monitoring %s%d%s optical drives...\n", ITALIC, num_drives, COLOR_RESET);
    }

    while (!stop) {
        for (int i = 0; i < num_drives; i++) {
            char current_label[128];
            uint32_t current_hash;
            int current_audio;

            int status = check_drive(&drives[i], current_label, sizeof(current_label),
                                     &current_hash, &current_audio);

            if (status == CDS_DISC_OK) {
                // Detect disc change
                if (strcmp(current_label, drives[i].last_label) != 0 ||
                    current_hash != drives[i].last_hash ||
                    current_audio != drives[i].last_audio) {
                    if (use_color()) {
                        printf("%s/dev/%s:%s %sDisc changed!%s\n", COLOR_BOLD_BRIGHT_WHITE, drives[i].devname,
                               COLOR_RESET,
                               ITALIC, COLOR_RESET);
                        printf("\t%sLabel:%s %s%s%s\n", COLOR_BOLD_BRIGHT_WHITE, COLOR_RESET, COLOR_BOLD_BRIGHT_YELLOW,
                               current_label[0] ? current_label : "(none)", COLOR_RESET);
                        printf("\t%sHash:%s %s0x%08X%s\n", COLOR_BOLD_BRIGHT_WHITE, COLOR_RESET, COLOR_BOLD_BRIGHT_BLUE,
                               current_hash, COLOR_RESET);
                        printf("\t%sAudio CD:%s %s%s%s\n", COLOR_BOLD_BRIGHT_WHITE, COLOR_RESET,
                               current_audio ? COLOR_BOLD_BRIGHT_GREEN : COLOR_BOLD_BRIGHT_RED,
                               current_audio ? "Yes" : "No", COLOR_RESET);
                    } else {
                        printf("/dev/%s: Disc changed!\n", drives[i].devname);
                        printf("\tLabel: %s\n", current_label[0] ? current_label : "(none)");
                        printf("\tHash: 0x%08X\n", current_hash);
                        printf("\tAudio CD: %s\n", current_audio ? "Yes" : "No");
                    }
                    // TODO: trigger 86Box refresh here

                    strncpy(drives[i].last_label, current_label, sizeof(drives[i].last_label));
                    drives[i].last_hash = current_hash;
                    drives[i].last_audio = current_audio;
                }
            } else {
                // Disc removed or tray open
                if (drives[i].last_label[0] != '\0' || drives[i].last_hash != 0) {
                    if (use_color()) {
                        printf("%s/dev/%s:%s %sDisc removed or tray open%s\n", COLOR_BOLD_BRIGHT_WHITE,
                               drives[i].devname,
                               COLOR_RESET, ITALIC, COLOR_RESET);
                    } else {
                        printf("/dev/%s: Disc removed or tray open\n", drives[i].devname);
                    }
                    drives[i].last_label[0] = '\0';
                    drives[i].last_hash = 0;
                    drives[i].last_audio = 0;
                }
            }
        }

        sleep(POLL_INTERVAL);
    }

    printf("\nShutting down cleanly...\n");
    return 0;
}
