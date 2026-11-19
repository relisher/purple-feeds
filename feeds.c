/*
 * purple-feeds: RSS and Atom feeds as buddies for Pidgin and other libpurple
 * clients. Each feed is a buddy whose name is the feed URL; new articles
 * arrive as messages from that buddy.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#define PURPLE_PLUGINS

#include <errno.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "account.h"
#include "accountopt.h"
#include "blist.h"
#include "connection.h"
#include "debug.h"
#include "plugin.h"
#include "prpl.h"
#include "request.h"
#include "server.h"
#include "util.h"
#include "version.h"
#include "xmlnode.h"

#define FEEDS_PLUGIN_ID "prpl-relisher-feeds"
#define FEEDS_VERSION "0.1.0"
#define FEEDS_USER_AGENT "Mozilla/5.0 (compatible; purple-feeds/" FEEDS_VERSION ")"

#define OPT_INTERVAL "check_interval"
#define OPT_INITIAL_ITEMS "initial_items"
#define BUDDY_LINK_FILTER "feeds-link-filter"
#define BUDDY_LAST_ERROR "feeds-last-error"

#define DEFAULT_INTERVAL_MINUTES 10
#define DEFAULT_INITIAL_ITEMS 5
#define MAX_FEED_BYTES (8 * 1024 * 1024)
/* Enough to remember every item a feed still lists, plus some it dropped. */
#define MAX_SEEN_IDS 1000

typedef struct {
    PurpleConnection *gc;
    guint timer;
    GSList *fetches;
} FeedsConnection;

typedef struct {
    FeedsConnection *fc;
    gchar *url;
    PurpleUtilFetchUrlData *request;
} FeedFetch;

typedef struct {
    gchar *id;
    gchar *title;
    gchar *link;
    time_t when;
    guint order;
} FeedItem;

static void feeds_fetch(FeedsConnection *fc, const gchar *url);

/* ------------------------------------------------------------------------ */
/* Dates                                                                    */

