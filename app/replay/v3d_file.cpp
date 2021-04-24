
/* wrap around the fatfs */
static struct file* file_open(const char *path, int flags, int rights) {
	struct file *filp = NULL;
	mm_segment_t oldfs;
	int err = 0;

	oldfs = get_fs();
	// set_fs(get_ds());
	set_fs(KERNEL_DS);
	filp = filp_open(path, flags, rights);
	set_fs(oldfs);
	if (IS_ERR(filp)) {
		err = PTR_ERR(filp);
		return NULL;
	}
	return filp;
}

static int __maybe_unused file_read(struct file *file,
		unsigned long long offset, void *data, unsigned int size)
{
	mm_segment_t oldfs;
	int ret;
	oldfs = get_fs();
	set_fs(KERNEL_DS);

	ret = kernel_read(file, data, size, &offset);
	BUG_ON(ret != (ssize_t)size);

	set_fs(oldfs);
	return ret;
}

static void file_close(struct file *file) {
	filp_close(file, NULL);
}
