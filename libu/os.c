static String8List
os_args(Arena *a, int argc, char **argv)
{
	String8List list;
	int i;
	String8 s;

	memset(&list, 0, sizeof(list));
	for (i = 0; i < argc; i++) {
		s = str8_cstr(argv[i]);
		str8_list_push(a, &list, s);
	}
	return list;
}

static String8
os_read_file(Arena *a, String8 path)
{
	u64 fd;
	FileProperties props;
	String8 data;

	fd = os_open(path, O_RDONLY);
	props = os_fstat(fd);
	data = os_read_file_range(a, fd, rng1u64(0, props.size));
	os_close(fd);
	return data;
}

static b32
os_write_file(String8 path, String8 data)
{
	b32 ok;
	u64 fd;

	ok = 0;
	fd = os_open(path, O_WRONLY);
	if (fd != 0) {
		ok = 1;
		os_write(fd, rng1u64(0, data.len), data.str);
		os_close(fd);
	}
	return ok;
}

static b32
os_append_file(String8 path, String8 data)
{
	b32 ok;
	u64 fd, pos;

	ok = 0;
	if (data.len != 0) {
		fd = os_open(path, O_WRONLY | O_APPEND | O_CREAT);
		if (fd != 0) {
			ok = 1;
			pos = os_fstat(fd).size;
			os_write(fd, rng1u64(pos, pos + data.len), data.str);
			os_close(fd);
		}
	}
	return ok;
}

static String8
os_read_file_range(Arena *a, u64 fd, Rng1U64 r)
{
	u64 pre, nread;
	String8 s;

	pre = arena_pos(a);
	s = str8_zero();
	s.len = dim1u64(r);
	s.str = push_array_no_zero(a, u8, s.len);
	nread = os_read(fd, r, s.str);
	if (nread < s.len) {
		arena_pop_to(a, pre + nread);
		s.len = nread;
	}
	return s;
}

static DateTime
os_tm_to_datetime(tm t, u32 msec)
{
	DateTime dt;

	dt.msec = msec;
	dt.sec = t.tm_sec;
	dt.min = t.tm_min;
	dt.hour = t.tm_hour;
	dt.day = t.tm_mday - 1;
	dt.mon = t.tm_mon;
	dt.year = t.tm_year + 1900;
	return dt;
}

static tm
os_datetime_to_tm(DateTime dt)
{
	tm t;

	memset(&t, 0, sizeof(t));
	t.tm_sec = dt.sec;
	t.tm_min = dt.min;
	t.tm_hour = dt.hour;
	t.tm_mday = dt.day + 1;
	t.tm_mon = dt.mon;
	t.tm_year = dt.year - 1900;
	return t;
}

static timespec
os_datetime_to_timespec(DateTime dt)
{
	tm t;
	time_t sec;
	timespec ts;

	t = os_datetime_to_tm(dt);
	sec = timegm(&t);
	memset(&ts, 0, sizeof(ts));
	ts.tv_sec = sec;
	return ts;
}

static DenseTime
os_timespec_to_densetime(timespec ts)
{
	tm t;
	DateTime dt;

	gmtime_r(&ts.tv_sec, &t);
	dt = os_tm_to_datetime(t, ts.tv_nsec / MILLION(1));
	return date_time_to_dense_time(dt);
}

static FileProperties
os_stat_to_props(struct stat *st)
{
	FileProperties props;

	memset(&props, 0, sizeof(props));
	props.size = st->st_size;
	props.modified = os_timespec_to_densetime(st->st_mtim);
	props.created = os_timespec_to_densetime(st->st_ctim);
	if (st->st_mode & S_IFDIR)
		props.flags |= FILE_PROPERTY_FLAG_IS_DIR;
	return props;
}

static String8
os_cwd(Arena *a)
{
	char *cwd;
	String8 s;

	cwd = getcwd(0, 0);
	s = push_str8_copy(a, str8_cstr(cwd));
	free(cwd);
	return s;
}

static void *
os_reserve(u64 size)
{
	void *p;

	p = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		p = NULL;
	return p;
}

static b32
os_commit(void *p, u64 size)
{
	mprotect(p, size, PROT_READ | PROT_WRITE);
	return 1;
}

static void
os_decommit(void *p, u64 size)
{
	madvise(p, size, MADV_DONTNEED);
	mprotect(p, size, PROT_NONE);
}

static void
os_release(void *p, u64 size)
{
	munmap(p, size);
}

static void *
os_reserve_large(u64 size)
{
	void *p;

	p = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (p == MAP_FAILED)
		p = NULL;
	return p;
}

static u64
os_open(String8 path, int flags)
{
	Temp scratch;
	String8 p;
	int fd;
	u64 handle;

	scratch = temp_begin(arena);
	p = push_str8_copy(scratch.a, path);
	fd = open(p.str, flags, 0755);
	handle = 0;
	if (fd != -1)
		handle = fd;
	temp_end(scratch);
	return handle;
}

