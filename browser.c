/* ============================================================
 * [1] インクルード
 *
 *   【小段落 1-1】 libcss 関連を追加
 * ============================================================ */
#include <gtk/gtk.h>
#include <curl/curl.h>
#include <gumbo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* === 追加ここから === */
#include <libcss/libcss.h>
#include <libwapcaplet/libwapcaplet.h>
/* === 追加ここまで === */

/* ============================================================
 * [2] グローバル定義
 * ============================================================ */
typedef struct {
    GtkWidget *entry;
    GtkWidget *search_entry;
    GtkWidget *text_view;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *reload_button;
} AppWidgets;

static AppWidgets g_app;

static GList *g_back_stack = NULL;
static GList *g_forward_stack = NULL;
static char  *g_current_url = NULL;
static char   g_base_url[2048] = "";

typedef struct {
    char *data;
    size_t size;
} MemoryBuffer;

static void load_url(const char *url, gboolean add_to_history);
static void update_nav_buttons(void);

/* === 追加ここから === */
/* クラッシュ時のデバッグ用：現在処理中のノード情報 */
static const char *g_debug_current_tag = "(none)";
static const char *g_debug_current_class = "";
static const char *g_debug_current_id = "";
static int g_debug_depth = 0;
/* === 追加ここまで === */

/* === 追加ここから === */
/* GumboNode* → GtkTextTag* のマップ（各要素の computed style を反映したタグ） */
static GHashTable *g_node_tags = NULL;
/* === 追加ここまで === */

/* === 追加ここから === */
/* フォームのコンテキスト */
typedef struct {
    char   *action;        /* 送信先 URL */
    char   *method;        /* "get" or "post" */
    GSList *input_tags;    /* このフォーム内の input タグのリスト */
} FormContext;

static FormContext *g_current_form = NULL;
/* === 追加ここまで === */


/* ============================================================
 * [3] libcurl コールバック
 * ============================================================ */
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "realloc failed\n");
        return 0;
    }
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

/* ============================================================
 * [4] URL 取得
 * ============================================================ */
static char *fetch_url(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    MemoryBuffer chunk = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (C-Browser)");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk.data);
        chunk.data = NULL;
    }
    curl_easy_cleanup(curl);
    return chunk.data;
}

/* ============================================================
 * [4.5] file:// URL 用のローカルファイル読み込み
 * ============================================================ */
static char *read_local_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        g_print("fopen failed: %s\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) { fclose(fp); return NULL; }

    char *buf = malloc(size + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, size, fp);
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

/* ============================================================
 * [5] 相対 URL 解決
 * ============================================================ */
static char *resolve_url(const char *base, const char *href) {
    if (!href || !*href) return NULL;
    if (strstr(href, "://") != NULL) {
        return strdup(href);
    }
    if (href[0] == '/' && href[1] == '/') {
        const char *colon = strchr(base, ':');
        if (colon) {
            size_t scheme_len = colon - base + 1;
            char *result = malloc(scheme_len + strlen(href) + 1);
            memcpy(result, base, scheme_len);
            strcpy(result + scheme_len, href);
            return result;
        }
        return strdup(href);
    }
    if (href[0] == '/') {
        const char *p = strstr(base, "://");
        if (!p) return strdup(href);
        p += 3;
        const char *slash = strchr(p, '/');
        size_t host_len = slash ? (size_t)(slash - base) : strlen(base);
        char *result = malloc(host_len + strlen(href) + 1);
        memcpy(result, base, host_len);
        strcpy(result + host_len, href);
        return result;
    }
    const char *last_slash = strrchr(base, '/');
    if (!last_slash) return strdup(href);
    size_t dir_len = last_slash - base + 1;
    char *result = malloc(dir_len + strlen(href) + 1);
    memcpy(result, base, dir_len);
    strcpy(result + dir_len, href);
    return result;
}

/* ============================================================
 * [6] リンクタグ生成
 * ============================================================ */
static GtkTextTag *create_link_tag(GtkTextBuffer *buffer, const char *url) {
    GtkTextTag *tag = gtk_text_buffer_create_tag(buffer, NULL,
        "foreground", "blue",
        "underline", PANGO_UNDERLINE_SINGLE,
        NULL);
    g_object_set_data_full(G_OBJECT(tag), "url", g_strdup(url), g_free);
    return tag;
}

/* === 追加ここから === */

/* ============================================================
 * [6.1] computed style → GtkTextTag 変換
 * ============================================================ */

/* css_color (AARRGGBB) を "#RRGGBB" 形式に変換 */
static void css_color_to_hex(css_color c, char out[8]) {
    snprintf(out, 8, "#%02X%02X%02X",
             (unsigned)((c >> 16) & 0xFF),
             (unsigned)((c >> 8) & 0xFF),
             (unsigned)(c & 0xFF));
}

/* computed style から GtkTextTag を作る。
   対応プロパティ：color, font-weight, font-size */
static GtkTextTag *create_tag_from_computed_style(GtkTextBuffer *buffer,
                                                  const css_computed_style *style) {
    if (style == NULL) return NULL;

    GtkTextTag *tag = gtk_text_buffer_create_tag(buffer, NULL, NULL);

    /* color */
    css_color color = 0;
    uint8_t ctype = css_computed_color(style, &color);
    if (ctype == CSS_COLOR_COLOR) {
        char hex[8];
        css_color_to_hex(color, hex);
        g_object_set(G_OBJECT(tag), "foreground", hex, NULL);
    }

    /* font-weight */
    uint8_t weight = css_computed_font_weight(style);
    switch (weight) {
        case CSS_FONT_WEIGHT_BOLD:
        case CSS_FONT_WEIGHT_BOLDER:
        case CSS_FONT_WEIGHT_600:
        case CSS_FONT_WEIGHT_700:
        case CSS_FONT_WEIGHT_800:
        case CSS_FONT_WEIGHT_900:
            g_object_set(G_OBJECT(tag), "weight", PANGO_WEIGHT_BOLD, NULL);
            break;
        case CSS_FONT_WEIGHT_500:
            g_object_set(G_OBJECT(tag), "weight", PANGO_WEIGHT_MEDIUM, NULL);
            break;
        case CSS_FONT_WEIGHT_300:
            g_object_set(G_OBJECT(tag), "weight", PANGO_WEIGHT_LIGHT, NULL);
            break;
        case CSS_FONT_WEIGHT_200:
        case CSS_FONT_WEIGHT_100:
        case CSS_FONT_WEIGHT_LIGHTER:
            g_object_set(G_OBJECT(tag), "weight", PANGO_WEIGHT_ULTRALIGHT, NULL);
            break;
        default:
            /* NORMAL / INHERIT は指定しない（親から継承） */
            break;
    }

    /* font-size */
    css_fixed size_len = 0;
    css_unit size_unit = CSS_UNIT_PX;
    uint8_t stype = css_computed_font_size(style, &size_len, &size_unit);
    if (stype != 0 /* INHERIT 以外 */ && size_len > 0) {
        /* css_fixed は 22.10 固定小数点。Pango の size は pt * PANGO_SCALE */
        /* 簡易的に px をそのまま Pango 単位として使う */
        int pango_size = (int)(size_len / 1024 * PANGO_SCALE);
        if (pango_size > 0) {
            g_object_set(G_OBJECT(tag), "size", pango_size, NULL);
        }
    }

    return tag;
}
/* === 追加ここまで === */

/* ============================================================
 * [7] CSS 収集
 * ============================================================ */
static void collect_inline_styles(GumboNode *node, GString *out) {
    if (node->type != GUMBO_NODE_ELEMENT) return;

    if (node->v.element.tag == GUMBO_TAG_STYLE) {
        GumboVector *children = &node->v.element.children;
        for (unsigned int i = 0; i < children->length; ++i) {
            GumboNode *child = children->data[i];
            if (child->type == GUMBO_NODE_TEXT) {
                g_string_append(out, child->v.text.text);
                g_string_append_c(out, '\n');
            }
        }
        return;
    }

    GumboVector *children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        collect_inline_styles(children->data[i], out);
    }
}

/* === 追加ここから === */

/* === 追加ここから === */
/* ============================================================
 * [7.1] CSS サニタイズ
 *
 *   libcss 0.9.2 が理解できない構文を除去する。
 *   まずは @media / @keyframes / @-webkit-keyframes を削除。
 * ============================================================ */