static const gchar *MONTHS[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Offset in seconds for the zone names RFC 822 allows. */
static gint zone_offset(const gchar *zone)
{
    static const struct { const gchar *name; gint hours; } names[] = {
        {"UT", 0}, {"UTC", 0}, {"GMT", 0}, {"Z", 0},
        {"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5},
        {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7},
    };
    guint i;

    if ((zone[0] == '+' || zone[0] == '-') && strlen(zone) >= 5) {
        gint hhmm = atoi(zone + 1);
        gint seconds = (hhmm / 100) * 3600 + (hhmm % 100) * 60;
        return zone[0] == '-' ? -seconds : seconds;
    }
    for (i = 0; i < G_N_ELEMENTS(names); i++)
        if (g_ascii_strcasecmp(zone, names[i].name) == 0)
            return names[i].hours * 3600;
    return 0;
}

/* Parse "Thu, 19 Nov 2026 15:04:05 GMT" (the weekday is optional). */
static time_t parse_rfc822(const gchar *text)
{
    gchar month[4] = "", zone[8] = "";
    guint day, year, hour, minute, second = 0;
    const gchar *comma = strchr(text, ',');
    GDateTime *utc, *dt;
    time_t result;
    gint m;

    if (comma)
        text = comma + 1;
    if (sscanf(text, " %u %3s %u %u:%u:%u %7s", &day, month, &year, &hour,
               &minute, &second, zone) < 5)
        return 0;
    for (m = 0; m < 12; m++)
        if (g_ascii_strcasecmp(month, MONTHS[m]) == 0)
            break;
    if (m == 12)
        return 0;
    if (year < 100)
        year += year < 70 ? 2000 : 1900;

    utc = g_date_time_new_utc(year, m + 1, day, hour, minute, second);
    if (!utc)
        return 0;
    dt = g_date_time_add_seconds(utc, -zone_offset(zone));
    result = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    g_date_time_unref(utc);
    return result;
}

static time_t parse_iso8601(const gchar *text)
{
    GDateTime *dt = g_date_time_new_from_iso8601(text, NULL);
    time_t result;

    if (!dt)
        return 0;
    result = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return result;
}

/* ------------------------------------------------------------------------ */
/* Parsing                                                                  */

static gchar *child_text(xmlnode *parent, const gchar *name)
{
    xmlnode *child = xmlnode_get_child(parent, name);
    gchar *text = child ? xmlnode_get_data_unescaped(child) : NULL;

    if (text)
        g_strstrip(text);
    if (text && *text == '\0') {
        g_free(text);
        text = NULL;
    }
    return text;
}

static gchar *atom_link(xmlnode *entry)
{
    xmlnode *link;

    for (link = xmlnode_get_child(entry, "link"); link;
         link = xmlnode_get_next_twin(link)) {
        const gchar *rel = xmlnode_get_attrib(link, "rel");
        const gchar *href = xmlnode_get_attrib(link, "href");
        if (href && (!rel || g_str_equal(rel, "alternate")))
            return g_strdup(href);
    }
    return NULL;
}

static void feed_item_free(FeedItem *item)
{
    g_free(item->id);
    g_free(item->title);
    g_free(item->link);
    g_free(item);
}

static FeedItem *feed_item_new(gchar *id, gchar *title, gchar *link,
                               time_t when, guint order)
{
    FeedItem *item = g_new0(FeedItem, 1);

    item->title = title ? title : g_strdup(link ? link : "(untitled)");
    item->link = link;
    item->id = id ? id : g_strdup(link ? link : item->title);
    item->when = when;
    item->order = order;
    return item;
}

/* Return the items of an RSS 2.0, RSS 1.0 or Atom document, and the feed's
 * own title in *feed_title. */
static GList *parse_feed(const gchar *text, gsize len, gchar **feed_title)
{
    xmlnode *root = xmlnode_from_str(text, len);
    xmlnode *channel, *node;
    GList *items = NULL;
    guint order = 0;

    *feed_title = NULL;
    if (!root)
        return NULL;

    if (g_str_equal(root->name, "feed")) {
        *feed_title = child_text(root, "title");
        for (node = xmlnode_get_child(root, "entry"); node;
             node = xmlnode_get_next_twin(node)) {
            gchar *date = child_text(node, "published");
            if (!date)
                date = child_text(node, "updated");
            items = g_list_prepend(items, feed_item_new(
                    child_text(node, "id"), child_text(node, "title"),
                    atom_link(node), date ? parse_iso8601(date) : 0, order++));
            g_free(date);
        }
    } else {
        channel = xmlnode_get_child(root, "channel");
        if (channel)
            *feed_title = child_text(channel, "title");
        /* RSS 2.0 nests items in the channel; RSS 1.0 puts them at the top. */
        node = channel ? xmlnode_get_child(channel, "item") : NULL;
        if (!node)
            node = xmlnode_get_child(root, "item");
        for (; node; node = xmlnode_get_next_twin(node)) {
            gchar *date = child_text(node, "pubDate");
            time_t when = 0;
            if (date)
                when = parse_rfc822(date);
            else if ((date = child_text(node, "date")) != NULL)
                when = parse_iso8601(date);
            items = g_list_prepend(items, feed_item_new(
                    child_text(node, "guid"), child_text(node, "title"),
                    child_text(node, "link"), when, order++));
            g_free(date);
        }
    }

    xmlnode_free(root);
    return g_list_reverse(items);
}

/* Oldest first; undated items keep the feed's newest-first order reversed. */
static gint compare_items(gconstpointer a, gconstpointer b)
{
    const FeedItem *x = a, *y = b;

    if (x->when && y->when && x->when != y->when)
        return x->when < y->when ? -1 : 1;
    return x->order > y->order ? -1 : (x->order < y->order ? 1 : 0);
}

/* ------------------------------------------------------------------------ */
/* Seen items, kept per feed in ~/.purple/feeds/                            */

static gchar *seen_path(PurpleAccount *account, const gchar *url)
{
    gchar *key = g_strdup_printf("%s\n%s", purple_account_get_username(account), url);
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key, -1);
    gchar *path = g_build_filename(purple_user_dir(), "feeds", hash, NULL);

    g_free(hash);
    g_free(key);
    return path;
}

/* NULL when the feed has never been fetched. */
static GHashTable *seen_load(PurpleAccount *account, const gchar *url)
{
    gchar *path = seen_path(account, url);
    gchar *contents = NULL;
    GHashTable *seen = NULL;

    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        gchar **ids = g_strsplit(contents, "\n", -1), **id;
        seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (id = ids; *id; id++)
            if (**id)
                g_hash_table_add(seen, g_strdup(*id));
        g_strfreev(ids);
        g_free(contents);
    }
    g_free(path);
    return seen;
}

