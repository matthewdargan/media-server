#include <curl/curl.h>
#include <dirent.h>
#include <libxml/parser.h>
#include <stdio.h>
#include <sys/mman.h>

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>

// clang-format off
#include "base/inc.h"
#include "os/inc.h"
#include "base/inc.c"
#include "os/inc.c"
// clang-format on

#define CHUNK_SIZE MB(1)
#define MAX_TORRENTS 75

global arena *g_arena = 0;

typedef struct params params;
struct params {
    u64 top_results;
    u8 filter;
    string8 category;
    string8 user;
    string8 sort;
    string8 order;
    string8 query;
};

typedef struct torrent torrent;
struct torrent {
    string8 link;
};

typedef struct torrent_array torrent_array;
struct torrent_array {
    u64 count;
    torrent *v;
};

internal void usage(void) {
    fprintf(stderr,
            "usage: moochrss [-f filter] [-c category] [-u user] [-s sort] "
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
    if (ps.top_results <= 0 || ps.top_results > MAX_TORRENTS) {
        fprintf(stderr, "invalid top results: %lu\n", ps.top_results);
        return 0;
    }
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
    string8 base_url = str8_lit("https://nyaa.si?page=rss");
    string8 user = str8_zero();
    if (ps.user.size > 0) {
        user = push_str8_cat(a, str8_lit("&u="), ps.user);
    }
    string8 query = push_str8f(a, (char *)"&f=%c&c=%s&q=%s", ps.filter, ps.category.str, encoded_query.str);
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
    curl_easy_setopt(curl, CURLOPT_URL, url.str);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "mooch: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return torrents;
    }
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "mooch: HTTP error %ld\n", http_code);
        curl_easy_cleanup(curl);
        return torrents;
    }
    xmlDocPtr doc = xmlReadMemory((const char *)chunk.str, chunk.size, (const char *)url.str, NULL,
                                  XML_PARSE_NOWARNING | XML_PARSE_NOERROR);
    if (doc == NULL) {
        fprintf(stderr, "mooch: failed to parse XML\n");
        curl_easy_cleanup(curl);
        return torrents;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root == NULL) {
        fprintf(stderr, "mooch: empty document\n");
        curl_easy_cleanup(curl);
        return torrents;
    }
    xmlNodePtr channel = NULL;
    for (xmlNodePtr node = root->children; node != NULL; node = node->next) {
        if (node->type == XML_ELEMENT_NODE && xmlStrcmp(node->name, BAD_CAST "channel") == 0) {
            channel = node;
            break;
        }
    }
    if (channel == NULL) {
        fprintf(stderr, "mooch: no channel found\n");
        curl_easy_cleanup(curl);
        return torrents;
    }
    torrents.v = push_array_no_zero(a, torrent, ps.top_results);
    for (xmlNodePtr item = channel->children; item != NULL && torrents.count < ps.top_results; item = item->next) {
        if (item->type == XML_ELEMENT_NODE && xmlStrcmp(item->name, BAD_CAST "item") == 0) {
            for (xmlNodePtr node = item->children; node != NULL; node = node->next) {
                if (node->type == XML_ELEMENT_NODE && xmlStrcmp(node->name, BAD_CAST "link") == 0) {
                    xmlChar *link_content = xmlNodeGetContent(node);
                    if (link_content != NULL) {
                        torrents.v[torrents.count].link =
                            push_str8_copy(a, str8((u8 *)link_content, xmlStrlen(link_content)));
                        ++torrents.count;
                    }
                }
            }
        }
    }
    curl_easy_cleanup(curl);
    return torrents;
}

