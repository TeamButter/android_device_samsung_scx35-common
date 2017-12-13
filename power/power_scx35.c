/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2014 The CyanogenMod Project
 * Copyright (C) 2017 The Lineage Project
 * Copyright (C) 2014-2015 Andreas Schneider <asn@cryptomilk.org>
 * Copyright (C) 2014-2015 Christopher N. Hesse <raymanfx@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>

#define LOG_TAG "SCX35PowerHAL"
/* #define LOG_NDEBUG 0 */
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define SCALING_GOVERNOR_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
#define CPU_MAX_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"
#define SCALING_MAX_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define SCALING_MIN_FREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq"
#define PANEL_BRIGHTNESS "/sys/class/backlight/panel/brightness"

struct samsung_power_module {
	struct power_module base;
	pthread_mutex_t lock;
	int boostpulse_fd;
	int boostpulse_warned;
	char cpu_hispeed_freq[10];
	char cpu_min_freq[10];
	char cpu_max_freq[10];
	char* touchscreen_power_path;
	char* touchkey_power_path;
	bool touchkey_blocked;
};

static char governor[20];
static char CPU_HISPEED_FREQ_PATH[80];
static char IO_IS_BUSY_PATH[80];
static char BOOSTPULSE_PATH[80];

enum power_profile_e {
	PROFILE_POWER_SAVE = 0,
	PROFILE_BALANCED,
	PROFILE_HIGH_PERFORMANCE
};
static enum power_profile_e current_power_profile = PROFILE_HIGH_PERFORMANCE;

/**********************************************************
 *** HELPER FUNCTIONS
 **********************************************************/

static int sysfs_read(char *path, char *s, int num_bytes)
{
	char errno_str[64];
	int len;
	int ret = 0;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		strerror_r(errno, errno_str, sizeof(errno_str));
		ALOGE("Error opening %s: %s\n", path, errno_str);
		return -1;
	}

	len = read(fd, s, num_bytes - 1);
	if (len < 0) {
		strerror_r(errno, errno_str, sizeof(errno_str));
		ALOGE("Error reading from %s: %s\n", path, errno_str);

		ret = -1;
	} else {
		s[len] = '\0';
	}

	close(fd);

	return ret;
}

static void sysfs_write(const char *path, char *s) {

	char errno_str[64];
	int len;
	int fd;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		strerror_r(errno, errno_str, sizeof(errno_str));
		ALOGE("Error opening %s: %s\n", path, errno_str);
		return;
	}

	len = write(fd, s, strlen(s));

	if (len < 0) {
		strerror_r(errno, errno_str, sizeof(errno_str));
		ALOGE("Error writing to %s: %s\n", path, errno_str);
	}

	close(fd);
}

static int get_scaling_governor() {
    
	if (sysfs_read(SCALING_GOVERNOR_PATH, governor,
		sizeof(governor)) == -1) {
		return -1;
	} else {

        // Strip newline at the end.
        int len = strlen(governor);

        len--;

        while (len >= 0 && (governor[len] == '\n' || governor[len] == '\r'))
            governor[len--] = '\0';
	}

	return 0;
}

static int get_sysfs_path() {

	if (get_scaling_governor() < 0) {
		return -1;
	} else {
		if (strncmp(governor, "interactive", 11) == 0) {
			strcpy(CPU_HISPEED_FREQ_PATH, "/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq");
			strcpy(IO_IS_BUSY_PATH, "/sys/devices/system/cpu/cpufreq/interactive/io_is_busy");
			strcpy(BOOSTPULSE_PATH, "/sys/devices/system/cpu/cpufreq/interactive/boostpulse");
		} else if (strncmp(governor, "intelliactive", 13) == 0) {
			strcpy(CPU_HISPEED_FREQ_PATH, "/sys/devices/system/cpu/cpufreq/intelliactive/hispeed_freq");
			strcpy(IO_IS_BUSY_PATH, "/sys/devices/system/cpu/cpufreq/intelliactive/io_is_busy");
			strcpy(BOOSTPULSE_PATH, "/sys/devices/system/cpu/cpufreq/intelliactive/boostpulse");
		}
	}

	return 0;
}
	
static unsigned int read_panel_brightness() {

	unsigned int i, ret = 0;
	int read_status;
	// brightness can range from 0 to 255, so max. 3 chars + '\0'
	char panel_brightness[4];

	read_status = sysfs_read(PANEL_BRIGHTNESS, panel_brightness, sizeof(PANEL_BRIGHTNESS));
	if (read_status < 0) {
		ALOGE("%s: Failed to read panel brightness from %s!\n", __func__, PANEL_BRIGHTNESS);
		return -1;
	}

	for (i = 0; i < (sizeof(panel_brightness) / sizeof(panel_brightness[0])); i++) {
		if (isdigit(panel_brightness[i])) {
			ret += (panel_brightness[i] - '0');
		}
	}

	ALOGV("%s: Panel brightness is: %d", __func__, ret);

	return ret;
}