/* Store the feed's current ids first, then older ones up to the cap. */
static void seen_save(PurpleAccount *account, const gchar *url, GList *items,
                      GHashTable *old)
{
    gchar *path = seen_path(account, url);
    gchar *dir = g_path_get_dirname(path);
    GHashTable *written = g_hash_table_new(g_str_hash, g_str_equal);
    GString *out = g_string_new(NULL);
    GHashTableIter iter;
    gpointer id;
    GList *l;

    for (l = items; l; l = l->next) {
        FeedItem *item = l->data;
        if (g_hash_table_size(written) < MAX_SEEN_IDS &&
            !g_hash_table_contains(written, item->id) &&
            !strchr(item->id, '\n')) {
            g_hash_table_add(written, item->id);
            g_string_append_printf(out, "%s\n", item->id);
        }
    }
    if (old) {
        g_hash_table_iter_init(&iter, old);
        while (g_hash_table_iter_next(&iter, &id, NULL) &&
               g_hash_table_size(written) < MAX_SEEN_IDS) {
            if (!g_hash_table_contains(written, id)) {
                g_hash_table_add(written, id);
                g_string_append_printf(out, "%s\n", (gchar *)id);
            }
        }
    }

    g_mkdir_with_parents(dir, 0700);
    if (!g_file_set_contents(path, out->str, out->len, NULL))
        purple_debug_error("feeds", "could not write %s\n", path);

    g_string_free(out, TRUE);
    g_hash_table_destroy(written);
    g_free(dir);
    g_free(path);
}

/* ------------------------------------------------------------------------ */
/* Fetching and delivery                                                    */

static gboolean link_allowed(PurpleBuddy *buddy, const FeedItem *item)
{
    const gchar *pattern = purple_blist_node_get_string(
            (PurpleBlistNode *)buddy, BUDDY_LINK_FILTER);
    GRegex *regex;
    gboolean allowed;

    if (!pattern || !*pattern)
        return TRUE;
    regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
    if (!regex)
        return TRUE;
    allowed = item->link && g_regex_match(regex, item->link, 0, NULL);
    g_regex_unref(regex);
    return allowed;
}

static void deliver(PurpleConnection *gc, PurpleBuddy *buddy, const FeedItem *item)
{
    gchar *title = g_markup_escape_text(item->title, -1);
    gchar *message;

    if (item->link) {
        gchar *href = g_markup_escape_text(item->link, -1);
        message = g_strdup_printf("<a href=\"%s\">%s</a>", href, title);
        g_free(href);
    } else {
        message = g_strdup(title);
    }
    serv_got_im(gc, purple_buddy_get_name(buddy), message, PURPLE_MESSAGE_RECV,
                item->when ? item->when : time(NULL));
    g_free(message);
    g_free(title);
}

static void set_feed_status(PurpleAccount *account, PurpleBuddy *buddy,
                            const gchar *error)
{
    purple_blist_node_set_string((PurpleBlistNode *)buddy, BUDDY_LAST_ERROR, error);
    purple_prpl_got_user_status(account, purple_buddy_get_name(buddy),
                                error ? "offline" : "available", NULL);
}