/* 指定された at-rule 名で始まるブロックを丸ごと除去する。
   ネストした {} を数えて、対応する } まで読み飛ばす。 */
static GString *strip_at_rule_blocks(const char *css,
                                     const char *at_name,
                                     GString *out) {
    size_t name_len = strlen(at_name);
    const char *p = css;

    while (*p) {
        /* コメントはそのまま通過（後に除去する） */
        if (p[0] == '/' && p[1] == '*') {
            const char *end = strstr(p + 2, "*/");
            if (!end) break;
            g_string_append_len(out, p, end - p + 2);
            p = end + 2;
            continue;
        }

        /* at-rule の開始を検出 */
        if (*p == '@' && strncmp(p, at_name, name_len) == 0) {
            const char *after = p + name_len;
            /* at-rule 名の直後が英数字でないこと（@media と @mediafoo の区別） */
            if (!isalnum((unsigned char)*after) && *after != '-' && *after != '_') {
                /* { を探してブロック開始 */
                const char *brace = strchr(p, '{');
                if (brace) {
                    int depth = 1;
                    const char *q = brace + 1;
                    while (*q && depth > 0) {
                        if (*q == '{') depth++;
                        else if (*q == '}') depth--;
                        q++;
                    }
                    /* q はブロック終端の次を指す */
                    p = q;
                    continue;
                }
            }
        }

        g_string_append_c(out, *p);
        p++;
    }
    return out;
}
/* === 追加ここまで === */


/* ============================================================
 * [7.5] libcss スタイルシート生成（ステップ2で追加）
 * ============================================================ */

/* libcss の URL 解決コールバック
   base は const char*、rel は lwc_string*、abs は lwc_string** を返す */
static css_error css_resolve_url(void *pw, const char *base,
                                 lwc_string *rel_url, lwc_string **abs_url) {
    (void)pw;

    const char *rel = lwc_string_data(rel_url);

    char buf[4096];
    if (strstr(rel, "://") != NULL) {
        /* 絶対 URL */
        snprintf(buf, sizeof(buf), "%s", rel);
    } else if (rel[0] == '/' && rel[1] == '/') {
        /* プロトコル相対 */
        const char *colon = strchr(base, ':');
        if (colon) {
            size_t scheme_len = colon - base + 1;
            snprintf(buf, sizeof(buf), "%.*s%s", (int)scheme_len, base, rel);
        } else {
            snprintf(buf, sizeof(buf), "%s", rel);
        }
    } else if (rel[0] == '/') {
        /* ルート相対 */
        const char *p = strstr(base, "://");
        if (p) {
            p += 3;
            const char *slash = strchr(p, '/');
            size_t host_len = slash ? (size_t)(slash - base) : strlen(base);
            snprintf(buf, sizeof(buf), "%.*s%s", (int)host_len, base, rel);
        } else {
            snprintf(buf, sizeof(buf), "%s", rel);
        }
    } else {
        /* 相対パス */
        const char *last_slash = strrchr(base, '/');
        if (last_slash) {
            size_t dir_len = last_slash - base + 1;
            snprintf(buf, sizeof(buf), "%.*s%s", (int)dir_len, base, rel);
        } else {
            snprintf(buf, sizeof(buf), "%s", rel);
        }
    }

    if (lwc_intern_string(buf, strlen(buf), abs_url) != lwc_error_ok) {
        return CSS_NOMEM;
    }
    return CSS_OK;
}

static css_stylesheet *create_stylesheet_from_css(const char *css_text) {
    css_stylesheet_params params;
    memset(&params, 0, sizeof(params));

    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_3;
    params.charset = NULL;
    params.url = g_base_url;
    params.title = NULL;
    params.allow_quirks = false;
    params.inline_style = false;
    params.resolve = css_resolve_url;
    params.resolve_pw = NULL;
    params.import = NULL;
    params.import_pw = NULL;
    params.color = NULL;
    params.color_pw = NULL;
    params.font = NULL;
    params.font_pw = NULL;

    /* 保険：URL が空ならダミーを入れる */
    if (params.url == NULL || params.url[0] == '\0') {
        params.url = "http://localhost/";
    }

    css_stylesheet *sheet = NULL;
    css_error err = css_stylesheet_create(&params, &sheet);
    if (err != CSS_OK) {
        g_print("css_stylesheet_create failed: %d (%s)\n",
                err, css_error_to_string(err));
        return NULL;
    }

    err = css_stylesheet_append_data(sheet,
            (const uint8_t *)css_text, strlen(css_text));
    if (err != CSS_OK && err != CSS_NEEDDATA) {
        g_print("css_stylesheet_append_data failed: %d (%s)\n",
                err, css_error_to_string(err));
        css_stylesheet_destroy(sheet);
        return NULL;
    }

    err = css_stylesheet_data_done(sheet);
    if (err != CSS_OK) {
        g_print("css_stylesheet_data_done failed: %d (%s)\n",
                err, css_error_to_string(err));
        css_stylesheet_destroy(sheet);
        return NULL;
    }

    return sheet;
}
/* === 追加ここまで === */

/* === 追加ここから === */
/* ============================================================
 * [7.6] libcss セレクタハンドラ
 *
 *   GumboNode をそのまま libcss のノードとして使う。
 *   libcss はノードを void* として扱うだけなので、
 *   libdom への変換は不要。
 * ============================================================ */

/* 【小段落 7.6-1】 node_name */
static css_error handle_node_name(void *pw, void *node, css_qname *qname) {
    (void)pw;
    qname->name = NULL;
    qname->ns = NULL;
    if (node == NULL) return CSS_OK;

    GumboNode *n = (GumboNode *)node;
    if (n->type != GUMBO_NODE_ELEMENT) return CSS_OK;

    GumboTag tag = n->v.element.tag;
    const char *tag_name = gumbo_normalized_tagname(tag);
    /* 未知のタグは "div" として扱う（空だと libcss が壊れる） */
    if (!tag_name || !*tag_name) {
        tag_name = "div";
    }
    if (lwc_intern_string(tag_name, strlen(tag_name), &qname->name) != lwc_error_ok) {
        return CSS_NOMEM;
    }
    return CSS_OK;
}

/* 【小段落 7.6-2】 node_classes */
static css_error handle_node_classes(void *pw, void *node,
                                     lwc_string ***classes, uint32_t *n_classes) {
    (void)pw;
    GumboNode *n = (GumboNode *)node;
    *classes = NULL;
    *n_classes = 0;
    if (n->type != GUMBO_NODE_ELEMENT) return CSS_OK;

    GumboAttribute *attr = gumbo_get_attribute(&n->v.element.attributes, "class");
    if (!attr || !attr->value || !*attr->value) return CSS_OK;

    /* 個数を数える */
    uint32_t count = 0;
    for (const char *p = attr->value; *p; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        count++;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    }
    if (count == 0) return CSS_OK;

    lwc_string **arr = calloc(count, sizeof(lwc_string *));
    if (!arr) return CSS_NOMEM;

    uint32_t i = 0;
    for (const char *p = attr->value; *p && i < count; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        size_t len = p - start;
        if (lwc_intern_string(start, len, &arr[i]) != lwc_error_ok) {
            for (uint32_t j = 0; j < i; j++) lwc_string_unref(arr[j]);
            free(arr);
            return CSS_NOMEM;
        }
        i++;
    }

    *classes = arr;
    *n_classes = i;
    return CSS_OK;
}

/* 【小段落 7.6-3】 node_id */
static css_error handle_node_id(void *pw, void *node, lwc_string **id) {
    (void)pw;
    GumboNode *n = (GumboNode *)node;
    *id = NULL;
    if (n->type != GUMBO_NODE_ELEMENT) return CSS_OK;

    GumboAttribute *attr = gumbo_get_attribute(&n->v.element.attributes, "id");
    if (!attr || !attr->value || !*attr->value) return CSS_OK;

    if (lwc_intern_string(attr->value, strlen(attr->value), id) != lwc_error_ok) {
        return CSS_NOMEM;
    }
    return CSS_OK;
}

