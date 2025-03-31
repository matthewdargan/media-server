internal b32 char_is_upper(u8 c) {
    return 'A' <= c && c <= 'Z';
}

internal b32 char_is_lower(u8 c) {
    return 'a' <= c && c <= 'z';
}

internal u8 char_to_upper(u8 c) {
    if (char_is_lower(c)) {
        c += ('A' - 'a');
    }
    return c;
}

internal u64 cstring8_length(u8 *c) {
    u8 *p = c;
    for (; *p != 0; ++p) {
    }
    return p - c;
}

internal string8 str8(u8 *str, u64 size) {
    return (string8){.str = str, .size = size};
}

internal string8 str8_range(u8 *first, u8 *one_past_last) {
    return (string8){.str = first, .size = (u64)(one_past_last - first)};
}

internal string8 str8_zero(void) {
    return (string8){0};
}

internal string8 str8_cstring(char *c) {
    return (string8){.str = (u8 *)c, .size = cstring8_length((u8 *)c)};
}

internal rng1u64 rng_1u64(u64 min, u64 max) {
    rng1u64 r = {.min = min, .max = max};
    if (r.min > r.max) {
        SWAP(u64, r.min, r.max);
    }
    return r;
}

internal u64 dim_1u64(rng1u64 r) {
    return r.max > r.min ? (r.max - r.min) : 0;
}

internal string8 str8_substr(string8 str, rng1u64 range) {
    range.min = MIN(range.min, str.size);
    range.max = MIN(range.max, str.size);
    str.str += range.min;
    str.size = dim_1u64(range);
    return str;
}

internal string8 str8_skip(string8 str, u64 amt) {
    amt = MIN(amt, str.size);
    str.str += amt;
    str.size -= amt;
    return str;
}

internal b32 str8_match(string8 a, string8 b, string_match_flags flags) {
    b32 result = 0;
    if (a.size == b.size && flags == 0) {
        result = MEMORY_MATCH(a.str, b.str, b.size);
    } else if (a.size == b.size || (flags & STRING_MATCH_FLAGS_RIGHT_SIDE_SLOPPY)) {
        b32 case_insensitive = (flags & STRING_MATCH_FLAGS_CASE_INSENSITIVE);
        u64 size = MIN(a.size, b.size);
        result = 1;
        for (u64 i = 0; i < size; ++i) {
            u8 at = a.str[i];
            u8 bt = b.str[i];
            if (case_insensitive) {
                at = char_to_upper(at);
                bt = char_to_upper(bt);
            }
            if (at != bt) {
                result = 0;
                break;
            }
        }
    }
    return result;
}

internal u64 str8_find_needle(string8 string, u64 start_pos, string8 needle, string_match_flags flags) {
    u8 *p = string.str + start_pos;
    u64 stop_offset = MAX(string.size + 1, needle.size) - needle.size;
    u8 *stop_p = string.str + stop_offset;
    if (needle.size > 0) {
        u8 *string_opl = string.str + string.size;
        string8 needle_tail = str8_skip(needle, 1);
        string_match_flags adjusted_flags = flags | STRING_MATCH_FLAGS_RIGHT_SIDE_SLOPPY;
        u8 needle_first_char_adjusted = needle.str[0];
        if (adjusted_flags & STRING_MATCH_FLAGS_CASE_INSENSITIVE) {
            needle_first_char_adjusted = char_to_upper(needle_first_char_adjusted);
        }
        for (; p < stop_p; ++p) {
            u8 haystack_char_adjusted = *p;
            if (adjusted_flags & STRING_MATCH_FLAGS_CASE_INSENSITIVE) {
                haystack_char_adjusted = char_to_upper(haystack_char_adjusted);
            }
            if (haystack_char_adjusted == needle_first_char_adjusted) {
                if (str8_match(str8_range(p + 1, string_opl), needle_tail, adjusted_flags)) {
                    break;
                }
            }
        }
    }
    u64 result = string.size;
    if (p < stop_p) {
        result = (u64)(p - string.str);
    }
    return result;
}

internal u64 str8_find_needle_reverse(string8 string, u64 start_pos, string8 needle, string_match_flags flags) {
    u64 result = 0;
    for (s64 i = string.size - start_pos - needle.size; i >= 0; --i) {
        string8 haystack = str8_substr(string, rng_1u64(i, i + needle.size));
        if (str8_match(haystack, needle, flags)) {
            result = (u64)i + needle.size;
            break;
        }
    }
    return result;
}

internal string8 push_str8_cat(arena *a, string8 s1, string8 s2) {
    string8 str;
    str.size = s1.size + s2.size;
    str.str = push_array_no_zero(a, u8, str.size + 1);
    memcpy(str.str, s1.str, s1.size);
    memcpy(str.str + s1.size, s2.str, s2.size);
    str.str[str.size] = 0;
    return str;
}

internal string8 push_str8_copy(arena *a, string8 s) {
    string8 str;
    str.size = s.size;
    str.str = push_array_no_zero(a, u8, str.size + 1);
    memcpy(str.str, s.str, s.size);
    str.str[str.size] = 0;
    return str;
}

internal string8 push_str8fv(arena *a, char *fmt, va_list args) {
    va_list args2;
    va_copy(args2, args);
    u32 needed_bytes = vsnprintf(0, 0, fmt, args) + 1;
    string8 result = {.str = push_array_no_zero(a, u8, needed_bytes),
                      .size = (u64)vsnprintf((char *)result.str, needed_bytes, fmt, args2)};
    result.str[result.size] = 0;
    va_end(args2);
    return result;
}

internal string8 push_str8f(arena *a, char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    string8 result = push_str8fv(a, fmt, args);
    va_end(args);
    return result;
}

internal u64 u64_from_str8(string8 string, u32 radix) {
    u64 x = 0;
    if (1 < radix && radix <= 16) {
        for (u64 i = 0; i < string.size; ++i) {
            x *= radix;
            x += integer_symbol_reverse[string.str[i] & 0x7F];
        }
    }
    return x;
}
