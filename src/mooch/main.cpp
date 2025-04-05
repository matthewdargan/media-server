#include <curl/curl.h>
#include <dirent.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <sys/mman.h>

#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>

// clang-format off
#include "base/inc.h"
#include "os/inc.h"
#include "base/inc.c"
#include "os/inc.c"
// clang-format on

#define CHUNK_SIZE MB(1)
#define RESULTS_PER_PAGE 75

typedef struct params params;
struct params {
    u8 filter;
    string8 category;
    string8 user;
    string8 sort;
    string8 order;
    string8 query;
};

typedef struct torrent torrent;
struct torrent {
    string8 title;
    string8 magnet;
};

typedef struct torrent_array torrent_array;
struct torrent_array {
    u64 count;
    torrent *v;
};

internal void usage(void) {
    fprintf(stderr,
            "usage: mooch [-f filter] [-c category] [-u user] [-s sort] "
            "[-o order] query\n");
    exit(2);
}

read_only global u8 filters[] = {'0', '1', '2'};
read_only global string8 categories[] = {
    str8_lit("0_0"), str8_lit("1_0"), str8_lit("1_1"), str8_lit("1_2"), str8_lit("1_3"), str8_lit("1_4"),
    str8_lit("2_0"), str8_lit("2_1"), str8_lit("2_2"), str8_lit("3_0"), str8_lit("3_1"), str8_lit("3_2"),
    str8_lit("3_3"), str8_lit("4_0"), str8_lit("4_1"), str8_lit("4_2"), str8_lit("4_3"), str8_lit("4_4"),
    str8_lit("5_0"), str8_lit("5_1"), str8_lit("5_2"), str8_lit("6_0"), str8_lit("6_1"), str8_lit("6_2"),
};
read_only global string8 sorts[] = {
    str8_lit("comments"), str8_lit("size"),     str8_lit("id"),
    str8_lit("seeders"),  str8_lit("leechers"), str8_lit("downloads"),
};
read_only global string8 orders[] = {str8_lit("asc"), str8_lit("desc")};

internal b32 validate_params(params ps) {
    b32 filter_valid = 0;
    for (u64 i = 0; i < ARRAY_COUNT(filters); ++i) {
        if (ps.filter == filters[i]) {
            filter_valid = 1;
            break;
        }
    }
    if (!filter_valid) {
        fprintf(stderr, "invalid filter: %c\n", ps.filter);
        return 0;
    }
    b32 category_valid = 0;
    for (u64 i = 0; i < ARRAY_COUNT(categories); ++i) {
        if (str8_match(ps.category, categories[i], 0)) {
            category_valid = 1;
            break;
        }
    }
    if (!category_valid) {
        fprintf(stderr, "invalid category: %s\n", ps.category.str);
        return 0;
    }
    if (ps.sort.size > 0) {
        b32 sort_valid = 0;
        for (u64 i = 0; i < ARRAY_COUNT(sorts); ++i) {
            if (str8_match(ps.sort, sorts[i], 0)) {
                sort_valid = 1;
                break;
            }
        }
        if (!sort_valid) {
            fprintf(stderr, "invalid sort: %s\n", ps.sort.str);
            return 0;
        }
    }
    if (ps.order.size > 0) {
        b32 order_valid = 0;
        for (u64 i = 0; i < ARRAY_COUNT(orders); ++i) {
            if (str8_match(ps.order, orders[i], 0)) {
                order_valid = 1;
                break;
            }
        }
        if (!order_valid) {
            fprintf(stderr, "invalid order: %s\n", ps.order.str);
            return 0;
        }
    }
    return 1;
}

internal size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    u64 real_size = size * nmemb;
    string8 *chunk = (string8 *)userp;
    if (chunk->size + real_size + 1 > CHUNK_SIZE) {
        fprintf(stderr, "mooch: HTTP response too large for chunk\n");
        return 0;
    }
    memcpy(&(chunk->str[chunk->size]), contents, real_size);
    chunk->size += real_size;
    chunk->str[chunk->size] = 0;
    return real_size;
}

internal u64 get_total_pages(htmlDocPtr doc, xmlXPathCompExprPtr pagination_expr) {
    xmlXPathContextPtr context = xmlXPathNewContext(doc);
    if (context == NULL) {
        fprintf(stderr, "mooch: unable to create XPath context\n");
        return 1;
    }
    xmlXPathObjectPtr result = xmlXPathCompiledEval(pagination_expr, context);
    if (result == NULL || xmlXPathNodeSetIsEmpty(result->nodesetval)) {
        return 1;
    }
    xmlNodePtr node = result->nodesetval->nodeTab[0];
    xmlChar *content = xmlNodeGetContent(node);
    string8 results_msg = str8((u8 *)content, xmlStrlen(content));
    string8 of = str8_lit("of ");
    u64 of_pos = str8_find_needle(results_msg, 0, of, 0);
    u64 num_pos = of_pos + of.size;
    u64 space_pos = str8_find_needle(results_msg, num_pos, str8_lit(" "), 0);
    u64 total = 1;
    if (num_pos < results_msg.size && space_pos < results_msg.size) {
        string8 total_str = str8_substr(results_msg, rng_1u64(num_pos, space_pos));
        total = u64_from_str8(total_str, 10);
    }
    return (total + RESULTS_PER_PAGE - 1) / RESULTS_PER_PAGE;
}