/* 【小段落 7.6-4】 named_* / parent / sibling は未実装（NULL 返し） */
static css_error handle_named_ancestor_node(void *pw, void *node,
        const css_qname *qname, void **ancestor) {
    (void)pw; (void)node; (void)qname;
    *ancestor = NULL;
    return CSS_OK;
}
static css_error handle_named_parent_node(void *pw, void *node,
        const css_qname *qname, void **parent) {
    (void)pw; (void)node; (void)qname;
    *parent = NULL;
    return CSS_OK;
}
static css_error handle_named_sibling_node(void *pw, void *node,
        const css_qname *qname, void **sibling) {
    (void)pw; (void)node; (void)qname;
    *sibling = NULL;
    return CSS_OK;
}
static css_error handle_named_generic_sibling_node(void *pw, void *node,
        const css_qname *qname, void **sibling) {
    (void)pw; (void)node; (void)qname;
    *sibling = NULL;
    return CSS_OK;
}
static css_error handle_parent_node(void *pw, void *node, void **parent) {
    (void)pw;
    GumboNode *n = (GumboNode *)node;
    *parent = n->parent;
    return CSS_OK;
}
static css_error handle_sibling_node(void *pw, void *node, void **sibling) {
    (void)pw;
    GumboNode *n = (GumboNode *)node;
    *sibling = NULL;
    if (n->parent == NULL) return CSS_OK;
    if (n->parent->type != GUMBO_NODE_ELEMENT) return CSS_OK;

    GumboVector *children = &n->parent->v.element.children;
    for (unsigned int i = 0; i < children->length; i++) {
        if (children->data[i] == n) {
            if (i + 1 < children->length) {
                *sibling = children->data[i + 1];
            }
            return CSS_OK;
        }
    }
    return CSS_OK;
}

/* 【小段落 7.6-5】 has_name / has_class / has_id */
static css_error handle_node_has_name(void *pw, void *node,
        const css_qname *qname, bool *match) {
    (void)pw;
    *match = false;
    GumboNode *n = (GumboNode *)node;
    if (n->type != GUMBO_NODE_ELEMENT) return CSS_OK;
    if (qname->name == NULL) return CSS_OK;

    const char *tag = gumbo_normalized_tagname(n->v.element.tag);
    if (!tag || !*tag) return CSS_OK;
    size_t qlen = lwc_string_length(qname->name);
    if (qlen != strlen(tag)) return CSS_OK;
    if (memcmp(lwc_string_data(qname->name), tag, qlen) == 0) {
        *match = true;
    }
    return CSS_OK;
}

static css_error handle_node_has_class(void *pw, void *node,
        lwc_string *name, bool *match) {
    (void)pw;
    *match = false;
    GumboNode *n = (GumboNode *)node;
    if (n->type != GUMBO_NODE_ELEMENT) return CSS_OK;

    GumboAttribute *attr = gumbo_get_attribute(&n->v.element.attributes, "class");
    if (!attr || !attr->value) return CSS_OK;

    const char *target = lwc_string_data(name);
    size_t target_len = lwc_string_length(name);

    const char *p = attr->value;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        size_t len = p - start;
        if (len == target_len && memcmp(start, target, len) == 0) {
            *match = true;
            return CSS_OK;
        }
    }
    return CSS_OK;
}

static css_error handle_node_has_id(void *pw, void *node,
        lwc_string *name, bool *match) {
    (void)pw;
    *match = false;
    GumboNode *n = (GumboNode *)node;
    if (n->type != GUMBO_NODE_ELEMENT) return CSS_OK;

    GumboAttribute *attr = gumbo_get_attribute(&n->v.element.attributes, "id");
    if (!attr || !attr->value) return CSS_OK;

    size_t len = lwc_string_length(name);
    if (strlen(attr->value) == len &&
        memcmp(attr->value, lwc_string_data(name), len) == 0) {
        *match = true;
    }
    return CSS_OK;
}

/* 【小段落 7.6-6】 属性マッチ系はとりあえず false 固定 */
static css_error handle_node_has_attribute(void *pw, void *node,
        const css_qname *qname, bool *match) {
    (void)pw; (void)node; (void)qname;
    *match = false; return CSS_OK;
}
static css_error handle_node_has_attribute_equal(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match) {
    (void)pw; (void)node; (void)qname; (void)value;
    *match = false; return CSS_OK;
}
static css_error handle_node_has_attribute_dashmatch(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match) {
    (void)pw; (void)node; (void)qname; (void)value;
    *match = false; return CSS_OK;
}
static css_error handle_node_has_attribute_includes(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match) {
    (void)pw; (void)node; (void)qname; (void)value;
    *match = false; return CSS_OK;
}
static css_error handle_node_has_attribute_prefix(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match) {
    (void)pw; (void)node; (void)qname; (void)value;
    *match = false; return CSS_OK;
}
static css_error handle_node_has_attribute_suffix(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match) {
    (void)pw; (void)node; (void)qname; (void)value;
    *match = false; return CSS_OK;
}
static css_error handle_node_has_attribute_substring(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match) {
    (void)pw; (void)node; (void)qname; (void)value;
    *match = false; return CSS_OK;
}

/* 【小段落 7.6-7】 ツリー構造系 */
static css_error handle_node_is_root(void *pw, void *node, bool *match) {
    (void)pw;
    GumboNode *n = (GumboNode *)node;
    *match = (n->parent == NULL);
    return CSS_OK;
}
static css_error handle_node_count_siblings(void *pw, void *node,
        bool same_name, bool after, int32_t *count) {
    (void)pw; (void)node; (void)same_name; (void)after;
    *count = 0;
    return CSS_OK;
}
static css_error handle_node_is_empty(void *pw, void *node, bool *match) {
    (void)pw;
    GumboNode *n = (GumboNode *)node;
    *match = (n->type == GUMBO_NODE_ELEMENT &&
              n->v.element.children.length == 0);
    return CSS_OK;
}

/* 【小段落 7.6-8】 状態疑似クラス */
static css_error handle_node_is_link(void *pw, void *node, bool *match) {
    (void)pw;
    GumboNode *n = (GumboNode *)node;
    *match = (n->type == GUMBO_NODE_ELEMENT && n->v.element.tag == GUMBO_TAG_A);
    return CSS_OK;
}
static css_error handle_node_is_visited(void *pw, void *node, bool *match) {
    (void)pw; (void)node; *match = false; return CSS_OK;
}
static css_error handle_node_is_hover(void *pw, void *node, bool *match) {
    (void)pw; (void)node; *match = false; return CSS_OK;
}
static css_error handle_node_is_active(void *pw, void *node, bool *match) {
    (void)pw; (void)node; *match = false; return CSS_OK;
}
static css_error handle_node_is_focus(void *pw, void *node, bool *match) {
    (void)pw; (void)node; *match = false; return CSS_OK;
}
static css_error handle_node_is_enabled(void *pw, void *node, bool *match) {
    (void)pw; (void)node; *match = true; return CSS_OK;
}
static css_error handle_node_is_disabled(void *pw, void *node, bool *match) {
    (void)pw; (void)node; *match = false; return CSS_OK;
}
static css_error handle_node_is_checked(void *pw, void *node, bool *match) {
    (void)pw; (void)node; *match = false; return CSS_OK;
}
static css_error handle_node_is_target(void *pw, void *node, bool *match) {
    (void)pw; (void)node; *match = false; return CSS_OK;
}
static css_error handle_node_is_lang(void *pw, void *node,
        lwc_string *lang, bool *match) {
    (void)pw; (void)node; (void)lang;
    *match = false; return CSS_OK;
}

/* 【小段落 7.6-9】 ヒント系 */
static css_error handle_node_presentational_hint(void *pw, void *node,
        uint32_t *nhints, css_hint **hints) {
    (void)pw; (void)node;
    *nhints = 0;
    *hints = NULL;
    return CSS_OK;
}
static css_error handle_ua_default_for_property(void *pw, uint32_t property,
        css_hint *hint) {
    (void)pw; (void)property; (void)hint;
    return CSS_INVALID;
}

/* 【小段落 7.6-10】 libcss_node_data 系 */
static css_error handle_set_libcss_node_data(void *pw, void *node,
        void *libcss_node_data) {
    (void)pw; (void)node; (void)libcss_node_data;
    return CSS_OK;
}
static css_error handle_get_libcss_node_data(void *pw, void *node,
        void **libcss_node_data) {
    (void)pw; (void)node;
    *libcss_node_data = NULL;
    return CSS_OK;
}