internal void download_torrents(torrent_array torrents) {
    if (torrents.count == 0) {
        return;
    }
    char *home = getenv("HOME");
    string8 config_path = push_str8_cat(g_arena, str8_cstring(home), str8_lit("/.config/moochrss"));
    if (!os_folder_path_exists(g_arena, config_path)) {
        os_make_directory(g_arena, config_path);
    }
    string8 history_path = push_str8_cat(g_arena, config_path, str8_lit("/history"));
    string8 history_data = str8_zero();
    if (os_file_path_exists(g_arena, history_path)) {
        history_data = os_data_from_file_path(g_arena, history_path);
    }
    libtorrent::settings_pack pack;
    pack.set_int(libtorrent::settings_pack::alert_mask,
                 libtorrent::alert::status_notification | libtorrent::alert::error_notification);
    libtorrent::session session(pack);
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "mooch: could not initialize curl\n");
        return;
    }
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mooch/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    string8 temp_path = str8_lit("/tmp/mooch.torrent");
    string8 torrents_to_download = str8_zero();
    u64 to_download = 0;
    for (u64 i = 0; i < torrents.count; ++i) {
        if (history_data.size > 0) {
            if (str8_find_needle(history_data, 0, torrents.v[i].link, 0) != history_data.size) {
                printf("mooch: already downloaded %s\n", torrents.v[i].link.str);
                continue;
            }
        }
        FILE *fp = fopen((const char *)temp_path.str, "wb");
        if (fp == NULL) {
            fprintf(stderr, "mooch: could not write to temporary file: %s\n", temp_path.str);
            continue;
        }
        curl_easy_setopt(curl, CURLOPT_URL, torrents.v[i].link.str);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        CURLcode res = curl_easy_perform(curl);
        fclose(fp);
        if (res != CURLE_OK) {
            fprintf(stderr, "mooch: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            continue;
        }
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code != 200) {
            fprintf(stderr, "mooch: HTTP error %ld\n", http_code);
            continue;
        }
        libtorrent::add_torrent_params ps;
        libtorrent::error_code ec;
        ps.ti = std::make_shared<libtorrent::torrent_info>((const char *)temp_path.str, ec);
        if (ec) {
            fprintf(stderr, "mooch: failed to parse torrent file: %s\n", ec.message().c_str());
            continue;
        }
        ps.save_path = ".";
        session.add_torrent(ps);
        string8 torrent_nl = push_str8_cat(g_arena, torrents.v[i].link, str8_lit("\n"));
        torrents_to_download = push_str8_cat(g_arena, torrents_to_download, torrent_nl);
        ++to_download;
    }
    unlink((const char *)temp_path.str);
    curl_easy_cleanup(curl);
    if (to_download == 0) {
        fprintf(stderr, "mooch: no torrents to download\n");
        return;
    }
    os_append_data_to_file_path(g_arena, history_path, torrents_to_download);
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
    printf("mooch: downloaded %lu torrents\n", to_download);
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

int main(int argc, char *argv[]) {
    g_arena = arena_alloc((arena_params){.flags = arena_default_flags,
                                         .reserve_size = arena_default_reserve_size,
                                         .commit_size = arena_default_commit_size});
    params ps = {
        .top_results = MAX_TORRENTS,
        .filter = '0',
        .category = str8_lit("0_0"),
    };
    for (int ch = 0; (ch = getopt(argc, argv, "t:f:c:u:s:o:")) != -1;) {
        switch (ch) {
            case 't':
                ps.top_results = u64_from_str8(str8_cstring(optarg), 10);
                break;
            case 'f':
                ps.filter = optarg[0];
                break;
            case 'c':
                ps.category = push_str8_copy(g_arena, str8_cstring(optarg));
                break;
            case 'u':
                ps.user = push_str8_copy(g_arena, str8_cstring(optarg));
                break;
            case 's':
                ps.sort = push_str8_copy(g_arena, str8_cstring(optarg));
                break;
            case 'o':
                ps.order = push_str8_copy(g_arena, str8_cstring(optarg));
                break;
            default:
                usage();
        }
    }
    if (optind >= argc) {
        usage();
    }
    ps.query = push_str8_copy(g_arena, str8_cstring(argv[optind]));
    if (!validate_params(ps)) {
        return 1;
    }
    curl_global_init_mem(CURL_GLOBAL_DEFAULT, arena_malloc_callback, arena_free_callback, arena_realloc_callback,
                         arena_strdup_callback, arena_calloc_callback);
    xmlMemSetup(arena_free_callback, arena_malloc_callback, arena_realloc_callback, arena_strdup_callback);
    xmlInitParser();
    torrent_array torrents = get_torrents(g_arena, ps);
    if (torrents.count == 0) {
        fprintf(stderr, "mooch: no torrents found\n");
        goto cleanup;
    }
    download_torrents(torrents);

cleanup:
    xmlCleanupParser();
    curl_global_cleanup();
    arena_release(g_arena);
    return 0;
}