/**********************************************************
 *** POWER FUNCTIONS
 **********************************************************/

/* You need to request the powerhal lock before calling this function */
static int boostpulse_open(struct samsung_power_module *samsung_pwr) {

	char errno_str[64];

	if (samsung_pwr->boostpulse_fd < 0) {
		samsung_pwr->boostpulse_fd = open(BOOSTPULSE_PATH, O_WRONLY);
		if (samsung_pwr->boostpulse_fd < 0) {
			if (!samsung_pwr->boostpulse_warned) {
				strerror_r(errno, errno_str, sizeof(errno_str));
				ALOGE("Error opening %s: %s\n", BOOSTPULSE_PATH, errno_str);
				samsung_pwr->boostpulse_warned = 1;
			}
		}
	}

	return samsung_pwr->boostpulse_fd;
}

static void set_power_profile(struct samsung_power_module *samsung_pwr,
                              enum power_profile_e profile)
{

	int rc;
	struct stat sb;

	if (current_power_profile == profile) {
		return;
	}

	ALOGV("%s: profile=%d", __func__, profile);

	switch (profile) {
	case PROFILE_POWER_SAVE:
		// Limit to min freq
		sysfs_write(SCALING_MAX_FREQ_PATH, samsung_pwr->cpu_min_freq);
		ALOGD("%s: set powersave mode", __func__);
		break;
	case PROFILE_BALANCED:
		// Limit to hispeed freq
		sysfs_write(SCALING_MAX_FREQ_PATH, samsung_pwr->cpu_hispeed_freq);
		ALOGD("%s: set balanced mode", __func__);
		break;
	case PROFILE_HIGH_PERFORMANCE:
		// Restore normal max freq
		sysfs_write(SCALING_MAX_FREQ_PATH, samsung_pwr->cpu_max_freq);
		ALOGD("%s: set performance mode", __func__);
		break;
	}

	current_power_profile = profile;
}

static void find_input_nodes(struct samsung_power_module *samsung_pwr, char *dir) {

	const char filename[] = "name";
	char errno_str[64];
	struct dirent *de;
	char file_content[20];
	char *path = NULL;
	char *node_path = NULL;
	size_t pathsize;
	size_t node_pathsize;
	DIR *d;

	d = opendir(dir);
	if (d == NULL) {
		return;
	}

	while ((de = readdir(d)) != NULL) {
		if (strncmp(filename, de->d_name, sizeof(filename)) == 0) {
			pathsize = strlen(dir) + strlen(de->d_name) + 2;
			node_pathsize = strlen(dir) + strlen("enabled") + 2;

			path = malloc(pathsize);
			node_path = malloc(node_pathsize);
			if (path == NULL || node_path == NULL) {
				strerror_r(errno, errno_str, sizeof(errno_str));
				ALOGE("Out of memory: %s\n", errno_str);
				return;
			}

			snprintf(path, pathsize, "%s/%s", dir, filename);
			sysfs_read(path, file_content, sizeof(file_content));
			snprintf(node_path, node_pathsize, "%s/%s", dir, "enabled");

			if (strncmp(file_content, "sec_touchkey", 12) == 0) {
				ALOGV("%s: found touchkey path: %s\n", __func__, node_path);
				samsung_pwr->touchkey_power_path = malloc(node_pathsize);
				if (samsung_pwr->touchkey_power_path == NULL) {
					strerror_r(errno, errno_str, sizeof(errno_str));
					ALOGE("Out of memory: %s\n", errno_str);
					return;
				}
				snprintf(samsung_pwr->touchkey_power_path, node_pathsize,
								"%s", node_path);
			}

			if (strncmp(file_content, "sec_touchscreen", 15) == 0) {
				ALOGV("%s: found touchscreen path: %s\n", __func__, node_path);
				samsung_pwr->touchscreen_power_path = malloc(node_pathsize);
				if (samsung_pwr->touchscreen_power_path == NULL) {
					strerror_r(errno, errno_str, sizeof(errno_str));
					ALOGE("Out of memory: %s\n", errno_str);
					return;
				}
				snprintf(samsung_pwr->touchscreen_power_path, node_pathsize,
								   "%s", node_path);
			}
		}
	}

	if (path)
		free(path);
	if (node_path)
		free(node_path);
	closedir(d);
}

/**********************************************************
 *** INIT FUNCTIONS
 **********************************************************/