/* 【小段落 7.6-11】 ハンドラ本体（グローバル変数） */
static css_select_handler g_select_handler = {
    .handler_version = CSS_SELECT_HANDLER_VERSION_1,
    .node_name = handle_node_name,
    .node_classes = handle_node_classes,
    .node_id = handle_node_id,
    .named_ancestor_node = handle_named_ancestor_node,
    .named_parent_node = handle_named_parent_node,
    .named_sibling_node = handle_named_sibling_node,
    .named_generic_sibling_node = handle_named_generic_sibling_node,
    .parent_node = handle_parent_node,
    .sibling_node = handle_sibling_node,
    .node_has_name = handle_node_has_name,
    .node_has_class = handle_node_has_class,
    .node_has_id = handle_node_has_id,
    .node_has_attribute = handle_node_has_attribute,
    .node_has_attribute_equal = handle_node_has_attribute_equal,
    .node_has_attribute_dashmatch = handle_node_has_attribute_dashmatch,
    .node_has_attribute_includes = handle_node_has_attribute_includes,
    .node_has_attribute_prefix = handle_node_has_attribute_prefix,
    .node_has_attribute_suffix = handle_node_has_attribute_suffix,
    .node_has_attribute_substring = handle_node_has_attribute_substring,
    .node_is_root = handle_node_is_root,
    .node_count_siblings = handle_node_count_siblings,
    .node_is_empty = handle_node_is_empty,
    .node_is_link = handle_node_is_link,
    .node_is_visited = handle_node_is_visited,
    .node_is_hover = handle_node_is_hover,
    .node_is_active = handle_node_is_active,
    .node_is_focus = handle_node_is_focus,
    .node_is_enabled = handle_node_is_enabled,
    .node_is_disabled = handle_node_is_disabled,
    .node_is_checked = handle_node_is_checked,
    .node_is_target = handle_node_is_target,
    .node_is_lang = handle_node_is_lang,
    .node_presentational_hint = handle_node_presentational_hint,
    .ua_default_for_property = handle_ua_default_for_property,
    .set_libcss_node_data = handle_set_libcss_node_data,
    .get_libcss_node_data = handle_get_libcss_node_data,
};
/* === 追加ここまで === */

/* === 追加ここから === */
/* === 追加ここから === */
/* ============================================================
 * [7.7] セレクタコンテキストとスタイル計算
 * ============================================================ */

/* 【小段落 7.7-1】 ツリーを走査して各要素のスタイルを計算し、
   その要素の GtkTextTag を作ってハッシュに保存する */
static void compute_styles_for_tree(GumboNode *node,
                                    css_select_ctx *select_ctx,
                                    const css_unit_ctx *unit_ctx,
                                    const css_media *media,
                                    GtkTextBuffer *buffer) {
    if (node == NULL) return;
    if (node->type != GUMBO_NODE_ELEMENT) return;

    GumboTag tag = node->v.element.tag;

    /* script / style の中身はスキップ */
    if (tag == GUMBO_TAG_SCRIPT || tag == GUMBO_TAG_STYLE) return;

    const char *tag_name = gumbo_normalized_tagname(tag);
    /* タグ名が取れない（未知のタグ、SVG の中身など）ならスキップ */
    if (!tag_name || !*tag_name) return;

    GumboAttribute *attr_class = gumbo_get_attribute(&node->v.element.attributes, "class");
    GumboAttribute *attr_id    = gumbo_get_attribute(&node->v.element.attributes, "id");

    g_debug_current_tag   = tag_name;
    g_debug_current_class = (attr_class && attr_class->value) ? attr_class->value : "";
    g_debug_current_id    = (attr_id && attr_id->value) ? attr_id->value : "";
    g_debug_depth++;

    g_print("selecting[%d]: <%s id=\"%s\" class=\"%s\">\n",
            g_debug_depth,
            g_debug_current_tag,
            g_debug_current_id,
            g_debug_current_class);
    fflush(stdout);

    css_select_results *results = NULL;
    css_error err = css_select_style(select_ctx, node, unit_ctx, media,
                                     NULL, &g_select_handler, NULL,
                                     &results);
    if (err == CSS_OK && results != NULL) {
        /* 通常の computed style を取り出す */
        css_computed_style *style = results->styles[CSS_PSEUDO_ELEMENT_NONE];
        if (style != NULL) {
            GtkTextTag *text_tag = create_tag_from_computed_style(buffer, style);
            if (text_tag != NULL) {
                g_hash_table_insert(g_node_tags, node, text_tag);
            }
        }
        css_select_results_destroy(results);
    } else if (err != CSS_OK) {
        g_print("css_select_style failed for <%s>: %d (%s)\n",
                g_debug_current_tag, err, css_error_to_string(err));
    }

    GumboVector *children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; i++) {
        compute_styles_for_tree(children->data[i],
                                select_ctx, unit_ctx, media, buffer);
    }

    g_debug_depth--;
}
/* === 追加ここまで === */

/* ============================================================
 * [8] Gumbo ツリーを TextBuffer に流し込む
 *
 *   active_tags には祖先要素のタグが入っており、
 *   テキスト挿入時に全部適用する。
 * ============================================================ */

/* 画像データをメモリ上にダウンロードするための構造体 */
typedef struct {
    unsigned char *data;
    size_t size;
} ImageBuffer;

static size_t write_image_data(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    ImageBuffer *mem = (ImageBuffer *)userdata;
    unsigned char *ptr2 = realloc(mem->data, mem->size + total);
    if (ptr2 == NULL) return 0;
    mem->data = ptr2;
    memcpy(&(mem->data[mem->size]), ptr, total);
    mem->size += total;
    return total;
}

/* URLから画像をダウンロードして GdkPixbuf を作成するヘルパー関数 */
static GdkPixbuf *download_pixbuf(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    ImageBuffer chunk = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_image_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); /* タイムアウト5秒 */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || chunk.data == NULL) {
        if (chunk.data) free(chunk.data);
        return NULL;
    }

    GInputStream *stream = g_memory_input_stream_new_from_data(chunk.data, chunk.size, g_free);
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(stream, NULL, NULL);
    g_object_unref(stream);

    return pixbuf;
}