static void handle_feed(FeedsConnection *fc, PurpleBuddy *buddy,
                        const gchar *text, gsize len)
{
    PurpleAccount *account = purple_connection_get_account(fc->gc);
    const gchar *url = purple_buddy_get_name(buddy);
    gchar *feed_title = NULL;
    GList *items = parse_feed(text, len, &feed_title);
    GHashTable *seen = seen_load(account, url);
    GList *fresh = NULL, *l;
    gint limit = purple_account_get_int(account, OPT_INITIAL_ITEMS,
                                        DEFAULT_INITIAL_ITEMS);

    if (!items && !feed_title) {
        set_feed_status(account, buddy, "Not an RSS or Atom feed");
        g_free(feed_title);
        if (seen)
            g_hash_table_destroy(seen);
        return;
    }

    if (feed_title && !purple_buddy_get_server_alias(buddy))
        serv_got_alias(fc->gc, url, feed_title);

    for (l = items; l; l = l->next) {
        FeedItem *item = l->data;
        if (seen && g_hash_table_contains(seen, item->id))
            continue;
        if (!link_allowed(buddy, item))
            continue;
        fresh = g_list_prepend(fresh, item);
    }
    fresh = g_list_sort(fresh, compare_items);

    /* On the first fetch, show only the newest few items. */
    if (!seen) {
        gint skip = (gint)g_list_length(fresh) - limit;
        while (skip-- > 0)
            fresh = g_list_delete_link(fresh, fresh);
    }

    for (l = fresh; l; l = l->next)
        deliver(fc->gc, buddy, l->data);
    purple_debug_info("feeds", "%s: %u items, %u new\n", url,
                      g_list_length(items), g_list_length(fresh));

    seen_save(account, url, items, seen);
    set_feed_status(account, buddy, NULL);

    g_list_free(fresh);
    g_list_free_full(items, (GDestroyNotify)feed_item_free);
    if (seen)
        g_hash_table_destroy(seen);
    g_free(feed_title);
}

static void fetch_done(PurpleUtilFetchUrlData *request, gpointer data,
                       const gchar *text, gsize len, const gchar *error)
{
    FeedFetch *fetch = data;
    FeedsConnection *fc = fetch->fc;
    PurpleAccount *account = purple_connection_get_account(fc->gc);
    PurpleBuddy *buddy = purple_find_buddy(account, fetch->url);

    fc->fetches = g_slist_remove(fc->fetches, fetch);

    if (buddy) {
        if (error) {
            purple_debug_warning("feeds", "%s: %s\n", fetch->url, error);
            set_feed_status(account, buddy, error);
        } else {
            handle_feed(fc, buddy, text, len);
        }
    }

    g_free(fetch->url);
    g_free(fetch);
}

static void feeds_fetch(FeedsConnection *fc, const gchar *url)
{
    PurpleAccount *account = purple_connection_get_account(fc->gc);
    FeedFetch *fetch;
    GSList *l;

    for (l = fc->fetches; l; l = l->next)
        if (g_str_equal(((FeedFetch *)l->data)->url, url))
            return;

    fetch = g_new0(FeedFetch, 1);
    fetch->fc = fc;
    fetch->url = g_strdup(url);
    fetch->request = purple_util_fetch_url_request_len_with_account(
            account, url, TRUE, FEEDS_USER_AGENT, TRUE, NULL, FALSE,
            MAX_FEED_BYTES, fetch_done, fetch);
    if (fetch->request)
        fc->fetches = g_slist_prepend(fc->fetches, fetch);
}

static void feeds_fetch_all(FeedsConnection *fc)
{
    PurpleAccount *account = purple_connection_get_account(fc->gc);
    GSList *buddies = purple_find_buddies(account, NULL), *l;

    for (l = buddies; l; l = l->next)
        feeds_fetch(fc, purple_buddy_get_name(l->data));
    g_slist_free(buddies);
}