static void init_cpufreqs(struct samsung_power_module *samsung_pwr) {

	int rc;
	struct stat sb;

	sysfs_read(SCALING_MIN_FREQ_PATH, samsung_pwr->cpu_min_freq,
		   sizeof(samsung_pwr->cpu_min_freq));
	sysfs_read(CPU_HISPEED_FREQ_PATH, samsung_pwr->cpu_hispeed_freq,
		   sizeof(samsung_pwr->cpu_hispeed_freq));
	sysfs_read(CPU_MAX_FREQ_PATH, samsung_pwr->cpu_max_freq,
		   sizeof(samsung_pwr->cpu_max_freq));
	ALOGV("%s: CPU min freq: %s\n", __func__, samsung_pwr->cpu_min_freq);
	ALOGV("%s: CPU hispeed freq: %s\n", __func__, samsung_pwr->cpu_hispeed_freq);
	ALOGV("%s: CPU max freq: %s\n", __func__, samsung_pwr->cpu_max_freq);
}

static void init_touch_input_power_path(struct samsung_power_module *samsung_pwr)
{

	char dir[1024];
	char errno_str[64];
	uint32_t i;

	for (i = 0; i < 20; i++) {
		snprintf(dir, sizeof(dir), "/sys/class/input/input%d", i);
		find_input_nodes(samsung_pwr, dir);
	}
}

/*
 * The init function performs power management setup actions at runtime
 * startup, such as to set default cpufreq parameters.  This is called only by
 * the Power HAL instance loaded by PowerManagerService.
 */
static void samsung_power_init(struct power_module *module) {

	struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;

	get_scaling_governor();
	get_sysfs_path();
	init_cpufreqs(samsung_pwr);
	init_touch_input_power_path(samsung_pwr);
}

/*
 * The setInteractive function performs power management actions upon the
 * system entering interactive state (that is, the system is awake and ready
 * for interaction, often with UI devices such as display and touchscreen
 * enabled) or non-interactive state (the system appears asleep, display
 * usually turned off).  The non-interactive state is usually entered after a
 * period of inactivity, in order to conserve battery power during such
 * inactive periods.
 *
 * Typical actions are to turn on or off devices and adjust cpufreq parameters.
 * This function may also call the appropriate interfaces to allow the kernel
 * to suspend the system to low-power sleep state when entering non-interactive
 * state, and to disallow low-power suspend when the system is in interactive
 * state.  When low-power suspend state is allowed, the kernel may suspend the
 * system whenever no wakelocks are held.
 *
 * on is non-zero when the system is transitioning to an interactive / awake
 * state, and zero when transitioning to a non-interactive / asleep state.
 *
 * This function is called to enter non-interactive state after turning off the
 * screen (if present), and called to enter interactive state prior to turning
 * on the screen.
 */
static void samsung_power_set_interactive(struct power_module *module, int on) {

	struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;
	struct stat sb;
	char buf[80];
	char touchkey_node[2];
	int rc;

	ALOGV("power_set_interactive: %d\n", on);

	// Do not disable any input devices if the screen is on but we are in a non-interactive state
	if (!on) {
		if (read_panel_brightness() > 0) {
			ALOGV("%s: Moving to non-interactive state, but screen is still on,"
			      " not disabling input devices\n", __func__);
		goto out;
		}
	}

	sysfs_write(samsung_pwr->touchscreen_power_path, on ? "1" : "0");

	rc = stat(samsung_pwr->touchkey_power_path, &sb);
	if (rc < 0) {
		goto out;
	}

	if (!on) {
		if (sysfs_read(samsung_pwr->touchkey_power_path, touchkey_node,
			       sizeof(touchkey_node)) == 0) {
			/*
			* If touchkey_node is 0, the keys have been disabled by another component
			* (for example cmhw), which means we don't want them to be enabled when resuming
			* from suspend.
			*/
			if ((touchkey_node[0] - '0') == 0) {
				samsung_pwr->touchkey_blocked = true;
			} else {
				samsung_pwr->touchkey_blocked = false;
				sysfs_write(samsung_pwr->touchkey_power_path, "0");
			}
		}
	} else {
		if (!samsung_pwr->touchkey_blocked) {
			sysfs_write(samsung_pwr->touchkey_power_path, "1");
		}
	}

out:
	sysfs_write(IO_IS_BUSY_PATH, on ? "1" : "0");
	ALOGV("power_set_interactive: %d done\n", on);
}