internal torrent_array extract_torrents(arena *a, htmlDocPtr doc, xmlXPathCompExprPtr row_expr,
                                        xmlXPathCompExprPtr title_expr, xmlXPathCompExprPtr magnet_expr) {
    torrent_array torrents = {0};
    xmlXPathContextPtr context = xmlXPathNewContext(doc);
    if (context == NULL) {
        fprintf(stderr, "mooch: unable to create XPath context\n");
        return torrents;
    }
    xmlXPathObjectPtr result = xmlXPathCompiledEval(row_expr, context);
    if (result == NULL || xmlXPathNodeSetIsEmpty(result->nodesetval)) {
        return torrents;
    }
    torrents.count = (u64)result->nodesetval->nodeNr;
    torrents.v = push_array_no_zero(a, torrent, torrents.count);
    for (u64 i = 0; i < (u64)result->nodesetval->nodeNr; ++i) {
        xmlNodePtr row = result->nodesetval->nodeTab[i];
        context->node = row;
        xmlXPathObjectPtr title_result = xmlXPathCompiledEval(title_expr, context);
        if (title_result && !xmlXPathNodeSetIsEmpty(title_result->nodesetval)) {
            xmlNodePtr title_node = title_result->nodesetval->nodeTab[0];
            xmlChar *title = xmlNodeGetContent(title_node);
            xmlXPathObjectPtr magnet_result = xmlXPathCompiledEval(magnet_expr, context);
            if (magnet_result && !xmlXPathNodeSetIsEmpty(magnet_result->nodesetval)) {
                xmlNodePtr magnet_node = magnet_result->nodesetval->nodeTab[0];
                xmlChar *magnet_path = xmlNodeGetContent(magnet_node);
                torrents.v[i].title = push_str8_copy(a, str8((u8 *)title, xmlStrlen(title)));
                torrents.v[i].magnet = push_str8_copy(a, str8((u8 *)magnet_path, xmlStrlen(magnet_path)));
            }
        }
    }
    return torrents;
}

internal torrent_array get_torrents(arena *a, params ps) {
    torrent_array torrents = {0};
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "mooch: could not initialize curl\n");
        return torrents;
    }
    char *curl_encoded_query = curl_easy_escape(curl, (const char *)ps.query.str, ps.query.size);
    if (curl_encoded_query == NULL) {
        fprintf(stderr, "mooch: could not URL encode query\n");
        curl_easy_cleanup(curl);
        return torrents;
    }
    string8 chunk = {
        .str = push_array_no_zero(a, u8, CHUNK_SIZE),
        .size = 0,
    };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mooch/1.0");
    string8 encoded_query = push_str8_copy(a, str8_cstring(curl_encoded_query));
    string8 base_url = str8_lit("https://nyaa.si");
    string8 user = str8_zero();
    if (ps.user.size > 0) {
        user = push_str8_cat(a, str8_lit("/user/"), ps.user);
    }
    string8 query = push_str8f(a, (char *)"?f=%c&c=%s&q=%s", ps.filter, ps.category.str, encoded_query.str);
    string8 sort = str8_zero();
    if (ps.sort.size > 0) {
        sort = push_str8_cat(a, str8_lit("&s="), ps.sort);
    }
    string8 order = str8_zero();
    if (ps.order.size > 0) {
        order = push_str8_cat(a, str8_lit("&o="), ps.order);
    }
    string8 url = push_str8_cat(a, base_url, user);
    url = push_str8_cat(a, url, query);
    url = push_str8_cat(a, url, sort);
    url = push_str8_cat(a, url, order);
    xmlXPathCompExprPtr pagination_expr = xmlXPathCompile(BAD_CAST "//div[@class='pagination-page-info']");
    xmlXPathCompExprPtr row_expr = xmlXPathCompile(BAD_CAST "//table/tbody/tr");
    xmlXPathCompExprPtr title_expr = xmlXPathCompile(BAD_CAST "./td[2]/a[last()]");
    xmlXPathCompExprPtr magnet_expr = xmlXPathCompile(BAD_CAST "./td[3]/a[2]/@href");
    for (u64 page = 1, total_pages = 1; page <= total_pages; ++page) {
        string8 query_url = push_str8_cat(a, url, str8_lit("&p="));
        query_url = push_str8_cat(a, query_url, str8_from_u64(a, page, 10, 0, 0));
        chunk.size = 0;
        curl_easy_setopt(curl, CURLOPT_URL, query_url.str);
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "mooch: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            continue;
        }
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code != 200) {
            fprintf(stderr, "mooch: HTTP error %ld for page %zu\n", http_code, page);
            continue;
        }
        htmlDocPtr doc = htmlReadMemory((const char *)chunk.str, chunk.size, (const char *)query_url.str, NULL,
                                        HTML_PARSE_NOWARNING | HTML_PARSE_NOERROR);
        if (doc == NULL) {
            fprintf(stderr, "mooch: failed to parse HTML for page %zu\n", page);
            continue;
        }
        if (page == 1) {
            total_pages = get_total_pages(doc, pagination_expr);
            torrents.v = push_array_no_zero(a, torrent, total_pages * RESULTS_PER_PAGE);
        }
        torrent_array page_torrents = extract_torrents(a, doc, row_expr, title_expr, magnet_expr);
        if (page_torrents.count > 0) {
            memcpy(torrents.v + torrents.count, page_torrents.v, sizeof(torrent) * page_torrents.count);
            torrents.count += page_torrents.count;
        }
    }
    curl_easy_cleanup(curl);
    return torrents;
}