static void render_gumbo_node(GumboNode *node, GtkTextBuffer *buffer,
                              GPtrArray *active_tags) {
    if (node->type == GUMBO_NODE_TEXT) {
        GtkTextIter start_iter;
        gtk_text_buffer_get_end_iter(buffer, &start_iter);
        GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &start_iter, TRUE);

        gtk_text_buffer_insert(buffer, &start_iter, node->v.text.text, -1);

        if (active_tags->len > 0) {
            GtkTextIter start, end;
            gtk_text_buffer_get_iter_at_mark(buffer, &start, mark);
            gtk_text_buffer_get_end_iter(buffer, &end);
            for (guint i = 0; i < active_tags->len; i++) {
                GtkTextTag *t = g_ptr_array_index(active_tags, i);
                gtk_text_buffer_apply_tag(buffer, t, &start, &end);
            }
        }
        gtk_text_buffer_delete_mark(buffer, mark);
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT) return;

    GumboTag tag = node->v.element.tag;

    if (tag == GUMBO_TAG_SCRIPT || tag == GUMBO_TAG_STYLE) return;

    /* この要素自身のタグがあれば、active_tags に追加 */
    GtkTextTag *self_tag = NULL;
    if (g_node_tags != NULL) {
        self_tag = g_hash_table_lookup(g_node_tags, node);
        if (self_tag != NULL) {
            g_ptr_array_add(active_tags, self_tag);
        }
    }

    /* 【小段落 8-1】FORM 要素のコンテキスト管理 */
    FormContext *parent_form = g_current_form;
    if (tag == GUMBO_TAG_FORM) {
        GumboAttribute *action_attr = gumbo_get_attribute(&node->v.element.attributes, "action");
        GumboAttribute *method_attr = gumbo_get_attribute(&node->v.element.attributes, "method");

        FormContext *fc = g_new0(FormContext, 1);
        fc->action = resolve_url(g_base_url, (action_attr && action_attr->value) ? action_attr->value : g_base_url);
        fc->method = g_ascii_strdown((method_attr && method_attr->value) ? method_attr->value : "get", -1);
        fc->input_tags = NULL;

        g_current_form = fc;
    }

    /* 【小段落 8-1.1】リンク要素 */
    if (tag == GUMBO_TAG_A) {
        GumboAttribute *href = gumbo_get_attribute(&node->v.element.attributes, "href");
        if (href && href->value) {
            char *abs_url = resolve_url(g_base_url, href->value);
            if (abs_url) {
                GtkTextIter start_iter;
                gtk_text_buffer_get_end_iter(buffer, &start_iter);
                GtkTextMark *start_mark = gtk_text_buffer_create_mark(buffer, NULL, &start_iter, TRUE);

                GumboVector *children = &node->v.element.children;
                for (unsigned int i = 0; i < children->length; ++i) {
                    render_gumbo_node(children->data[i], buffer, active_tags);
                }

                GtkTextIter start, end;
                gtk_text_buffer_get_iter_at_mark(buffer, &start, start_mark);
                gtk_text_buffer_get_end_iter(buffer, &end);

                GtkTextTag *link_tag = create_link_tag(buffer, abs_url);
                gtk_text_buffer_apply_tag(buffer, link_tag, &start, &end);

                gtk_text_buffer_delete_mark(buffer, start_mark);
                free(abs_url);

                if (self_tag != NULL) {
                    g_ptr_array_remove_index(active_tags, active_tags->len - 1);
                }
                return;
            }
        }
    }

    /* 【小段落 8-1.2】画像要素（<img>）のレンダリング処理追加 */
    if (tag == GUMBO_TAG_IMG) {
        GumboAttribute *src_attr = gumbo_get_attribute(&node->v.element.attributes, "src");
        GumboAttribute *alt_attr = gumbo_get_attribute(&node->v.element.attributes, "alt");

        if (src_attr && src_attr->value) {
            char *img_url = resolve_url(g_base_url, src_attr->value);
            if (img_url) {
                GdkPixbuf *pix = download_pixbuf(img_url);
                if (pix) {
                    GtkTextIter end;
                    gtk_text_buffer_get_end_iter(buffer, &end);
                    /* テキストバッファの途中にインラインで画像を叩き込む */
                    gtk_text_buffer_insert_pixbuf(buffer, &end, pix);
                    g_object_unref(pix);
                } else {
                    /* 画像の取得に失敗した場合は [画像: alt] と表示 */
                    const char *alt = (alt_attr && alt_attr->value) ? alt_attr->value : "Image";
                    GtkTextIter end;
                    gtk_text_buffer_get_end_iter(buffer, &end);
                    char alt_buf[128];
                    snprintf(alt_buf, sizeof(alt_buf), "[🖼️ %s]", alt);
                    gtk_text_buffer_insert(buffer, &end, alt_buf, -1);
                }
                free(img_url);
            }
        }

        if (self_tag != NULL) {
            g_ptr_array_remove_index(active_tags, active_tags->len - 1);
        }
        return;
    }

    /* 【小段落 8-1.5】フォーム要素処理 */
    if (tag == GUMBO_TAG_INPUT) {
        GumboAttribute *type_attr = gumbo_get_attribute(&node->v.element.attributes, "type");
        GumboAttribute *name_attr = gumbo_get_attribute(&node->v.element.attributes, "name");
        GumboAttribute *value_attr = gumbo_get_attribute(&node->v.element.attributes, "value");
        GumboAttribute *placeholder = gumbo_get_attribute(&node->v.element.attributes, "placeholder");

        const char *type = (type_attr && type_attr->value) ? type_attr->value : "text";
        const char *name = (name_attr && name_attr->value) ? name_attr->value : "";
        const char *value = (value_attr && value_attr->value) ? value_attr->value : "";
        const char *ph = (placeholder && placeholder->value) ? placeholder->value : "";

        GString *buf = g_string_new(NULL);
        GtkTextTag *input_tag = NULL;

        if (strcmp(type, "submit") == 0 || strcmp(type, "image") == 0) {
            const char *label = (*value) ? value : "Submit";
            g_string_append_printf(buf, "[ %s ]", label);

            input_tag = gtk_text_buffer_create_tag(buffer, NULL,
                "background", "#d0e0ff",
                "foreground", "black",
                NULL);
            g_object_set_data(G_OBJECT(input_tag), "is_submit", GINT_TO_POINTER(1));
            g_object_set_data_full(G_OBJECT(input_tag), "input_name", g_strdup(name), g_free);
            g_object_set_data_full(G_OBJECT(input_tag), "input_value", g_strdup(value), g_free);
            if (g_current_form) {
                g_object_set_data_full(G_OBJECT(input_tag), "form_action",
                                       g_strdup(g_current_form->action), g_free);
                g_object_set_data_full(G_OBJECT(input_tag), "form_method",
                                       g_strdup(g_current_form->method), g_free);
                g_object_set_data(G_OBJECT(input_tag), "form_inputs",
                                  g_slist_copy(g_current_form->input_tags));
            }
        } else if (strcmp(type, "button") == 0 || strcmp(type, "reset") == 0) {
            const char *label = (*value) ? value : "Button";
            g_string_append_printf(buf, "[ %s ]", label);

            input_tag = gtk_text_buffer_create_tag(buffer, NULL,
                "background", "#d0e0ff",
                "foreground", "black",
                NULL);
            g_object_set_data_full(G_OBJECT(input_tag), "input_name", g_strdup(name), g_free);
            g_object_set_data_full(G_OBJECT(input_tag), "input_value", g_strdup(value), g_free);
        } else if (strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0) {
            GumboAttribute *checked_attr = gumbo_get_attribute(&node->v.element.attributes, "checked");
            gboolean is_checked = (checked_attr != NULL);
            g_string_append_printf(buf, "%s", is_checked ? "[•]" : "[ ]");

            input_tag = gtk_text_buffer_create_tag(buffer, NULL,
                "background", "#e0e0e0",
                "foreground", "black",
                NULL);
            g_object_set_data(G_OBJECT(input_tag), "is_toggle", GINT_TO_POINTER(1));
            g_object_set_data(G_OBJECT(input_tag), "is_checked",
                              GINT_TO_POINTER(is_checked ? 1 : 0));
            g_object_set_data_full(G_OBJECT(input_tag), "input_name", g_strdup(name), g_free);
            g_object_set_data_full(G_OBJECT(input_tag), "input_value", g_strdup(value), g_free);
            if (g_current_form) {
                g_current_form->input_tags = g_slist_append(g_current_form->input_tags, input_tag);
            }
        } else if (strcmp(type, "hidden") == 0) {
            /* 画面描画はしない。ただし送信用にタグを登録 */
            if (g_current_form && *name) {
                GtkTextTag *hidden_tag = gtk_text_buffer_create_tag(buffer, NULL, NULL);
                g_object_set_data_full(G_OBJECT(hidden_tag), "input_name", g_strdup(name), g_free);
                g_object_set_data_full(G_OBJECT(hidden_tag), "input_value", g_strdup(value), g_free);
                g_current_form->input_tags = g_slist_append(g_current_form->input_tags, hidden_tag);
            }
            g_string_free(buf, TRUE);
            if (self_tag != NULL) {
                g_ptr_array_remove_index(active_tags, active_tags->len - 1);
            }
            return;
        } else {
            /* text / search / email / password / url など */
            const char *shown = (*value) ? value : (*ph ? ph : "");
            g_string_append_printf(buf, "[ %s ]", shown);

            input_tag = gtk_text_buffer_create_tag(buffer, NULL,
                "background", "#e0e0e0",
                "foreground", "black",
                NULL);
            g_object_set_data(G_OBJECT(input_tag), "is_input", GINT_TO_POINTER(1));
            g_object_set_data_full(G_OBJECT(input_tag), "input_name", g_strdup(name), g_free);
            g_object_set_data_full(G_OBJECT(input_tag), "input_value", g_strdup(value), g_free);
            if (g_current_form) {
                g_current_form->input_tags = g_slist_append(g_current_form->input_tags, input_tag);
            }
        }

        if (buf->len > 0 && input_tag != NULL) {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer, &end);
            GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &end, TRUE);
            gtk_text_buffer_insert(buffer, &end, buf->str, -1);

            GtkTextIter s, e;
            gtk_text_buffer_get_iter_at_mark(buffer, &s, mark);
            gtk_text_buffer_get_end_iter(buffer, &e);
            gtk_text_buffer_apply_tag(buffer, input_tag, &s, &e);

            if (active_tags->len > 0) {
                for (guint i = 0; i < active_tags->len; i++) {
                    GtkTextTag *t = g_ptr_array_index(active_tags, i);
                    gtk_text_buffer_apply_tag(buffer, t, &s, &e);
                }
            }
            gtk_text_buffer_delete_mark(buffer, mark);
        }
        g_string_free(buf, TRUE);

        if (self_tag != NULL) {
            g_ptr_array_remove_index(active_tags, active_tags->len - 1);
        }
        return;
    }
    /* 【小段落 8-2】ブロック要素の前で改行 */
    if (tag == GUMBO_TAG_P || tag == GUMBO_TAG_DIV ||
        tag == GUMBO_TAG_H1 || tag == GUMBO_TAG_H2 || tag == GUMBO_TAG_H3 ||
        tag == GUMBO_TAG_LI || tag == GUMBO_TAG_BR) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_insert(buffer, &end, "\n", -1);
    }

    /* 【小段落 8-3】子ノードを再帰的に処理 */
    GumboVector *children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; ++i) {
        render_gumbo_node(children->data[i], buffer, active_tags);
    }

    /* フォームタグを抜け出したらコンテキストをクリア */
    if (tag == GUMBO_TAG_FORM && g_current_form) {
        g_free(g_current_form->action);
        g_free(g_current_form->method);
        g_slist_free(g_current_form->input_tags);
        g_free(g_current_form);
        g_current_form = parent_form;
    }

    if (self_tag != NULL) {
        g_ptr_array_remove_index(active_tags, active_tags->len - 1);
    }
}

