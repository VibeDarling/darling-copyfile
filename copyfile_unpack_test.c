#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <copyfile.h>
#include <libkern/OSByteOrder.h>

#define TEST_SRC_PATH "/tmp/darling_copyfile_test_src.txt"
#define TEST_AD_PATH  "/tmp/._darling_copyfile_test_dst.txt"
#define TEST_DST_PATH "/tmp/darling_copyfile_test_dst.txt"

#define TEST_ATTR_NAME  "com.apple.test_provenance"
#define TEST_ATTR_VALUE "nanobrew-test-attribute-content"

typedef struct rsrcfork_header {
	u_int32_t	fh_DataOffset;
	u_int32_t	fh_MapOffset;
	u_int32_t	fh_DataLength;
	u_int32_t	fh_MapLength;
	u_int8_t	systemData[112];
	u_int8_t	appData[128];
	u_int32_t	mh_DataOffset;
	u_int32_t	mh_MapOffset;
	u_int32_t	mh_DataLength;
	u_int32_t	mh_MapLength;
	u_int32_t	mh_Next;
	u_int16_t	mh_RefNum;
	u_int8_t	mh_Attr;
	u_int8_t	mh_InMemoryAttr;
	u_int16_t	mh_Types;
	u_int16_t	mh_Names;
	u_int16_t	typeCount;
} __attribute__((aligned(2), packed)) rsrcfork_header_t;

#define RF_FIRST_RESOURCE    256
#define RF_NULL_MAP_LENGTH    30
#define RF_EMPTY_TAG  "This resource fork intentionally left blank   "

static const rsrcfork_header_t empty_rsrcfork_header = {
	OSSwapHostToBigInt32(RF_FIRST_RESOURCE),
	OSSwapHostToBigInt32(RF_FIRST_RESOURCE),
	0,
	OSSwapHostToBigInt32(RF_NULL_MAP_LENGTH),
	{ RF_EMPTY_TAG, },
	{ 0 },
	OSSwapHostToBigInt32(RF_FIRST_RESOURCE),
	OSSwapHostToBigInt32(RF_FIRST_RESOURCE),
	0,
	OSSwapHostToBigInt32(RF_NULL_MAP_LENGTH),
	0,
	0,
	0,
	0,
	OSSwapHostToBigInt16(RF_NULL_MAP_LENGTH - 2),
	OSSwapHostToBigInt16(RF_NULL_MAP_LENGTH),
	OSSwapHostToBigInt16(-1),
};

static void cleanup_files(void)
{
	unlink(TEST_SRC_PATH);
	unlink(TEST_AD_PATH);
	unlink(TEST_DST_PATH);
}