static gboolean poll_cb(gpointer data)
{
    feeds_fetch_all(data);
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Protocol callbacks                                                       */

static const char *feeds_list_icon(PurpleAccount *account, PurpleBuddy *buddy)
{
    return "feeds";
}

static GList *feeds_status_types(PurpleAccount *account)
{
    GList *types = NULL;

    types = g_list_append(types, purple_status_type_new_full(
            PURPLE_STATUS_AVAILABLE, "available", NULL, TRUE, TRUE, FALSE));
    types = g_list_append(types, purple_status_type_new_full(
            PURPLE_STATUS_OFFLINE, "offline", NULL, TRUE, TRUE, FALSE));
    return types;
}

static char *feeds_status_text(PurpleBuddy *buddy)
{
    const gchar *error = purple_blist_node_get_string(
            (PurpleBlistNode *)buddy, BUDDY_LAST_ERROR);
    return error ? g_markup_escape_text(error, -1) : NULL;
}

static void feeds_tooltip_text(PurpleBuddy *buddy, PurpleNotifyUserInfo *info,
                               gboolean full)
{
    const gchar *filter = purple_blist_node_get_string(
            (PurpleBlistNode *)buddy, BUDDY_LINK_FILTER);
    const gchar *error = purple_blist_node_get_string(
            (PurpleBlistNode *)buddy, BUDDY_LAST_ERROR);

    purple_notify_user_info_add_pair_plaintext(info, "Feed", purple_buddy_get_name(buddy));
    if (filter && *filter)
        purple_notify_user_info_add_pair_plaintext(info, "Link filter", filter);
    if (error)
        purple_notify_user_info_add_pair_plaintext(info, "Last error", error);
}

static void feeds_login(PurpleAccount *account)
{
    PurpleConnection *gc = purple_account_get_connection(account);
    FeedsConnection *fc = g_new0(FeedsConnection, 1);
    gint minutes = purple_account_get_int(account, OPT_INTERVAL,
                                          DEFAULT_INTERVAL_MINUTES);

    fc->gc = gc;
    purple_connection_set_protocol_data(gc, fc);
    purple_connection_set_state(gc, PURPLE_CONNECTED);

    fc->timer = purple_timeout_add_seconds(MAX(minutes, 1) * 60, poll_cb, fc);
    feeds_fetch_all(fc);
}

static void feeds_close(PurpleConnection *gc)
{
    FeedsConnection *fc = purple_connection_get_protocol_data(gc);

    if (!fc)
        return;
    if (fc->timer)
        purple_timeout_remove(fc->timer);
    while (fc->fetches) {
        FeedFetch *fetch = fc->fetches->data;
        purple_util_fetch_url_cancel(fetch->request);
        fc->fetches = g_slist_delete_link(fc->fetches, fc->fetches);
        g_free(fetch->url);
        g_free(fetch);
    }
    g_free(fc);
    purple_connection_set_protocol_data(gc, NULL);
}

static int feeds_send_im(PurpleConnection *gc, const char *who,
                         const char *message, PurpleMessageFlags flags)
{
    return -ENOTSUP;
}

static void feeds_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
                            PurpleGroup *group)
{
    feeds_fetch(purple_connection_get_protocol_data(gc), purple_buddy_get_name(buddy));
}

static void feeds_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
                               PurpleGroup *group)
{
    gchar *path = seen_path(purple_connection_get_account(gc),
                            purple_buddy_get_name(buddy));
    g_unlink(path);
    g_free(path);
}

static void set_filter_cb(PurpleBuddy *buddy, const char *pattern)
{
    GError *error = NULL;
    GRegex *regex;

    if (pattern && *pattern) {
        regex = g_regex_new(pattern, 0, 0, &error);
        if (!regex) {
            purple_notify_error(NULL, "Feeds", "Invalid link filter", error->message);
            g_error_free(error);
            return;
        }
        g_regex_unref(regex);
    }
    purple_blist_node_set_string((PurpleBlistNode *)buddy, BUDDY_LINK_FILTER,
                                 pattern && *pattern ? pattern : NULL);
}

