static b32
char_is_upper(u8 c)
{
	return 'A' <= c && c <= 'Z';
}

static b32
char_is_lower(u8 c)
{
	return 'a' <= c && c <= 'z';
}

static u8
char_to_lower(u8 c)
{
	if (char_is_upper(c))
		c += ('a' - 'A');
	return c;
}

static u8
char_to_upper(u8 c)
{
	if (char_is_lower(c))
		c += ('A' - 'a');
	return c;
}

static u64
cstr8_len(u8 *c)
{
	u8 *p;

	for (p = c; *p != 0; p++) {
	}
	return p - c;
}

static String8
str8(u8 *str, u64 len)
{
	String8 s;

	s.str = str;
	s.len = len;
	return s;
}

static String8
str8_rng(u8 *start, u8 *end)
{
	String8 s;

	s.str = start;
	s.len = end - start;
	return s;
}

static String8
str8_zero(void)
{
	String8 s;

	s.str = 0;
	s.len = 0;
	return s;
}

static String8
str8_cstr(char *c)
{
	String8 s;

	s.str = c;
	s.len = cstr8_len(c);
	return s;
}

static Rng1U64
rng1u64(u64 min, u64 max)
{
	Rng1U64 r;

	r.min = min;
	r.max = max;
	if (r.min > r.max)
		SWAP(u64, r.min, r.max);
	return r;
}

static u64
dim1u64(Rng1U64 r)
{
	return r.max > r.min ? (r.max - r.min) : 0;
}

static b32
str8_cmp(String8 a, String8 b, u32 flags)
{
	b32 ok, case_insensitive;
	u64 len, i;
	u8 at, bt;

	ok = 0;
	if (a.len == b.len && flags == 0)
		ok = memcmp(a.str, b.str, b.len) == 0;
	else if (a.len == b.len || (flags & STRING_CMP_FLAGS_RIGHT_SIDE_SLOPPY)) {
		case_insensitive = (flags & STRING_CMP_FLAGS_CASE_INSENSITIVE);
		len = MIN(a.len, b.len);
		ok = 1;
		for (i = 0; i < len; i++) {
			at = a.str[i];
			bt = b.str[i];
			if (case_insensitive) {
				at = char_to_upper(at);
				bt = char_to_upper(bt);
			}
			if (at != bt) {
				ok = 0;
				break;
			}
		}
	}
	return ok;
}

static u64
str8_index(String8 s, u64 start_pos, String8 needle, u32 flags)
{
	u8 *p, *s_end, n0_adj, c;
	u64 stop_len, i;
	String8 tail, haystack_rest;
	u32 adj_flags;

	if (needle.len == 0)
		return s.len;
	p = s.str + start_pos;
	stop_len = s.len - needle.len;
	s_end = s.str + s.len;
	tail = str8_skip(needle, 1);
	adj_flags = flags | STRING_CMP_FLAGS_RIGHT_SIDE_SLOPPY;
	n0_adj = needle.str[0];
	if (adj_flags & STRING_CMP_FLAGS_CASE_INSENSITIVE)
		n0_adj = char_to_upper(n0_adj);
	for (; p - s.str <= stop_len; p++) {
		c = *p;
		if (adj_flags & STRING_CMP_FLAGS_CASE_INSENSITIVE)
			c = char_to_upper(c);
		if (c == n0_adj) {
			haystack_rest = str8_rng(p + 1, s_end);
			if (str8_cmp(haystack_rest, tail, adj_flags))
				break;
		}
	}
	i = s.len;
	if (p - s.str <= stop_len)
		i = p - s.str;
	return i;
}

static u64
str8_rindex(String8 s, u64 start_pos, String8 needle, u32 flags)
{
	u64 idx;
	s64 i;
	Rng1U64 r;
	String8 haystack;

	idx = 0;
	for (i = s.len - start_pos - needle.len; i >= 0; i--) {
		r = rng1u64(i, i + needle.len);
		haystack = str8_substr(s, r);
		if (str8_cmp(haystack, needle, flags)) {
			idx = i;
			break;
		}
	}
	return idx;
}

static String8
str8_substr(String8 s, Rng1U64 r)
{
	r.min = MIN(r.min, s.len);
	r.max = MIN(r.max, s.len);
	s.str += r.min;
	s.len = dim1u64(r);
	return s;
}

static String8
str8_prefix(String8 s, u64 len)
{
	s.len = MIN(len, s.len);
	return s;
}

static String8
str8_suffix(String8 s, u64 len)
{
	len = MIN(len, s.len);
	s.str = s.str + s.len - len;
	s.len = len;
	return s;
}

static String8
str8_skip(String8 s, u64 len)
{
	len = MIN(len, s.len);
	s.str += len;
	s.len -= len;
	return s;
}