internal void run_editor(arena *a, string8 path) {
    char *editor = getenv("EDITOR");
    if (editor == NULL || strlen(editor) == 0) {
        editor = (char *)"vi";
    }
    string8 cmd = push_str8_cat(a, str8_cstring(editor), str8_lit(" "));
    cmd = push_str8_cat(a, cmd, path);
    int status = system((char *)cmd.str);
    if (status != 0) {
        fprintf(stderr, "mooch: editor command failed with status %d\n", status);
        exit(1);
    }
}

internal torrent_array process_selected_torrents(arena *a, string8 path) {
    torrent_array selected = {0};
    string8 data = os_data_from_file_path(a, path);
    if (data.size == 0) {
        fprintf(stderr, "mooch: could not read file %s\n", path.str);
        return selected;
    }
    for (u64 i = 0; i < data.size; ++i) {
        if (data.str[i] == '\n') {
            ++selected.count;
        }
    }
    selected.v = push_array_no_zero(a, torrent, selected.count);
    selected.count = 0;
    for (string8 line = str8_zero(); data.size > 0;) {
        u64 eol_pos = str8_find_needle(data, 0, str8_lit("\n"), 0);
        if (eol_pos == data.size) {
            line = data;
            data = str8_zero();
        } else {
            line = str8_substr(data, rng_1u64(0, eol_pos));
            data = str8_skip(data, eol_pos + 1);
        }
        u64 space_pos = str8_find_needle_reverse(line, 0, str8_lit(" "), 0);
        if (space_pos > 0) {
            string8 title = str8_substr(line, rng_1u64(0, space_pos - 1));
            string8 magnet = str8_substr(line, rng_1u64(space_pos, line.size));
            selected.v[selected.count].title = push_str8_copy(a, title);
            selected.v[selected.count].magnet = push_str8_copy(a, magnet);
            ++selected.count;
        }
    }
    return selected;
}

internal void download_torrents(torrent_array torrents) {
    if (torrents.count == 0) {
        return;
    }
    libtorrent::settings_pack pack;
    pack.set_int(libtorrent::settings_pack::alert_mask,
                 libtorrent::alert::status_notification | libtorrent::alert::error_notification);
    libtorrent::session session(pack);
    for (u64 i = 0; i < torrents.count; ++i) {
        try {
            libtorrent::add_torrent_params ps;
            libtorrent::error_code ec;
            libtorrent::parse_magnet_uri((char *)torrents.v[i].magnet.str, ps, ec);
            if (ec) {
                fprintf(stderr, "mooch: failed to parse magnet URI: %s\n", ec.message().c_str());
                continue;
            }
            ps.save_path = ".";
            session.add_torrent(ps);
        } catch (const std::exception &e) {
            fprintf(stderr, "mooch: failed to add torrent: %s\n", e.what());
        }
    }
    time_t last_update = time(0);
    for (b32 all_done = 0; !all_done;) {
        time_t now = time(0);
        if (now - last_update >= 1) {
            last_update = now;
            std::vector<libtorrent::torrent_handle> handles = session.get_torrents();
            all_done = 1;
            for (libtorrent::torrent_handle h : handles) {
                if (!h.is_valid()) {
                    continue;
                }
                libtorrent::torrent_status s = h.status();
                u8 *status_str;
                switch (s.state) {
                    case libtorrent::torrent_status::seeding:
                        status_str = (u8 *)"seeding";
                        break;
                    case libtorrent::torrent_status::finished:
                        status_str = (u8 *)"finished";
                        break;
                    case libtorrent::torrent_status::downloading:
                        status_str = (u8 *)"downloading";
                        break;
                    default:
                        status_str = (u8 *)"other";
                        break;
                }
                printf("mooch: name=%s, status=%s, downloaded=%ld, peers=%d\n", s.name.c_str(), status_str,
                       s.total_done, s.num_peers);
                if (s.state != libtorrent::torrent_status::seeding && s.state != libtorrent::torrent_status::finished) {
                    all_done = 0;
                }
            }
        }
        os_sleep_milliseconds(100);
    }
    printf("mooch: downloads complete\n");
}

