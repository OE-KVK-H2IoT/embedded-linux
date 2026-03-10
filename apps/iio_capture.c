/* iio_capture.c — Read BMI160 IIO buffered data
 *
 * Usage: sudo ./iio_capture [device_num] [num_samples]
 *
 * Reads scan element format from sysfs, opens /dev/iio:deviceN,
 * and prints timestamped accelerometer + gyroscope data.
 *
 * Prerequisites:
 *   - Mainline BMI160 IIO driver loaded (modprobe bmi160_spi)
 *   - IIO buffered mode enabled (scan elements + trigger + buffer/enable)
 *
 * See: IIO Buffered Capture tutorial
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define IIO_SYSFS "/sys/bus/iio/devices/iio:device%d"
#define IIO_DEV   "/dev/iio:device%d"

/*
 * One sample set from the BMI160 IIO driver with all 6 axes + timestamp enabled.
 *
 * Channel layout (from scan_elements):
 *   in_accel_x:    le:s16/16>>0  scan_index=0
 *   in_accel_y:    le:s16/16>>0  scan_index=1
 *   in_accel_z:    le:s16/16>>0  scan_index=2
 *   in_anglvel_x:  le:s16/16>>0  scan_index=3
 *   in_anglvel_y:  le:s16/16>>0  scan_index=4
 *   in_anglvel_z:  le:s16/16>>0  scan_index=5
 *   in_timestamp:  le:s64/64>>0  scan_index=6
 *
 * Total: 6 × 2 bytes + 2 bytes padding + 8 bytes timestamp = 22 bytes
 * (padding inserted by IIO core to align timestamp to 8-byte boundary)
 */
struct iio_sample {
	int16_t accel_x;
	int16_t accel_y;
	int16_t accel_z;
	int16_t anglvel_x;
	int16_t anglvel_y;
	int16_t anglvel_z;
	int16_t _pad;       /* alignment padding to 8-byte boundary */
	int64_t timestamp;  /* nanoseconds from iio_get_time_ns() */
} __attribute__((packed));

static float read_float_attr(int dev_num, const char *attr)
{
	char path[256];
	float val;
	FILE *f;

	snprintf(path, sizeof(path), IIO_SYSFS "/%s", dev_num, attr);
	f = fopen(path, "r");
	if (!f) {
		perror(path);
		exit(1);
	}
	if (fscanf(f, "%f", &val) != 1) {
		fprintf(stderr, "Failed to read value from %s\n", path);
		fclose(f);
		exit(1);
	}
	fclose(f);
	return val;
}

int main(int argc, char *argv[])
{
	int dev_num = argc > 1 ? atoi(argv[1]) : 0;
	int num_samples = argc > 2 ? atoi(argv[2]) : 100;

	/* Read scale factors from sysfs */
	float accel_scale = read_float_attr(dev_num, "in_accel_scale");
	float gyro_scale = read_float_attr(dev_num, "in_anglvel_scale");

	printf("Accel scale: %f, Gyro scale: %f\n", accel_scale, gyro_scale);
	printf("Sample size: %zu bytes\n\n", sizeof(struct iio_sample));

	/* Open the IIO character device */
	char dev_path[64];
	snprintf(dev_path, sizeof(dev_path), IIO_DEV, dev_num);
	int fd = open(dev_path, O_RDONLY);
	if (fd < 0) {
		perror(dev_path);
		fprintf(stderr, "\nMake sure IIO buffered mode is enabled:\n");
		fprintf(stderr, "  echo 1 > /sys/bus/iio/devices/iio:device%d/buffer/enable\n",
			dev_num);
		return 1;
	}

	printf("Reading %d samples from %s...\n\n", num_samples, dev_path);
	printf("%16s  %8s %8s %8s  %8s %8s %8s\n",
	       "timestamp_ns", "ax", "ay", "az", "gx", "gy", "gz");

	struct iio_sample sample;
	int count = 0;

	while (count < num_samples) {
		ssize_t n = read(fd, &sample, sizeof(sample));
		if (n < 0) {
			if (errno == EAGAIN)
				continue;
			perror("read");
			break;
		}
		if ((size_t)n != sizeof(sample)) {
			fprintf(stderr, "Short read: %zd bytes (expected %zu)\n",
				n, sizeof(sample));
			continue;
		}

		printf("%16ld  %8.3f %8.3f %8.3f  %8.3f %8.3f %8.3f\n",
		       (long)sample.timestamp,
		       sample.accel_x * accel_scale,
		       sample.accel_y * accel_scale,
		       sample.accel_z * accel_scale,
		       sample.anglvel_x * gyro_scale,
		       sample.anglvel_y * gyro_scale,
		       sample.anglvel_z * gyro_scale);
		count++;
	}

	close(fd);
	printf("\nCaptured %d samples.\n", count);
	return 0;
}