/* === 追加ここから === */
/* ============================================================
 * [8.5] テキスト入力ダイアログ
 * ============================================================ */
static char *show_input_dialog(GtkWindow *parent, const char *title, const char *initial) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        title,
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK", GTK_RESPONSE_OK,
        "_Cancel", GTK_RESPONSE_CANCEL,
        NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();
    if (initial) {
        gtk_entry_set_text(GTK_ENTRY(entry), initial);
    }
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show_all(dialog);

    char *result = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        result = g_strdup(text);
    }
    gtk_widget_destroy(dialog);
    return result;
}
/* === 追加ここまで === */

/* ============================================================
 * [9] テキストビューのクリック処理
 * ============================================================ */
static gboolean on_text_view_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)user_data;

    /* 左ボタン以外は無視 */
    if (event->button != 1) return FALSE;

    /* ダブルクリック（GDK_2BUTTON_PRESS）のみ動作させる */
    if (event->type != GDK_2BUTTON_PRESS) {
        return FALSE;
    }

    GtkTextView *view = GTK_TEXT_VIEW(widget);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);

    GtkTextIter iter;
    int x, y;
    gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_WIDGET,
                                          (int)event->x, (int)event->y, &x, &y);
    gtk_text_view_get_iter_at_location(view, &iter, x, y);

    GSList *tags = gtk_text_iter_get_tags(&iter);
    for (GSList *l = tags; l != NULL; l = l->next) {
        GtkTextTag *tag = GTK_TEXT_TAG(l->data);

        /* 1. 送信ボタン（[ Submit ]）がダブルクリックされた場合 -> フォーム送信 */
        if (g_object_get_data(G_OBJECT(tag), "is_submit")) {
            const char *action = g_object_get_data(G_OBJECT(tag), "form_action");
            const char *method = g_object_get_data(G_OBJECT(tag), "form_method");
            GSList *inputs = g_object_get_data(G_OBJECT(tag), "form_inputs");

            if (!action) action = g_base_url;

            /* クエリパラメータ／POSTボディの生成 */
            GString *query = g_string_new(NULL);
            for (GSList *inp = inputs; inp != NULL; inp = inp->next) {
                GtkTextTag *itag = GTK_TEXT_TAG(inp->data);
                const char *iname = g_object_get_data(G_OBJECT(itag), "input_name");
                const char *ival = g_object_get_data(G_OBJECT(itag), "input_value");

                if (iname && *iname) {
                    char *esc_name = g_uri_escape_string(iname, NULL, TRUE);
                    char *esc_val = g_uri_escape_string(ival ? ival : "", NULL, TRUE);

                    if (query->len > 0) g_string_append_c(query, '&');
                    g_string_append_printf(query, "%s=%s", esc_name, esc_val);

                    g_free(esc_name);
                    g_free(esc_val);
                }
            }

            if (method && g_ascii_strcasecmp(method, "post") == 0) {
                g_print("Form Submit (POST): %s [body: %s]\n", action, query->str);
                /* ※実際のPOST送信時はcurl等のオプション拡張が必要。ここではURL遷移として処理 */
                load_url(action, TRUE);
            } else {
                /* GET送信 */
                GString *target_url = g_string_new(action);
                if (query->len > 0) {
                    g_string_append_c(target_url, strchr(action, '?') ? '&' : '?');
                    g_string_append(target_url, query->str);
                }
                g_print("Form Submit (GET): %s\n", target_url->str);
                load_url(target_url->str, TRUE);
                g_string_free(target_url, TRUE);
            }

            g_string_free(query, TRUE);
            g_slist_free(tags);
            return TRUE;
        }

        /* 2. テキストボックスがダブルクリックされた場合 -> 入力ダイアログを表示して書き換え */
        if (g_object_get_data(G_OBJECT(tag), "is_input")) {
            const char *old_val = g_object_get_data(G_OBJECT(tag), "input_value");
            GtkWindow *top = GTK_WINDOW(gtk_widget_get_toplevel(widget));
            char *new_val = show_input_dialog(top, "テキスト入力", old_val);

            if (new_val) {
                /* タグが適用されている範囲（[ ... ]）を特定して表示を差し替える */
                GtkTextIter start = iter, end = iter;
                if (!gtk_text_iter_starts_tag(&start, tag)) {
                    gtk_text_iter_backward_to_tag_toggle(&start, tag);
                }
                if (!gtk_text_iter_ends_tag(&end, tag)) {
                    gtk_text_iter_forward_to_tag_toggle(&end, tag);
                }

                gtk_text_buffer_delete(buffer, &start, &end);

                char new_text[256];
                snprintf(new_text, sizeof(new_text), "[ %s ]", new_val);
                gtk_text_buffer_insert_with_tags(buffer, &start, new_text, -1, tag, NULL);

                /* 値を更新して保存 */
                g_object_set_data_full(G_OBJECT(tag), "input_value", new_val, g_free);
            }
            g_slist_free(tags);
            return TRUE;
        }

        /* 3. リンク要素がダブルクリックされた場合 -> URL読み込み */
        const char *url = g_object_get_data(G_OBJECT(tag), "url");
        if (url) {
            g_print("Link double-clicked: %s\n", url);
            load_url(url, TRUE);
            g_slist_free(tags);
            return TRUE;
        }
    }
    g_slist_free(tags);
    return FALSE;
}

/* ============================================================
 * [10] ナビゲーションボタンの有効/無効
 * ============================================================ */
static void update_nav_buttons(void) {
    gtk_widget_set_sensitive(g_app.back_button,    g_back_stack    != NULL);
    gtk_widget_set_sensitive(g_app.forward_button, g_forward_stack != NULL);
    gtk_widget_set_sensitive(g_app.reload_button,  g_current_url   != NULL);
}

/* ============================================================
 * [11] 履歴スタックのクリア
 * ============================================================ */
static void clear_stack(GList **stack) {
    for (GList *l = *stack; l != NULL; l = l->next) {
        g_free(l->data);
    }
    g_list_free(*stack);
    *stack = NULL;
}

/* ============================================================
 * [12] URL 読み込み本体
 * ============================================================ */
