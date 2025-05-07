static u64
cmd_hash(String8 s)
{
	u64 h, i;

	for (i = 0; i < s.len; i++)
		h = ((h << 5) + h) + s.str[i];
	return h;
}

static CmdOpt **
cmd_slot(Cmd *c, String8 s)
{
	CmdOpt **slot;
	u64 h, bucket;

	slot = NULL;
	if (c->opt_table_size != 0) {
		h = cmd_hash(s);
		bucket = h % c->opt_table_size;
		slot = &c->opt_table[bucket];
	}
	return slot;
}

static CmdOpt *
cmd_slot_to_opt(CmdOpt **slot, String8 s)
{
	CmdOpt *opt, *v;

	opt = NULL;
	for (v = *slot; v != NULL; v = v->hash_next)
		if (str8_cmp(s, v->str, 0)) {
			opt = v;
			break;
		}
	return opt;
}

static void
cmd_push_opt(CmdOptList *list, CmdOpt *v)
{
	SLL_QUEUE_PUSH(list->start, list->end, v);
	list->cnt++;
}

static CmdOpt *
cmd_insert_opt(Arena *a, Cmd *c, String8 s, String8List vals)
{
	CmdOpt *v, *existing;
	CmdOpt **slot;
	StringJoin join;

	v = NULL;
	slot = cmd_slot(c, s);
	existing = cmd_slot_to_opt(slot, s);
	if (existing != NULL)
		v = existing;
	else {
		v = push_array(a, CmdOpt, 1);
		v->hash_next = *slot;
		v->hash = cmd_hash(s);
		v->str = push_str8_copy(a, s);
		v->vals = vals;
		join.pre = str8_lit("");
		join.sep = str8_lit(",");
		join.post = str8_lit("");
		v->val = str8_list_join(a, &v->vals, &join);
		*slot = v;
		cmd_push_opt(&c->opts, v);
	}
	return v;
}

static Cmd
cmd_parse(Arena *a, String8List args)
{
	Cmd parsed;
	b32 after_pass, first_pass, is_opt;
	String8Node *node, *next;
	String8 opt_name;
	String8List opt_vals;
	u64 arg_pos, i;

	memset(&parsed, 0, sizeof(parsed));
	parsed.exe = args.start->str;
	parsed.opt_table_size = 4096;
	parsed.opt_table = push_array(a, CmdOpt *, parsed.opt_table_size);
	after_pass = 0;
	first_pass = 1;
	for (node = args.start->next, next = NULL; node != NULL; node = next) {
		next = node->next;
		opt_name = node->str;
		is_opt = 1;
		if (!after_pass) {
			if (str8_cmp(node->str, str8_lit("--"), 0)) {
				after_pass = 1;
				is_opt = 0;
			} else if (str8_cmp(str8_prefix(node->str, 2), str8_lit("--"), 0))
				opt_name = str8_skip(opt_name, 2);
			else if (str8_cmp(str8_prefix(node->str, 1), str8_lit("-"), 0))
				opt_name = str8_skip(opt_name, 1);
			else
				is_opt = 0;
		} else
			is_opt = 0;
		if (is_opt) {
			memset(&opt_vals, 0, sizeof(opt_vals));
			arg_pos = str8_index(opt_name, 0, str8_lit("="), 0);
			if (arg_pos < opt_name.len) {
				str8_list_push(a, &opt_vals, str8_skip(opt_name, arg_pos + 1));
				opt_name = str8_prefix(opt_name, arg_pos);
			} else if (next != NULL && next->str.len > 0 && next->str.str[0] != '-') {
				str8_list_push(a, &opt_vals, next->str);
				next = next->next;
			}
			cmd_insert_opt(a, &parsed, opt_name, opt_vals);
		} else {
			if (!str8_cmp(node->str, str8_lit("--"), 0))
				after_pass = 1;
			if (after_pass || !first_pass)
				str8_list_push(a, &parsed.inputs, node->str);
			first_pass = 0;
		}
	}
	parsed.argc = args.node_cnt;
	parsed.argv = push_array(a, char *, parsed.argc);
	for (node = args.start, i = 0; node != NULL; node = node->next, i++)
		parsed.argv[i] = (char *)push_str8_copy(a, node->str).str;
	return parsed;
}

static CmdOpt *
cmd_opt(Cmd *c, String8 name)
{
	return cmd_slot_to_opt(cmd_slot(c, name), name);
}

static String8
cmd_str(Cmd *c, String8 name)
{
	String8 s;
	CmdOpt *v;

	s = str8_zero();
	v = cmd_opt(c, name);
	if (v != NULL)
		s = v->val;
	return s;
}

static b32
cmd_has_flag(Cmd *c, String8 name)
{
	return cmd_opt(c, name) != NULL;
}

static b32
cmd_has_arg(Cmd *c, String8 name)
{
	CmdOpt *v;

	v = cmd_opt(c, name);
	return v != NULL && v->vals.node_cnt > 0;
}