/*
 * The powerHint function is called to pass hints on power requirements, which
 * may result in adjustment of power/performance parameters of the cpufreq
 * governor and other controls.
 *
 * The possible hints are:
 *
 * POWER_HINT_VSYNC
 *
 *     Foreground app has started or stopped requesting a VSYNC pulse
 *     from SurfaceFlinger.  If the app has started requesting VSYNC
 *     then CPU and GPU load is expected soon, and it may be appropriate
 *     to raise speeds of CPU, memory bus, etc.  The data parameter is
 *     non-zero to indicate VSYNC pulse is now requested, or zero for
 *     VSYNC pulse no longer requested.
 *
 * POWER_HINT_INTERACTION
 *
 *     User is interacting with the device, for example, touchscreen
 *     events are incoming.  CPU and GPU load may be expected soon,
 *     and it may be appropriate to raise speeds of CPU, memory bus,
 *     etc.  The data parameter is unused.
 *
 * POWER_HINT_LOW_POWER
 *
 *     Low power mode is activated or deactivated. Low power mode
 *     is intended to save battery at the cost of performance. The data
 *     parameter is non-zero when low power mode is activated, and zero
 *     when deactivated.
 *
 * POWER_HINT_CPU_BOOST
 *
 *     An operation is happening where it would be ideal for the CPU to
 *     be boosted for a specific duration. The data parameter is an
 *     integer value of the boost duration in microseconds.
 */
static void samsung_power_hint(struct power_module *module,
                                  power_hint_t hint,
                                  void *data)
{

	struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;
	char errno_str[64];
	int len;

	switch (hint) {
		case POWER_HINT_INTERACTION: {
			char errno_str[64];
			ssize_t len;
			int fd;

			if (current_power_profile == PROFILE_POWER_SAVE) {
				return;
			}

			ALOGV("%s: POWER_HINT_INTERACTION", __func__);

			if (boostpulse_open(samsung_pwr) >= 0) {
				len = write(samsung_pwr->boostpulse_fd, "1", 1);

				if (len < 0) {
					strerror_r(errno, errno_str, sizeof(errno_str));
					ALOGE("Error writing to %s: %s\n", BOOSTPULSE_PATH, errno_str);
				}
			}

		break;
		}
		case POWER_HINT_VSYNC: {
			ALOGV("%s: POWER_HINT_VSYNC", __func__);
			break;
		}
		case POWER_HINT_SET_PROFILE: {
			int profile = *((intptr_t *)data);

			ALOGV("%s: POWER_HINT_SET_PROFILE", __func__);
	
			set_power_profile(samsung_pwr, profile);
			break;
		}
		default:
			break;
	}
}

static int samsung_get_feature(struct power_module *module __unused,
                               feature_t feature)
{

	if (feature == POWER_FEATURE_SUPPORTED_PROFILES) {
		return 3;
	}

	return -1;
}

static void samsung_set_feature(struct power_module *module, feature_t feature, int state)
{

	struct samsung_power_module *samsung_pwr = (struct samsung_power_module *) module;

	switch (feature) {
#ifdef TARGET_TAP_TO_WAKE_NODE
		case POWER_FEATURE_DOUBLE_TAP_TO_WAKE:
			ALOGV("%s: %s double tap to wake", __func__, state ? "enabling" : "disabling");
			sysfs_write(TARGET_TAP_TO_WAKE_NODE, state > 0 ? "1" : "0");
			break;
#endif
		default:
			break;
	}
}

static int samsung_power_open(const hw_module_t* module, const char* name,
                    hw_device_t** device)
{
    ALOGD("%s: enter; name=%s", __FUNCTION__, name);

    if (strcmp(name, POWER_HARDWARE_MODULE_ID)) {
        return -EINVAL;
    }

    power_module_t *dev = (power_module_t *)calloc(1,
            sizeof(power_module_t));

    if (!dev) {
        ALOGD("%s: failed to allocate memory", __FUNCTION__);
        return -ENOMEM;
    }

    dev->common.tag = HARDWARE_MODULE_TAG;
    dev->common.module_api_version = POWER_MODULE_API_VERSION_0_2;
    dev->common.hal_api_version = HARDWARE_HAL_API_VERSION;

    dev->init = samsung_power_init;
    dev->powerHint = samsung_power_hint;
    dev->setInteractive = samsung_power_set_interactive;

    *device = (hw_device_t*)dev;

    ALOGD("%s: exit", __FUNCTION__);

    return 0;
}

static struct hw_module_methods_t power_module_methods = {
	.open = samsung_power_open,
};

struct samsung_power_module HAL_MODULE_INFO_SYM = {
	.base = {
		.common = {
			.tag = HARDWARE_MODULE_TAG,
			.module_api_version = POWER_MODULE_API_VERSION_0_2,
			.hal_api_version = HARDWARE_HAL_API_VERSION,
			.id = POWER_HARDWARE_MODULE_ID,
			.name = "Samsung Power HAL",
			.author = "The CyanogenMod Project",
			.methods = &power_module_methods,
		},

		.init = samsung_power_init,
		.setInteractive = samsung_power_set_interactive,
		.powerHint = samsung_power_hint,
		.getFeature = samsung_get_feature,
		.setFeature = samsung_set_feature
	},

	.lock = PTHREAD_MUTEX_INITIALIZER,
	.boostpulse_fd = -1,
	.boostpulse_warned = 0,
};