internal void *arena_malloc_callback(u64 size) {
    return push_array_no_zero(g_arena, u8, size);
}

internal void arena_free_callback(void *) {
    // No-op
}

internal void *arena_realloc_callback(void *ptr, u64 size) {
    if (ptr == NULL) {
        return arena_malloc_callback(size);
    }
    if (size == 0) {
        return NULL;
    }
    u8 *new_ptr = push_array_no_zero(g_arena, u8, size);
    memmove(new_ptr, ptr, size);
    return new_ptr;
}

internal char *arena_strdup_callback(const char *s) {
    u64 len = cstring8_length((u8 *)s);
    u8 *dup = push_array_no_zero(g_arena, u8, len + 1);
    memcpy(dup, s, len);
    dup[len] = 0;
    return (char *)dup;
}

internal void *arena_calloc_callback(u64 nmemb, u64 size) {
    u64 total_size = nmemb * size;
    u8 *ptr = push_array(g_arena, u8, total_size);
    return ptr;
}

int entry_point(int argc, char **argv) {
    temp scratch = temp_begin(g_arena);
    params ps = {
        .filter = '0',
        .category = str8_lit("0_0"),
    };
    for (int ch = 0; (ch = getopt(argc, argv, "f:c:u:s:o:")) != -1;) {
        switch (ch) {
            case 'f':
                ps.filter = optarg[0];
                break;
            case 'c':
                ps.category = push_str8_copy(scratch.a, str8_cstring(optarg));
                break;
            case 'u':
                ps.user = push_str8_copy(scratch.a, str8_cstring(optarg));
                break;
            case 's':
                ps.sort = push_str8_copy(scratch.a, str8_cstring(optarg));
                break;
            case 'o':
                ps.order = push_str8_copy(scratch.a, str8_cstring(optarg));
                break;
            default:
                usage();
        }
    }
    if (optind >= argc) {
        usage();
    }
    ps.query = push_str8_copy(scratch.a, str8_cstring(argv[optind]));
    if (!validate_params(ps)) {
        return 1;
    }
    curl_global_init_mem(CURL_GLOBAL_DEFAULT, arena_malloc_callback, arena_free_callback, arena_realloc_callback,
                         arena_strdup_callback, arena_calloc_callback);
    xmlMemSetup(arena_free_callback, arena_malloc_callback, arena_realloc_callback, arena_strdup_callback);
    xmlInitParser();
    string8 temp_path = push_str8_cat(scratch.a, str8_lit("/tmp/mooch-"), str8_from_u64(scratch.a, os_now_microseconds(), 10, 0, 0));
    string8 temp_data = str8_zero();
    torrent_array torrents = get_torrents(scratch.a, ps);
    torrent_array selected_torrents = {0};
    if (torrents.count == 0) {
        fprintf(stderr, "mooch: no torrents found\n");
        goto cleanup;
    }
    for (u64 i = 0; i < torrents.count; ++i) {
        string8 line = push_str8_cat(scratch.a, torrents.v[i].title, str8_lit(" "));
        line = push_str8_cat(scratch.a, line, torrents.v[i].magnet);
        line = push_str8_cat(scratch.a, line, str8_lit("\n"));
        temp_data = push_str8_cat(scratch.a, temp_data, line);
    }
    if (!os_append_data_to_file_path(temp_path, temp_data)) {
        fprintf(stderr, "mooch: could not write to temporary file: %s\n", temp_path.str);
        goto cleanup;
    }
    run_editor(scratch.a, temp_path);
    selected_torrents = process_selected_torrents(scratch.a, temp_path);
    if (selected_torrents.count == 0) {
        fprintf(stderr, "mooch: no torrents selected\n");
        goto cleanup;
    }
    os_delete_file_at_path(temp_path);
    download_torrents(selected_torrents);

cleanup:
    xmlCleanupParser();
    curl_global_cleanup();
    temp_end(scratch);
    return 0;
}