static void load_url(const char *url, gboolean add_to_history) {
    if (!url || !*url) return;

    g_print("Fetching: %s\n", url);
    char *html = NULL;

    /* file:// は libcurl を通さず直接読む */
    if (strncmp(url, "file://", 7) == 0) {
        html = read_local_file(url + 7);  /* "file://" の後ろがパス */
    } else {
        html = fetch_url(url);
    }

    if (!html) {
        g_print("Failed to fetch.\n");
        return;
    }

    GumboOutput *output = gumbo_parse(html);
    if (!output) {
        free(html);
        return;
    }

    /* 【小段落 12-1】ベース URL を先に更新 */
    strncpy(g_base_url, url, sizeof(g_base_url) - 1);
    g_base_url[sizeof(g_base_url) - 1] = '\0';

    /* 【小段落 12-2】CSS 収集 */
    GString *css_text = g_string_new(NULL);
    collect_inline_styles(output->root, css_text);

    /* 【小段落 12-2.5】CSS サニタイズ */
    if (css_text->len > 0) {
        GString *clean = g_string_new(NULL);
        strip_at_rule_blocks(css_text->str, "@media", clean);
        g_string_free(css_text, TRUE);
        css_text = clean;

        clean = g_string_new(NULL);
        strip_at_rule_blocks(css_text->str, "@-webkit-keyframes", clean);
        g_string_free(css_text, TRUE);
        css_text = clean;

        clean = g_string_new(NULL);
        strip_at_rule_blocks(css_text->str, "@keyframes", clean);
        g_string_free(css_text, TRUE);
        css_text = clean;

        g_print("CSS after sanitize: %zu bytes\n", css_text->len);
    }

    /* 【小段落 12-3】libcss でパース */
    css_stylesheet *sheet = NULL;
    if (css_text->len > 0) {
        sheet = create_stylesheet_from_css(css_text->str);
    }
    g_string_free(css_text, TRUE);

    /* === 追加ここから === */
    /* 【小段落 12-4】セレクタコンテキスト作成 → スタイル計算 */
    if (sheet) {
        css_select_ctx *select_ctx = NULL;
        css_error err = css_select_ctx_create(&select_ctx);
        if (err == CSS_OK && select_ctx != NULL) {
            err = css_select_ctx_append_sheet(select_ctx, sheet,
                                              CSS_ORIGIN_AUTHOR, "screen");
            if (err == CSS_OK) {
                /* css_unit_ctx は measure が const なので、
                   宣言時に初期化子で全フィールドを指定する必要がある */
                css_unit_ctx unit_ctx = {
                    .viewport_width    = 800 * (1 << 10),
                    .viewport_height   = 600 * (1 << 10),
                    .font_size_default = 16 * (1 << 10),
                    .font_size_minimum = 0,
                    .device_dpi        = 96 * (1 << 10),
                    .root_style        = NULL,
                    .measure           = NULL,
                };

                css_media media;
                memset(&media, 0, sizeof(media));
                media.type   = CSS_MEDIA_SCREEN;
                media.width  = 800 * (1 << 10);
                media.height = 600 * (1 << 10);
                media.color  = 8;

                /* ノード→タグのハッシュを初期化 */
                g_node_tags = g_hash_table_new(g_direct_hash, g_direct_equal);

                g_print("Computing styles...\n");
                compute_styles_for_tree(output->root, select_ctx,
                                        &unit_ctx, &media,
                                        gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app.text_view)));
                g_print("Style computation done.\n");
            } else {
                g_print("css_select_ctx_append_sheet failed: %d (%s)\n",
                        err, css_error_to_string(err));
            }
            css_select_ctx_destroy(select_ctx);
        } else {
            g_print("css_select_ctx_create failed: %d (%s)\n",
                    err, css_error_to_string(err));
        }
    }
    /* === 追加ここまで === */

    /* 履歴更新 */
    if (add_to_history) {
        if (g_current_url) {
            g_back_stack = g_list_append(g_back_stack, g_strdup(g_current_url));
        }
        clear_stack(&g_forward_stack);
    }

    if (g_current_url) g_free(g_current_url);
    g_current_url = g_strdup(url);

    /* アドレスバー更新 */
    gtk_entry_set_text(GTK_ENTRY(g_app.entry), url);

    /* 描画 */
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_app.text_view));
    gtk_text_buffer_set_text(buffer, "", -1);

    GPtrArray *active_tags = g_ptr_array_new();
    render_gumbo_node(output->root, buffer, active_tags);
    g_ptr_array_free(active_tags, TRUE);

    /* ハッシュを破棄（タグはバッファが所有権を持つので unref 不要） */
    if (g_node_tags != NULL) {
        g_hash_table_destroy(g_node_tags);
        g_node_tags = NULL;
    }

    /* クリーンアップ */
    if (sheet) {
        css_stylesheet_destroy(sheet);
    }
    gumbo_destroy_output(&kGumboDefaultOptions, output);
    free(html);

    update_nav_buttons();
}


/* ============================================================
 * [13] 戻る
 * ============================================================ */
static void on_back_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget; (void)user_data;
    if (!g_back_stack) return;

    GList *last = g_list_last(g_back_stack);
    char *url = (char *)last->data;
    g_back_stack = g_list_delete_link(g_back_stack, last);

    if (g_current_url) {
        g_forward_stack = g_list_append(g_forward_stack, g_strdup(g_current_url));
    }

    load_url(url, FALSE);
    g_free(url);
}

/* ============================================================
 * [14] 進む
 * ============================================================ */
static void on_forward_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget; (void)user_data;
    if (!g_forward_stack) return;

    GList *last = g_list_last(g_forward_stack);
    char *url = (char *)last->data;
    g_forward_stack = g_list_delete_link(g_forward_stack, last);

    if (g_current_url) {
        g_back_stack = g_list_append(g_back_stack, g_strdup(g_current_url));
    }

    load_url(url, FALSE);
    g_free(url);
}

/* ============================================================
 * [15] 再読み込み
 * ============================================================ */
static void on_reload_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget; (void)user_data;
    if (!g_current_url) return;
    load_url(g_current_url, FALSE);
}

/* ============================================================
 * [16] Go ボタン／Enter
 * ============================================================ */
static void on_load_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    const char *url = gtk_entry_get_text(GTK_ENTRY(g_app.entry));
    if (strlen(url) == 0) return;

    char normalized[2048];
    if (strstr(url, "://") == NULL) {
        snprintf(normalized, sizeof(normalized), "http://%s", url);
    } else {
        strncpy(normalized, url, sizeof(normalized) - 1);
        normalized[sizeof(normalized) - 1] = '\0';
    }
    load_url(normalized, TRUE);
}

/* === 追加ここから === */
/* ============================================================
 * [16.5] Search ボタン（検索バー用）
 * ============================================================ */

/* 検索クエリを URL エンコードして Google 検索 URL を組み立てる */
static char *build_search_url(const char *query) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char *escaped = curl_easy_escape(curl, query, 0);
    if (!escaped) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    const char *base = "https://lite.duckduckgo.com/lite/?q=";
    size_t len = strlen(base) + strlen(escaped) + 1;
    char *url = malloc(len);
    if (url) {
        snprintf(url, len, "%s%s", base, escaped);
    }

    curl_free(escaped);
    curl_easy_cleanup(curl);
    return url;
}

static void on_search_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    const char *query = gtk_entry_get_text(GTK_ENTRY(g_app.search_entry));
    if (strlen(query) == 0) return;

    char *search_url = build_search_url(query);
    if (search_url) {
        g_print("Search: %s\n", search_url);
        load_url(search_url, TRUE);
        free(search_url);
    } else {
        g_print("Failed to build search URL\n");
    }
}
/* === 追加ここまで === */

/* === 追加ここから === */
/* === 追加ここから === */
/* ============================================================
 * [16.6] メニュー、About ダイアログ、ライセンスダイアログ
 * ============================================================ */

/* About ダイアログを表示 */
static void show_about_dialog(GtkWidget *parent) {
    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "C-Browser");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), "1.1.5");
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), "Copyright: 2026 NOK.T");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
        "C 言語 + GTK + libcurl + gumbo-parser + libcss で作られた\n"
        "シンプルなテキストベースブラウザ");
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
        GTK_LICENSE_GPL_3_0);

    /* 独自アイコンを読み込んで縮小する */
    GError *err = NULL;
    GdkPixbuf *logo_orig = gdk_pixbuf_new_from_file("icon.png", &err);
    if (logo_orig != NULL) {
        int w = gdk_pixbuf_get_width(logo_orig);
        int h = gdk_pixbuf_get_height(logo_orig);

        int target = 128;
        int new_w, new_h;
        if (w > h) {
            new_w = target;
            new_h = (int)((double)h * target / w);
        } else {
            new_h = target;
            new_w = (int)((double)w * target / h);
        }

        GdkPixbuf *logo = gdk_pixbuf_scale_simple(logo_orig,
                                                   new_w, new_h,
                                                   GDK_INTERP_BILINEAR);
        if (logo != NULL) {
            gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), logo);
            g_object_unref(logo);
        }
        g_object_unref(logo_orig);
    } else {
        gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), "web-browser");
        if (err) {
            g_print("Failed to load icon.png: %s\n", err->message);
            g_error_free(err);
        }
    }

    if (parent != NULL) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));
        gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    }

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* ライセンスダイアログを表示 */
static void show_license_dialog(GtkWidget *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "ライセンスについて",
        parent ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_閉じる", GTK_RESPONSE_CLOSE,
        NULL);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 640, 520);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 6);

    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view), 12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text_view), 12);

    const char *license_text =
        "Copyright (C) 2026 NOK.T\n"
        "\n"
        "This program is free software: you can redistribute it and/or modify\n"
        "it under the terms of the GNU General Public License as published by\n"
        "the Free Software Foundation, either version 3 of the License, or\n"
        "(at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program.  If not, see <https://www.gnu.org/licenses/>.\n";

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    gtk_text_buffer_set_text(buffer, license_text, -1);

    gtk_container_add(GTK_CONTAINER(scroll), text_view);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* === 追加ここから === */