int main(void)
{
	int ret;
	char val_buf[128] = {0};

	printf("--- Test 1: Standard xattr pack and unpack ---\n");
	cleanup_files();

	int fd = open(TEST_SRC_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		perror("open src");
		return 1;
	}
	write(fd, "test payload\n", 13);
	close(fd);

	ret = setxattr(TEST_SRC_PATH, TEST_ATTR_NAME, TEST_ATTR_VALUE, strlen(TEST_ATTR_VALUE), 0, 0);
	if (ret < 0) {
		perror("setxattr");
		return 1;
	}

	copyfile_state_t state = copyfile_state_alloc();
	ret = copyfile(TEST_SRC_PATH, TEST_AD_PATH, state, COPYFILE_PACK | COPYFILE_XATTR);
	if (ret < 0) {
		perror("copyfile PACK");
		copyfile_state_free(state);
		return 1;
	}
	copyfile_state_free(state);

	fd = open(TEST_DST_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		perror("open dst");
		return 1;
	}
	write(fd, "test payload\n", 13);
	close(fd);

	state = copyfile_state_alloc();
	ret = copyfile(TEST_AD_PATH, TEST_DST_PATH, state, COPYFILE_UNPACK | COPYFILE_XATTR);
	if (ret < 0) {
		perror("copyfile UNPACK");
		copyfile_state_free(state);
		return 1;
	}
	copyfile_state_free(state);

	ssize_t attr_len = getxattr(TEST_DST_PATH, TEST_ATTR_NAME, val_buf, sizeof(val_buf) - 1, 0, 0);
	if (attr_len < 0) {
		perror("getxattr restored");
		return 1;
	}
	val_buf[attr_len] = '\0';
	if (strcmp(val_buf, TEST_ATTR_VALUE) != 0) {
		fprintf(stderr, "xattr mismatch: expected '%s', got '%s'\n", TEST_ATTR_VALUE, val_buf);
		return 1;
	}
	printf("Test 1 PASSED: xattr preserved accurately across pack & unpack\n\n");

	printf("--- Test 2: Unpack AppleDouble with empty resource fork header ---\n");
	struct stat ad_sb;
	if (stat(TEST_AD_PATH, &ad_sb) < 0) {
		perror("stat AD");
		return 1;
	}
	uint32_t rsrc_offset = (uint32_t)ad_sb.st_size;
	uint32_t rsrc_len = sizeof(empty_rsrcfork_header);

	int ad_fd = open(TEST_AD_PATH, O_RDWR);
	if (ad_fd < 0) {
		perror("open AD for writing rsrc");
		return 1;
	}
	lseek(ad_fd, rsrc_offset, SEEK_SET);
	if (write(ad_fd, &empty_rsrcfork_header, rsrc_len) != (ssize_t)rsrc_len) {
		perror("write rsrcfork header");
		close(ad_fd);
		return 1;
	}
	uint32_t be_type = OSSwapHostToBigInt32(2);
	uint32_t be_offset = OSSwapHostToBigInt32(rsrc_offset);
	uint32_t be_len = OSSwapHostToBigInt32(rsrc_len);
	pwrite(ad_fd, &be_type, 4, 38);
	pwrite(ad_fd, &be_offset, 4, 42);
	pwrite(ad_fd, &be_len, 4, 46);
	close(ad_fd);

	state = copyfile_state_alloc();
	ret = copyfile(TEST_AD_PATH, TEST_DST_PATH, state, COPYFILE_UNPACK | COPYFILE_XATTR);
	copyfile_state_free(state);
	if (ret != 0) {
		fprintf(stderr, "copyfile UNPACK failed with empty resource fork: ret = %d\n", ret);
		return 1;
	}
	printf("Test 2 PASSED: empty resource fork successfully treated as non-fatal\n\n");

	printf("--- Test 3: Unpack AppleDouble with non-empty resource fork ---\n");
	uint32_t oversize_len = 70000;
	char *oversize_buf = calloc(1, oversize_len);
	if (!oversize_buf) {
		perror("calloc oversize_buf");
		return 1;
	}
	memcpy(oversize_buf, &empty_rsrcfork_header, sizeof(empty_rsrcfork_header));
	rsrcfork_header_t *rh = (rsrcfork_header_t *)oversize_buf;
	rh->fh_DataLength = OSSwapHostToBigInt32(oversize_len - sizeof(empty_rsrcfork_header));

	ad_fd = open(TEST_AD_PATH, O_RDWR);
	if (ad_fd < 0) {
		perror("open AD for oversize rsrc");
		free(oversize_buf);
		return 1;
	}
	lseek(ad_fd, rsrc_offset, SEEK_SET);
	if (write(ad_fd, oversize_buf, oversize_len) != (ssize_t)oversize_len) {
		perror("write oversize rsrc");
		close(ad_fd);
		free(oversize_buf);
		return 1;
	}
	uint32_t be_oversize_len = OSSwapHostToBigInt32(oversize_len);
	pwrite(ad_fd, &be_oversize_len, 4, 46);
	close(ad_fd);
	free(oversize_buf);

	state = copyfile_state_alloc();
	ret = copyfile(TEST_AD_PATH, TEST_DST_PATH, state, COPYFILE_UNPACK | COPYFILE_XATTR);
	copyfile_state_free(state);
	if (ret == 0) {
		fprintf(stderr, "Test 3 FAILED: copyfile UNPACK falsely reported success on non-empty resource fork!\n");
		return 1;
	}
	printf("Test 3 PASSED: non-empty resource fork correctly failed with ret = %d (no false success/data loss)\n\n", ret);

	cleanup_files();
	printf("All copyfile_unpack tests PASSED!\n");
	return 0;
}