static void set_filter_action(PurpleBlistNode *node, gpointer data)
{
    PurpleBuddy *buddy = (PurpleBuddy *)node;
    PurpleAccount *account = purple_buddy_get_account(buddy);

    purple_request_input(account, "Link filter",
            "Only show articles whose link matches this regular expression",
            "Leave empty to show every article.",
            purple_blist_node_get_string(node, BUDDY_LINK_FILTER),
            FALSE, FALSE, NULL, "Save", G_CALLBACK(set_filter_cb),
            "Cancel", NULL, account, purple_buddy_get_name(buddy), NULL, buddy);
}

static void check_now_action(PurpleBlistNode *node, gpointer data)
{
    PurpleBuddy *buddy = (PurpleBuddy *)node;
    PurpleConnection *gc = purple_account_get_connection(purple_buddy_get_account(buddy));

    if (gc && purple_connection_get_protocol_data(gc))
        feeds_fetch(purple_connection_get_protocol_data(gc), purple_buddy_get_name(buddy));
}

static GList *feeds_blist_node_menu(PurpleBlistNode *node)
{
    GList *menu = NULL;

    if (!PURPLE_BLIST_NODE_IS_BUDDY(node))
        return NULL;
    menu = g_list_append(menu, purple_menu_action_new(
            "Check now", PURPLE_CALLBACK(check_now_action), NULL, NULL));
    menu = g_list_append(menu, purple_menu_action_new(
            "Set link filter...", PURPLE_CALLBACK(set_filter_action), NULL, NULL));
    return menu;
}

static void check_all_action(PurplePluginAction *action)
{
    PurpleConnection *gc = action->context;

    if (gc && purple_connection_get_protocol_data(gc))
        feeds_fetch_all(purple_connection_get_protocol_data(gc));
}

static GList *feeds_actions(PurplePlugin *plugin, gpointer context)
{
    return g_list_append(NULL, purple_plugin_action_new("Check all feeds now",
                                                        check_all_action));
}

static GHashTable *feeds_account_text_table(PurpleAccount *account)
{
    GHashTable *table = g_hash_table_new(g_str_hash, g_str_equal);

    g_hash_table_insert(table, "login_label", (gpointer)"Account name...");
    return table;
}

static PurplePluginProtocolInfo prpl_info = {
    .options = OPT_PROTO_NO_PASSWORD,
    .icon_spec = NO_BUDDY_ICONS,
    .list_icon = feeds_list_icon,
    .status_text = feeds_status_text,
    .tooltip_text = feeds_tooltip_text,
    .status_types = feeds_status_types,
    .blist_node_menu = feeds_blist_node_menu,
    .login = feeds_login,
    .close = feeds_close,
    .send_im = feeds_send_im,
    .add_buddy = feeds_add_buddy,
    .remove_buddy = feeds_remove_buddy,
    .struct_size = sizeof(PurplePluginProtocolInfo),
    .get_account_text_table = feeds_account_text_table,
};

static void plugin_init(PurplePlugin *plugin)
{
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
            purple_account_option_int_new("Check every (minutes)", OPT_INTERVAL,
                                          DEFAULT_INTERVAL_MINUTES));
    prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
            purple_account_option_int_new("Articles to show from a new feed",
                                          OPT_INITIAL_ITEMS, DEFAULT_INITIAL_ITEMS));
}

static PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_PROTOCOL,
    NULL,
    0,
    NULL,
    PURPLE_PRIORITY_DEFAULT,
    FEEDS_PLUGIN_ID,
    "Feeds",
    FEEDS_VERSION,
    "RSS and Atom feeds as buddies",
    "Adds each RSS or Atom feed as a buddy; new articles arrive as messages.",
    "Anton Relin",
    "https://github.com/relisher/purple-feeds",
    NULL,
    NULL,
    NULL,
    NULL,
    &prpl_info,
    NULL,
    feeds_actions,
    NULL, NULL, NULL, NULL
};

PURPLE_INIT_PLUGIN(feeds, plugin_init, info);