/* 謝辞ダイアログ（サードパーティライブラリのライセンス） */
static void show_credits_dialog(GtkWidget *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "謝辞",
        parent ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_閉じる", GTK_RESPONSE_CLOSE,
        NULL);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 680, 560);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 6);

    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view), 12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text_view), 12);

    const char *credits_text =
        "C-Browser は以下のオープンソースソフトウェアを利用しています。\n"
        "各ライブラリの著作権は、それぞれの作者に帰属します。\n"
        "\n"
        "────────────────────────────────────────────────\n"
        "\n"
        "■ GTK+ 3\n"
        "  https://www.gtk.org/\n"
        "  License: GNU Lesser General Public License v2.1 (LGPL-2.1)\n"
        "  Copyright (C) The GTK Team\n"
        "\n"
        "■ GLib\n"
        "  https://wiki.gnome.org/Projects/GLib\n"
        "  License: GNU Lesser General Public License v2.1 (LGPL-2.1)\n"
        "  Copyright (C) The GLib Team\n"
        "\n"
        "■ libcurl\n"
        "  https://curl.se/libcurl/\n"
        "  License: curl License (MIT/X derivate)\n"
        "  Copyright (C) Daniel Stenberg and contributors\n"
        "\n"
        "■ gumbo-parser\n"
        "  https://github.com/google/gumbo-parser\n"
        "  License: Apache License 2.0\n"
        "  Copyright (C) Google Inc.\n"
        "\n"
        "■ libcss\n"
        "  https://www.netsurf-browser.org/projects/libcss/\n"
        "  License: MIT License\n"
        "  Copyright (C) 2007-2024 John-Mark Bell and contributors\n"
        "\n"
        "■ libdom\n"
        "  https://www.netsurf-browser.org/projects/libdom/\n"
        "  License: MIT License\n"
        "  Copyright (C) 2007-2024 John-Mark Bell and contributors\n"
        "\n"
        "■ libwapcaplet / libparserutils / libhubbub\n"
        "  https://www.netsurf-browser.org/projects/\n"
        "  License: MIT License\n"
        "  Copyright (C) 2007-2024 John-Mark Bell and contributors\n"
        "\n"
        "────────────────────────────────────────────────\n"
        "\n"
        "ご協力いただいたすべての開発者に感謝します。\n"
        "\n"
        "詳細なライセンス全文は、各プロジェクトのリポジトリを\n"
        "ご参照ください。\n";

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    gtk_text_buffer_set_text(buffer, credits_text, -1);

    gtk_container_add(GTK_CONTAINER(scroll), text_view);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}
/* === 追加ここまで === */

/* メニュー項目「このブラウザについて」 */
static void on_menu_about_activate(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);
    show_about_dialog(window);
}

/* メニュー項目「ライセンスについて」 */
static void on_menu_license_activate(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);
    show_license_dialog(window);
}

/* === 追加ここから === */
/* メニュー項目「謝辞」 */
static void on_menu_credits_activate(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);
    show_credits_dialog(window);
}
/* === 追加ここまで === */

/* [...] ボタンが押されたとき：ポップアップメニューを表示 */
static void on_menu_button_clicked(GtkButton *button, gpointer user_data) {
    GtkWidget *window = GTK_WIDGET(user_data);

    GtkWidget *menu = gtk_menu_new();

    GtkWidget *item_about = gtk_menu_item_new_with_label("このブラウザについて");
    g_signal_connect(item_about, "activate",
                     G_CALLBACK(on_menu_about_activate), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_about);

    GtkWidget *item_license = gtk_menu_item_new_with_label("ライセンスについて");
    g_signal_connect(item_license, "activate",
                     G_CALLBACK(on_menu_license_activate), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_license);

    GtkWidget *item_credits = gtk_menu_item_new_with_label("謝辞");
    g_signal_connect(item_credits, "activate",
                     G_CALLBACK(on_menu_credits_activate), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_credits);

    GtkWidget *item_sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_sep);

    GtkWidget *item_quit = gtk_menu_item_new_with_label("終了");
    g_signal_connect_swapped(item_quit, "activate",
                             G_CALLBACK(gtk_widget_destroy), window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_quit);

    gtk_widget_show_all(menu);

    gtk_menu_popup_at_widget(GTK_MENU(menu),
                             GTK_WIDGET(button),
                             GDK_GRAVITY_SOUTH_WEST,
                             GDK_GRAVITY_NORTH_WEST,
                             NULL);
}
/* === 追加ここまで === */

/* ============================================================
 * [17] メイン
 * ============================================================ */
int main(int argc, char *argv[]) {
    curl_global_init(CURL_GLOBAL_ALL);
    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "C-Browser");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    /* ウィンドウアイコンを設定 */
    GError *icon_err = NULL;
    gtk_window_set_icon_from_file(GTK_WINDOW(window), "icon.png", &icon_err);
    if (icon_err) {
        g_print("Failed to set window icon: %s\n", icon_err->message);
        g_error_free(icon_err);
    }

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    /* 【小段落 17-1】ナビゲーションボタン */
    GtkWidget *menu_button    = gtk_button_new_with_label("...");
    GtkWidget *back_button    = gtk_button_new_with_label("←");
    GtkWidget *forward_button = gtk_button_new_with_label("→");
    GtkWidget *reload_button  = gtk_button_new_with_label("⟳");
    gtk_box_pack_start(GTK_BOX(hbox), menu_button,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), back_button,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), reload_button,  FALSE, FALSE, 0);

    /* 【小段落 17-2】アドレスバーと Go ボタン */
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), "http://example.com");
    gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);

    GtkWidget *button = gtk_button_new_with_label("Go");
    gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);

    /* 【小段落 17-2b】検索バーと Search ボタン */
    GtkWidget *search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "検索...");
    gtk_entry_set_width_chars(GTK_ENTRY(search_entry), 20);
    gtk_box_pack_start(GTK_BOX(hbox), search_entry, FALSE, FALSE, 0);

    GtkWidget *search_button = gtk_button_new_with_label("Search");
    gtk_box_pack_start(GTK_BOX(hbox), search_button, FALSE, FALSE, 0);

    /* 【小段落 17-3】コンテンツ表示領域 */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_events(text_view, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(scroll), text_view);

    /* 【小段落 17-4】グローバルに保存 */
    g_app.entry = entry;
    g_app.search_entry = search_entry;
    g_app.text_view = text_view;
    g_app.back_button = back_button;
    g_app.forward_button = forward_button;
    g_app.reload_button = reload_button;

    /* 【小段落 17-5】シグナル接続 */
    g_signal_connect(button, "clicked", G_CALLBACK(on_load_clicked), NULL);
    g_signal_connect(entry, "activate", G_CALLBACK(on_load_clicked), NULL);
    g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_clicked), NULL);
    g_signal_connect(search_entry, "activate", G_CALLBACK(on_search_clicked), NULL);
    g_signal_connect(text_view, "button-press-event",
                     G_CALLBACK(on_text_view_button_press), NULL);
    g_signal_connect(menu_button,    "clicked", G_CALLBACK(on_menu_button_clicked), window);
    g_signal_connect(back_button,    "clicked", G_CALLBACK(on_back_clicked), NULL);
    g_signal_connect(forward_button, "clicked", G_CALLBACK(on_forward_clicked), NULL);
    g_signal_connect(reload_button,  "clicked", G_CALLBACK(on_reload_clicked), NULL);

    gtk_widget_set_sensitive(back_button, FALSE);
    gtk_widget_set_sensitive(forward_button, FALSE);
    gtk_widget_set_sensitive(reload_button, FALSE);

    gtk_widget_show_all(window);
    gtk_main();

    /* 【小段落 17-6】後始末 */
    clear_stack(&g_back_stack);
    clear_stack(&g_forward_stack);
    if (g_current_url) g_free(g_current_url);
    curl_global_cleanup();
    return 0;
}