static String8
push_str8_cat(Arena *a, String8 s1, String8 s2)
{
	String8 str;

	str.len = s1.len + s2.len;
	str.str = push_array_no_zero(a, u8, str.len + 1);
	memcpy(str.str, s1.str, s1.len);
	memcpy(str.str + s1.len, s2.str, s2.len);
	str.str[str.len] = 0;
	return str;
}

static String8
push_str8_copy(Arena *a, String8 s)
{
	String8 str;

	str.len = s.len;
	str.str = push_array_no_zero(a, u8, str.len + 1);
	memcpy(str.str, s.str, s.len);
	str.str[str.len] = 0;
	return str;
}

static String8
push_str8fv(Arena *a, char *fmt, va_list args)
{
	va_list args2;
	u32 nbytes;
	String8 s;

	va_copy(args2, args);
	nbytes = vsnprintf(0, 0, fmt, args) + 1;
	s.str = push_array_no_zero(a, u8, nbytes);
	s.len = vsnprintf(s.str, nbytes, fmt, args2);
	s.str[s.len] = 0;
	va_end(args2);
	return s;
}

static String8
push_str8f(Arena *a, char *fmt, ...)
{
	va_list args;
	String8 s;

	va_start(args, fmt);
	s = push_str8fv(a, fmt, args);
	va_end(args);
	return s;
}

static b32
str8_is_int(String8 s, u32 radix)
{
	b32 ok;
	u64 i;
	u8 c;

	ok = 0;
	if (s.len > 0) {
		if (1 < radix && radix <= 16) {
			ok = 1;
			for (i = 0; i < s.len; i++) {
				c = s.str[i];
				if (!(c < 0x80) || int_symbol_reverse[c] >= radix) {
					ok = 0;
					break;
				}
			}
		}
	}
	return ok;
}

static u64
str8_to_u64(String8 s, u32 radix)
{
	u64 x, i;

	x = 0;
	if (1 < radix && radix <= 16) {
		for (i = 0; i < s.len; i++) {
			x *= radix;
			x += int_symbol_reverse[s.str[i] & 0x7f];
		}
	}
	return x;
}

static b32
str8_to_u64_ok(String8 s, u64 *x)
{
	b32 ok = 0;
	String8 hex_part, bin_part, oct_part;

	if (str8_is_int(s, 10)) {
		ok = 1;
		*x = str8_to_u64(s, 10);
	} else {
		if (s.len >= 2 && str8_cmp(str8_prefix(s, 2), str8_lit("0x"), 0)) {
			hex_part = str8_skip(s, 2);
			if (str8_is_int(hex_part, 16)) {
				ok = 1;
				*x = str8_to_u64(hex_part, 16);
			}
		} else if (s.len >= 2 && str8_cmp(str8_prefix(s, 2), str8_lit("0b"), 0)) {
			bin_part = str8_skip(s, 2);
			if (str8_is_int(bin_part, 2)) {
				ok = 1;
				*x = str8_to_u64(bin_part, 2);
			}
		} else if (s.len >= 1 && s.str[0] == '0') {
			oct_part = str8_skip(s, 1);
			if (str8_is_int(oct_part, 8)) {
				ok = 1;
				*x = str8_to_u64(oct_part, 8);
			}
		}
	}
	return ok;
}

static String8
u64_to_str8(Arena *a, u64 v, u32 radix, u8 min_digits, u8 digit_sep)
{
	String8 s, pre;
	u64 digit_group_size, nleading_0s, ndigits, v_rem, nseps, digits_until_sep, i, leading_0_idx;

	s = str8_zero();
	pre = str8_zero();
	switch (radix) {
		case 16:
			pre = str8_lit("0x");
			break;
		case 8:
			pre = str8_lit("0");
			break;
		case 2:
			pre = str8_lit("0b");
			break;
		default:
			break;
	}
	digit_group_size = 3;
	switch (radix) {
		default:
			break;
		case 2:
		case 8:
		case 16:
			digit_group_size = 4;
			break;
	}
	nleading_0s = 0;
	ndigits = 1;
	v_rem = v;
	for (;;) {
		v_rem /= radix;
		if (v_rem == 0)
			break;
		ndigits++;
	}
	nleading_0s = (min_digits > ndigits) ? min_digits - ndigits : 0;
	nseps = 0;
	if (digit_sep != 0) {
		nseps = (ndigits + nleading_0s) / digit_group_size;
		if (nseps > 0 && (ndigits + nleading_0s) % digit_group_size == 0)
			nseps--;
	}
	s.len = pre.len + nleading_0s + nseps + ndigits;
	s.str = push_array_no_zero(a, u8, s.len + 1);
	s.str[s.len] = 0;
	v_rem = v;
	digits_until_sep = digit_group_size;
	for (i = 0; i < s.len; i++) {
		if (digits_until_sep == 0 && digit_sep != 0) {
			s.str[s.len - i - 1] = digit_sep;
			digits_until_sep = digit_group_size + 1;
		} else {
			s.str[s.len - i - 1] = char_to_lower(int_symbols[v_rem % radix]);
			v_rem /= radix;
		}
		digits_until_sep--;
		if (v_rem == 0)
			break;
	}
	for (leading_0_idx = 0; leading_0_idx < nleading_0s; leading_0_idx++)
		s.str[pre.len + leading_0_idx] = '0';
	if (pre.len != 0)
		memcpy(s.str, pre.str, pre.len);
	return s;
}