static void
os_close(u64 fd)
{
	if (fd == 0)
		return;
	close(fd);
}

static u64
os_read(u64 fd, Rng1U64 r, void *out)
{
	u64 size, nread, nleft;
	int n;

	if (fd == 0)
		return 0;
	size = dim1u64(r);
	nread = 0;
	nleft = size;
	for (; nleft > 0;) {
		n = pread(fd, out + nread, nleft, r.min + nread);
		if (n >= 0) {
			nread += n;
			nleft -= n;
		} else if (errno != EINTR)
			break;
	}
	return nread;
}

static u64
os_write(u64 fd, Rng1U64 r, void *data)
{
	u64 size, nwrite, nleft;
	int n;

	if (fd == 0)
		return 0;
	size = dim1u64(r);
	nwrite = 0;
	nleft = size;
	for (; nleft > 0;) {
		n = pwrite(fd, data + nwrite, nleft, r.min + nwrite);
		if (n >= 0) {
			nwrite += n;
			nleft -= n;
		} else if (errno != EINTR)
			break;
	}
	return nwrite;
}

static b32
os_set_times(u64 fd, DateTime dt)
{
	timespec ts, times[2];

	if (fd == 0)
		return 0;
	ts = os_datetime_to_timespec(dt);
	times[0] = ts;
	times[1] = ts;
	return futimens(fd, times) != -1;
}

static FileProperties
os_fstat(u64 fd)
{
	struct stat st;
	FileProperties props;

	memset(&props, 0, sizeof(props));
	if (fd == 0)
		return props;
	if (fstat(fd, &st) != -1)
		props = os_stat_to_props(&st);
	return props;
}

static b32
os_remove(String8 path)
{
	Temp scratch;
	b32 ok;
	String8 p;

	scratch = temp_begin(arena);
	ok = 0;
	p = push_str8_copy(scratch.a, path);
	if (remove(p.str) != -1)
		ok = 1;
	temp_end(scratch);
	return ok;
}

static String8
os_abspath(String8 path)
{
	Temp scratch;
	String8 p, s;
	char buf[PATH_MAX];

	scratch = temp_begin(arena);
	p = push_str8_copy(scratch.a, path);
	if (realpath(p.str, buf) == NULL) {
		temp_end(scratch);
		return str8_zero();
	}
	s = push_str8_copy(scratch.a, str8_cstr(buf));
	temp_end(scratch);
	return s;
}

static b32
os_file_exists(String8 path)
{
	Temp scratch;
	String8 p;
	b32 ok;

	scratch = temp_begin(arena);
	p = push_str8_copy(scratch.a, path);
	ok = 0;
	if (access(p.str, F_OK) == 0)
		ok = 1;
	temp_end(scratch);
	return ok;
}

static b32
os_dir_exists(String8 path)
{
	Temp scratch;
	String8 p;
	b32 ok;
	DIR *d;

	scratch = temp_begin(arena);
	p = push_str8_copy(scratch.a, path);
	ok = 0;
	d = opendir(p.str);
	if (d != NULL) {
		closedir(d);
		ok = 1;
	}
	temp_end(scratch);
	return ok;
}

static FileProperties
os_stat(String8 path)
{
	Temp scratch;
	String8 p;
	struct stat st;
	FileProperties props;

	scratch = temp_begin(arena);
	p = push_str8_copy(scratch.a, path);
	memset(&props, 0, sizeof(props));
	if (stat(p.str, &st) != -1)
		props = os_stat_to_props(&st);
	temp_end(scratch);
	return props;
}

static b32
os_mkdir(String8 path)
{
	Temp scratch;
	String8 p;
	b32 ok;

	scratch = temp_begin(arena);
	p = push_str8_copy(scratch.a, path);
	ok = 0;
	if (mkdir(p.str, 0755) != -1)
		ok = 1;
	temp_end(scratch);
	return ok;
}

static u64
os_now_us(void)
{
	timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * MILLION(1) + (ts.tv_nsec / THOUSAND(1));
}

static u32
os_now_unix(void)
{
	return time(0);
}

static DateTime
os_now_utc(void)
{
	time_t t;
	tm ut;

	t = 0;
	time(&t);
	gmtime_r(&t, &ut);
	return os_tm_to_datetime(ut, 0);
}

static DateTime
os_local_to_utc(DateTime dt)
{
	tm lt, ut;
	time_t t;

	lt = os_datetime_to_tm(dt);
	lt.tm_isdst = -1;
	t = mktime(&lt);
	gmtime_r(&t, &ut);
	return os_tm_to_datetime(ut, 0);
}

static DateTime
os_utc_to_local(DateTime dt)
{
	tm ut, lt;
	time_t t;

	ut = os_datetime_to_tm(dt);
	ut.tm_isdst = -1;
	t = timegm(&ut);
	localtime_r(&t, &lt);
	return os_tm_to_datetime(lt, 0);
}

static void
os_sleep_ms(u32 msec)
{
	usleep(msec * THOUSAND(1));
}