static String8Node *
str8_list_push_node_set_str(String8List *list, String8Node *node, String8 s)
{
	SLL_QUEUE_PUSH(list->start, list->end, node);
	list->node_cnt++;
	list->total_len += s.len;
	node->str = s;
	return node;
}

static String8Node *
str8_list_push(Arena *a, String8List *list, String8 s)
{
	String8Node *node;

	node = push_array_no_zero(a, String8Node, 1);
	str8_list_push_node_set_str(list, node, s);
	return node;
}

static String8List
str8_split(Arena *a, String8 s, u8 *split, u64 split_len, u32 flags)
{
	String8List list;
	b32 keep_empty, is_split;
	u8 *p, *end, *start, c;
	u64 i;
	String8 ss;

	memset(&list, 0, sizeof(list));
	keep_empty = flags & STRING_SPLIT_FLAGS_KEEP_EMPTY;
	p = s.str;
	end = s.str + s.len;
	for (; p < end;) {
		start = p;
		for (; p < end; p++) {
			c = *p;
			is_split = 0;
			for (i = 0; i < split_len; i++) {
				if (split[i] == c) {
					is_split = 1;
					break;
				}
			}
			if (is_split)
				break;
		}
		ss = str8_rng(start, p);
		if (keep_empty || ss.len > 0)
			str8_list_push(a, &list, ss);
		p++;
	}
	return list;
}

static String8
str8_list_join(Arena *a, String8List *list, StringJoin *opts)
{
	StringJoin join;
	u64 sep_cnt;
	String8 s;
	u8 *p;
	String8Node *node;

	memset(&join, 0, sizeof(join));
	if (opts != NULL)
		memcpy(&join, opts, sizeof(join));
	sep_cnt = 0;
	if (list->node_cnt > 0)
		sep_cnt = list->node_cnt - 1;
	s = str8_zero();
	s.len = join.pre.len + join.post.len + sep_cnt * join.sep.len + list->total_len;
	p = s.str = push_array_no_zero(a, u8, s.len + 1);
	memcpy(p, join.pre.str, join.pre.len);
	p += join.pre.len;
	for (node = list->start; node != NULL; node = node->next) {
		memcpy(p, node->str.str, node->str.len);
		p += node->str.len;
		if (node->next != NULL) {
			memcpy(p, join.sep.str, join.sep.len);
			p += join.sep.len;
		}
	}
	memcpy(p, join.post.str, join.post.len);
	p += join.post.len;
	*p = 0;
	return s;
}

static String8Array
str8_list_to_array(Arena *a, String8List *list)
{
	String8Array array;
	u64 i;
	String8Node *node;

	array.cnt = list->node_cnt;
	array.v = push_array_no_zero(a, String8, array.cnt);
	i = 0;
	for (node = list->start; node != NULL; node = node->next, i++)
		array.v[i] = node->str;
	return array;
}

static String8Array
str8_array_reserve(Arena *a, u64 cnt)
{
	String8Array array;

	array.cnt = 0;
	array.v = push_array(a, String8, cnt);
	return array;
}

static String8
str8_dirname(String8 s)
{
	u8 *p;

	if (s.len > 0) {
		for (p = s.str + s.len - 1; p >= s.str; p--)
			if (*p == '/')
				break;
		if (p >= s.str)
			s.len = p - s.str;
		else
			s.len = 0;
	}
	return s;
}

static String8
str8_basename(String8 s)
{
	u8 *p;

	if (s.len > 0) {
		for (p = s.str + s.len - 1; p >= s.str; p--)
			if (*p == '/')
				break;
		if (p >= s.str) {
			p++;
			s.len = s.str + s.len - p;
			s.str = p;
		}
	}
	return s;
}

static String8
str8_prefix_ext(String8 s)
{
	String8 pre;
	u64 p;

	pre = s;
	for (p = s.len; p > 0;) {
		p--;
		if (s.str[p] == '.') {
			pre = str8_prefix(s, p);
			break;
		}
	}
	return pre;
}

static String8
str8_ext(String8 s)
{
	String8 ext;
	u64 p;

	ext = s;
	for (p = s.len; p > 0;) {
		p--;
		if (s.str[p] == '.') {
			ext = str8_skip(s, p + 1);
			break;
		}
	}
	return ext;
}
